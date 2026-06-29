// xr-workspace: SpaceWalker-like virtual monitor renderer for VITURE XR glasses
// on bare Wayland/Hyprland. Reads IMU pose from /dev/shm/breezy_desktop_imu
// (written by xrDriver), renders head-locked virtual monitors via wlr-layer-shell
// using EGL/GLES2.

#include <wayland-client.h>
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include "wlr-layer-shell-unstable-v1-client.h"
#include "xdg-output-unstable-v1-client.h"
#include "hyprland-global-shortcuts-v1-client.h"
#include "wlr-screencopy-unstable-v1-client.h"
#include "ext-image-capture-source-v1-client.h"
#include "ext-image-copy-capture-v1-client.h"
#include "linux-dmabuf-unstable-v1-client.h"

#include "linalg.h"
#include "pose.h"
#include "pose_breezy.h"
#include "capture.h"
#include "capture_solid.h"
#include "capture_ctx.h"
#include "capture_factory.h"
#include "egl_dmabuf.h"
#include "hypr_ipc.h"
#include "config.h"
#include "control.h"
#include "sink_opentrack.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>
#include <algorithm>
#include <cerrno>
#include <poll.h>

// ─── Config ───────────────────────────────────────────────────────────────────
static int         g_monitor_count = 3;
static float       g_arc_radius    = 2.0f;
static float       g_fov_deg       = 46.0f;
static std::string g_target_output_name;
static std::string g_config_path; // --config override
static std::string g_resolved_config_path; // actual path loaded (for reload/save)

// ─── Wayland globals ──────────────────────────────────────────────────────────
static struct wl_display    *g_display    = nullptr;
static struct wl_compositor *g_compositor = nullptr;
static struct zwlr_layer_shell_v1 *g_layer_shell = nullptr;
static struct wl_output     *g_xr_output  = nullptr;
static struct hyprland_global_shortcuts_manager_v1 *g_shortcuts_mgr     = nullptr;
static struct hyprland_global_shortcut_v1          *g_shortcut_recenter  = nullptr;

// Capture-related globals (bound in registry_global, shared via g_capture_ctx).
static struct wl_shm                                       *g_shm        = nullptr;
static struct zwlr_screencopy_manager_v1                   *g_screencopy = nullptr;
static struct ext_image_copy_capture_manager_v1            *g_ext_copy   = nullptr;
static struct ext_output_image_capture_source_manager_v1   *g_ext_src    = nullptr;
static struct zwp_linux_dmabuf_v1                          *g_dmabuf     = nullptr;

static EglDmabuf      g_egl_dmabuf;
static CaptureContext g_capture_ctx;
static std::string    g_capture_protocol = "auto";
static bool           g_prefer_dmabuf    = true;

// Stereoscopic side-by-side (SBS) output + pose smoothing.
static bool  g_stereo      = false;
static float g_ipd_m       = 0.063f;
static float g_smoothing   = 0.0f;
static Quat  g_smoothed    = {0.f, 0.f, 0.f, 1.f};
static bool  g_smoothed_ok = false;

// Track all outputs for name-based selection
struct OutputInfo {
    struct wl_output *output  = nullptr;
    std::string       str_name;
    int               width   = 0;
    int               height  = 0;
};
static std::vector<OutputInfo> g_outputs;

static int   g_output_width  = 1920;
static int   g_output_height = 1080;
static bool  g_configured    = false;

// EGL / GL
static EGLDisplay            g_egl_display = EGL_NO_DISPLAY;
static EGLContext            g_egl_context = EGL_NO_CONTEXT;
static EGLSurface            g_egl_surface = EGL_NO_SURFACE;
static struct wl_egl_window *g_egl_window  = nullptr;

// Layer-shell surface
static struct wl_surface            *g_surface    = nullptr;
static struct zwlr_layer_surface_v1 *g_layer_surf = nullptr;

// Frame callback
static struct wl_callback *g_frame_callback = nullptr;

// GL handles
static GLuint g_prog      = 0;
static GLuint g_vbo       = 0;
static GLint  g_u_mvp     = -1;
static GLint  g_u_color   = -1;
static GLint  g_u_tex     = -1;
static GLint  g_u_use_tex = -1;
static GLint  g_u_swap_rb   = -1;
static GLint  g_u_flip_y    = -1;
static GLint  g_u_curvature = -1;
static int    g_quad_vertex_count = 6;

// ─── Signal flags — async-signal-safe ────────────────────────────────────────
static volatile sig_atomic_t g_running     = 1;
static volatile sig_atomic_t g_do_recenter = 0;

static void sig_quit(int)     { g_running = 0; }
static void sig_recenter(int) { g_do_recenter = 1; } // main thread does the work

// ─── Precomputed per-monitor geometry ─────────────────────────────────────────
// These are all STATIC (monitor positions on the arc never change at runtime).
// Built once in precompute_monitors(). Each monitor: 2 draw calls (quad + border).
// ─── Per-monitor runtime state ────────────────────────────────────────────────
// Each monitor carries its config, precomputed model matrices, and a swappable
// capture source. The list is data-driven (config.json / control API), so the
// number and placement of monitors is fully dynamic.
struct Monitor {
    MonitorConfig   cfg;
    Mat4            model;        // model matrix for the screen quad
    Mat4            border_model; // model matrix for the slightly-larger border
    ICaptureSource *capture = nullptr;
};
static std::vector<Monitor> g_monitors;

// Cached projection matrix — rebuilt only when output size changes
static Mat4 g_proj;        // full-width (mono)
static Mat4 g_proj_stereo; // half-width per-eye (SBS)

// ─── Pose source (swappable IPoseSource) ──────────────────────────────────────
static BreezyImuSource g_pose;

static inline Quat read_head_quat() { return g_pose.read().orientation; }

// ─── Control API server (runtime add/remove/configure monitors) ────────────────
static ControlServer g_control;

// ─── Head-tracking output (mode B: head-look to flight/space sims) ─────────────
static OpenTrackSink g_head_sink;
static HeadTrackConfig g_head_cfg;                                  // active settings
static std::vector<std::pair<std::string, HeadTrackConfig>> g_profiles;
static std::string g_profile_name;                                 // active profile ("" = none)

// (Re)configure the head-tracking sink and open/close the transport to match
// `c.enabled`. Called at startup and whenever the control API toggles it.
static void apply_head_tracking(const HeadTrackConfig &c) {
    g_head_sink.close();
    g_head_cfg = c;
    g_head_sink.configure(g_head_cfg);
    if (g_head_cfg.enabled) {
        if (g_head_sink.open())
            fprintf(stderr, "xr-workspace: head-tracking ON → %s %s:%d\n",
                    g_head_cfg.protocol.c_str(), g_head_cfg.host.c_str(), g_head_cfg.port);
        else
            fprintf(stderr, "xr-workspace: head-tracking enable FAILED (socket)\n");
    } else {
        fprintf(stderr, "xr-workspace: head-tracking OFF\n");
    }
}

// ─── Recenter ─────────────────────────────────────────────────────────────────
static Quat g_origin_inv = {0.f, 0.f, 0.f, 1.f};

static void recenter() {
    g_origin_inv = quat_conj(read_head_quat());
    fprintf(stderr, "xr-workspace: recentered\n");
}

// ─── Static data precomputation ───────────────────────────────────────────────
// Monitor model matrices are built by hand (cheaper than chained mat4_mul).
// Column-major layout: T * Ry(-a) * Scale, computed analytically.
static void precompute_monitors() {
    const float to_rad = (float)M_PI / 180.f;

    for (auto &mon : g_monitors) {
        const float a   = mon.cfg.angle_deg * to_rad;
        const float px  = g_arc_radius * sinf(a);
        const float pz  = -g_arc_radius * cosf(a);
        const float ca  = cosf(-a), sa = sinf(-a);
        const float w   = mon.cfg.size_m;
        const float h   = mon.cfg.size_m * 0.5625f; // 16:9

        // Model = Translate(px,0,pz) * RotY(-a) * Scale(w,h,1), computed inline.
        Mat4 &mod = mon.model;
        mod = Mat4::identity();
        mod.m[0]=ca*w;  mod.m[4]=0.f; mod.m[8] =sa;  mod.m[12]=px;
        mod.m[1]=0.f;   mod.m[5]=h;   mod.m[9] =0.f; mod.m[13]=0.f;
        mod.m[2]=-sa*w; mod.m[6]=0.f; mod.m[10]=ca;  mod.m[14]=pz;
        mod.m[3]=0.f;   mod.m[7]=0.f; mod.m[11]=0.f; mod.m[15]=1.f;

        // Border: 4% larger in screen-plane, pushed 0.002 behind the monitor.
        const float push = 0.002f;
        const float bw   = w * 1.04f;
        const float bh   = h * 1.04f;
        const float bpx  = px + sa * push;
        const float bpz  = pz + ca * push;

        Mat4 &bm = mon.border_model;
        bm = Mat4::identity();
        bm.m[0]=ca*bw;  bm.m[4]=0.f; bm.m[8] =sa;  bm.m[12]=bpx;
        bm.m[1]=0.f;    bm.m[5]=bh;  bm.m[9] =0.f; bm.m[13]=0.f;
        bm.m[2]=-sa*bw; bm.m[6]=0.f; bm.m[10]=ca;  bm.m[14]=bpz;
        bm.m[3]=0.f;    bm.m[7]=0.f; bm.m[11]=0.f; bm.m[15]=1.f;
    }
}

static void precompute_proj() {
    const float fov_rad = g_fov_deg * (float)M_PI / 180.f;
    const float aspect  = (float)g_output_width / (float)g_output_height;
    g_proj = mat4_perspective(fov_rad, aspect, 0.1f, 100.f);
    // Per-eye projection for SBS: each eye gets half the horizontal pixels.
    const float aspect_eye = (float)(g_output_width / 2) / (float)g_output_height;
    g_proj_stereo = mat4_perspective(fov_rad, aspect_eye, 0.1f, 100.f);
}

// ─── GL init ──────────────────────────────────────────────────────────────────
// Build a capture source for a monitor via the backend factory: real screen
// capture (ext-image-copy-capture → wlr-screencopy, dmabuf → shm) when a source
// output is set, else a solid-colour placeholder.
static ICaptureSource *make_capture(const MonitorConfig &cfg) {
    return make_capture_source(&g_capture_ctx, cfg, g_capture_protocol);
}

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char buf[512]; glGetShaderInfoLog(s, 512, nullptr, buf);
        fprintf(stderr, "Shader error: %s\n", buf);
    }
    return s;
}

static void gl_init() {
    const char *vert = R"glsl(
        attribute vec3 a_pos;
        attribute vec2 a_uv;
        uniform mat4 u_mvp;
        uniform float u_curvature;
        varying vec2 v_uv;
        void main() {
            v_uv = a_uv;
            vec3 p = a_pos;
            // Cylindrical curve around a vertical axis. At curvature=1 the screen
            // edges bend ~1 rad toward the viewer; flat when curvature≈0.
            if (u_curvature > 0.001) {
                float R = 0.5 / u_curvature;
                float theta = p.x / R;
                p.x = R * sin(theta);
                p.z = R * (cos(theta) - 1.0);
            }
            gl_Position = u_mvp * vec4(p, 1.0);
        }
    )glsl";

    const char *frag = R"glsl(
        precision mediump float;
        varying vec2 v_uv;
        uniform vec4 u_color;
        uniform sampler2D u_tex;
        uniform int u_use_tex;
        uniform int u_swap_rb;
        uniform int u_flip_y;
        void main() {
            if (u_use_tex != 0) {
                vec2 uv = v_uv;
                if (u_flip_y != 0) uv.y = 1.0 - uv.y;
                vec4 c = texture2D(u_tex, uv);
                if (u_swap_rb != 0) c = c.bgra;
                gl_FragColor = c * u_color;
            } else {
                gl_FragColor = u_color;
            }
        }
    )glsl";

    const GLuint v = compile_shader(GL_VERTEX_SHADER,   vert);
    const GLuint f = compile_shader(GL_FRAGMENT_SHADER, frag);
    g_prog = glCreateProgram();
    glAttachShader(g_prog, v);
    glAttachShader(g_prog, f);
    glBindAttribLocation(g_prog, 0, "a_pos");
    glBindAttribLocation(g_prog, 1, "a_uv");
    glLinkProgram(g_prog);
    { GLint ok; glGetProgramiv(g_prog, GL_LINK_STATUS, &ok);
      if (!ok) { char b[512]; glGetProgramInfoLog(g_prog,512,nullptr,b); fprintf(stderr,"Link: %s\n",b); } }
    glDeleteShader(v); glDeleteShader(f);

    g_u_mvp       = glGetUniformLocation(g_prog, "u_mvp");
    g_u_color     = glGetUniformLocation(g_prog, "u_color");
    g_u_tex       = glGetUniformLocation(g_prog, "u_tex");
    g_u_use_tex   = glGetUniformLocation(g_prog, "u_use_tex");
    g_u_swap_rb   = glGetUniformLocation(g_prog, "u_swap_rb");
    g_u_flip_y    = glGetUniformLocation(g_prog, "u_flip_y");
    g_u_curvature = glGetUniformLocation(g_prog, "u_curvature");

    // Tessellated unit quad: a horizontal strip of N segments so the vertex
    // shader can bend it for curved screens. Interleaved pos(3)+uv(2).
    // UV: u 0→1 left→right, v 0 at top / 1 at bottom (matches texture row order).
    const int N = 32;
    std::vector<float> mesh;
    mesh.reserve((size_t)N * 6 * 5);
    const float dx = 1.f / (float)N;
    for (int i = 0; i < N; i++) {
        const float x0 = -0.5f + (float)i * dx;
        const float x1 = -0.5f + (float)(i + 1) * dx;
        const float u0 = (float)i * dx;
        const float u1 = (float)(i + 1) * dx;
        const float verts[6][5] = {
            {x0,-0.5f,0.f, u0,1.f}, {x1,-0.5f,0.f, u1,1.f}, {x1,0.5f,0.f, u1,0.f},
            {x0,-0.5f,0.f, u0,1.f}, {x1, 0.5f,0.f, u1,0.f}, {x0,0.5f,0.f, u0,0.f},
        };
        for (auto &vv : verts) { for (int k = 0; k < 5; k++) mesh.push_back(vv[k]); }
    }
    g_quad_vertex_count = N * 6;

    glGenBuffers(1, &g_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(mesh.size() * sizeof(float)),
                 mesh.data(), GL_STATIC_DRAW);
    // Set vertex attribute pointers ONCE — constant for the lifetime of the app
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5*4, (void*)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5*4, (void*)(3*4));
    // Leave the VBO bound permanently — we never rebind a different one

    // Capture sources — one per monitor (ICaptureSource), built from config.
    for (auto &mon : g_monitors)
        mon.capture = make_capture(mon.cfg);
    glBindTexture(GL_TEXTURE_2D, 0);

    // Set immutable GL state — program, blend, depth — set once, never touch again
    glUseProgram(g_prog);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(g_u_tex, 0);
}

// ─── Render ───────────────────────────────────────────────────────────────────
// Draw every monitor (screen quad + border) for one eye, given its
// projection·view matrix. Capture sources must already have been updated this
// frame (call update_captures() once, before any eye).
static void draw_monitors(const Mat4 &pv) {
    for (const auto &mon : g_monitors) {
        // Screen quad
        const Mat4 mvp = mat4_mul(pv, mon.model);
        glUniformMatrix4fv(g_u_mvp, 1, GL_FALSE, mvp.m);
        glUniform1f(g_u_curvature, mon.cfg.curvature);

        const GLuint tex = mon.capture ? mon.capture->texture() : 0;
        if (tex) {
            glUniform4f(g_u_color, 1.f, 1.f, 1.f, 1.f);
            glBindTexture(GL_TEXTURE_2D, tex);
            glUniform1i(g_u_use_tex, 1);
            glUniform1i(g_u_swap_rb, mon.capture->swizzle_bgr() ? 1 : 0);
            glUniform1i(g_u_flip_y,  mon.capture->flip_y() ? 1 : 0);
        } else {
            // No frame yet — show the monitor's configured solid colour.
            glUniform4f(g_u_color,
                        ((mon.cfg.color >> 16) & 0xFF) / 255.f,
                        ((mon.cfg.color >> 8)  & 0xFF) / 255.f,
                        ( mon.cfg.color        & 0xFF) / 255.f, 1.f);
            glUniform1i(g_u_use_tex, 0);
        }
        glDrawArrays(GL_TRIANGLES, 0, g_quad_vertex_count);

        // Border quad
        const Mat4 bmvp = mat4_mul(pv, mon.border_model);
        glUniformMatrix4fv(g_u_mvp, 1, GL_FALSE, bmvp.m);
        glUniform4f(g_u_color, 0.2f, 0.2f, 0.2f, 0.9f);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUniform1i(g_u_use_tex, 0);
        glDrawArrays(GL_TRIANGLES, 0, g_quad_vertex_count);
    }
}

static void render_frame() {
    // Deferred recenter from SIGUSR1 — safe to call here in main thread
    if (g_do_recenter) { g_do_recenter = 0; recenter(); }

    // IMU read as LATE as possible before vertex transform = minimum motion-to-photon latency
    const Quat head    = read_head_quat();
    Quat       rel     = quat_mul(g_origin_inv, head);

    // Optional pose smoothing: slerp toward the new orientation. Higher
    // smoothing = heavier low-pass (more stable, more lag).
    if (g_smoothing > 0.0001f) {
        if (!g_smoothed_ok) { g_smoothed = rel; g_smoothed_ok = true; }
        const float t = 1.0f - g_smoothing;
        g_smoothed = quat_normalize(quat_slerp(g_smoothed, rel, t));
        rel = g_smoothed;
    }

    if (g_head_sink.config().enabled) g_head_sink.send(rel); // mode B: head-look → sim

    const Mat4 head_m = quat_to_mat4(rel);
    const Mat4 view   = mat4_rot_inverse(head_m);

    // Refresh all capture textures ONCE per frame (not per eye).
    for (auto &mon : g_monitors)
        if (mon.capture) mon.capture->update();

    glClearColor(0.f, 0.f, 0.f, 1.f);

    if (g_stereo) {
        // Side-by-side: left eye → left half, right eye → right half, each with
        // a horizontal camera offset of ±IPD/2.
        const int half = g_output_width / 2;
        glViewport(0, 0, g_output_width, g_output_height);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        const Mat4 view_l = mat4_mul(mat4_translate( g_ipd_m * 0.5f, 0.f, 0.f), view);
        glViewport(0, 0, half, g_output_height);
        draw_monitors(mat4_mul(g_proj_stereo, view_l));

        const Mat4 view_r = mat4_mul(mat4_translate(-g_ipd_m * 0.5f, 0.f, 0.f), view);
        glViewport(half, 0, g_output_width - half, g_output_height);
        draw_monitors(mat4_mul(g_proj_stereo, view_r));
    } else {
        glViewport(0, 0, g_output_width, g_output_height);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        draw_monitors(mat4_mul(g_proj, view));
    }

    eglSwapBuffers(g_egl_display, g_egl_surface);
}

// ─── Wayland frame callback ───────────────────────────────────────────────────
static void schedule_frame(); // forward decl

static void on_frame(void*, struct wl_callback *cb, uint32_t) {
    wl_callback_destroy(cb);
    g_frame_callback = nullptr;
    if (!g_running) return;
    render_frame();
    schedule_frame();
}

static const struct wl_callback_listener g_frame_listener = { on_frame };

static void schedule_frame() {
    g_frame_callback = wl_surface_frame(g_surface);
    wl_callback_add_listener(g_frame_callback, &g_frame_listener, nullptr);
    wl_surface_commit(g_surface);
}

// ─── EGL init ─────────────────────────────────────────────────────────────────
static bool egl_init() {
    g_egl_display = eglGetDisplay((EGLNativeDisplayType)g_display);
    if (g_egl_display == EGL_NO_DISPLAY) { fprintf(stderr, "eglGetDisplay failed\n"); return false; }

    EGLint major, minor;
    if (!eglInitialize(g_egl_display, &major, &minor)) { fprintf(stderr, "eglInitialize failed\n"); return false; }
    fprintf(stderr, "EGL %d.%d\n", major, minor);

    eglBindAPI(EGL_OPENGL_ES_API);

    static const EGLint cfg_attrs[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE,   8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 16,
        EGL_NONE
    };
    EGLConfig cfg; EGLint num;
    if (!eglChooseConfig(g_egl_display, cfg_attrs, &cfg, 1, &num) || num < 1) {
        fprintf(stderr, "eglChooseConfig failed\n"); return false;
    }

    static const EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    g_egl_context = eglCreateContext(g_egl_display, cfg, EGL_NO_CONTEXT, ctx_attrs);
    if (g_egl_context == EGL_NO_CONTEXT) { fprintf(stderr, "eglCreateContext failed\n"); return false; }

    g_egl_window  = wl_egl_window_create(g_surface, g_output_width, g_output_height);
    g_egl_surface = eglCreateWindowSurface(g_egl_display, cfg, (EGLNativeWindowType)g_egl_window, nullptr);
    if (g_egl_surface == EGL_NO_SURFACE) { fprintf(stderr, "eglCreateWindowSurface failed\n"); return false; }

    eglMakeCurrent(g_egl_display, g_egl_surface, g_egl_surface, g_egl_context);

    // Disable EGL vsync — frame pacing is driven by wl_surface.frame callbacks instead.
    // This eliminates the double-throttle that would otherwise add a full frame of latency.
    eglSwapInterval(g_egl_display, 0);
    return true;
}

// ─── Wayland listeners ────────────────────────────────────────────────────────
static OutputInfo *find_output(struct wl_output *out) {
    for (auto &o : g_outputs) if (o.output == out) return &o;
    return nullptr;
}

static void layer_surface_configure(void*, struct zwlr_layer_surface_v1 *surf,
    uint32_t serial, uint32_t w, uint32_t h)
{
    if (w > 0) g_output_width  = (int)w;
    if (h > 0) g_output_height = (int)h;
    zwlr_layer_surface_v1_ack_configure(surf, serial);
    g_configured = true;
    fprintf(stderr, "xr-workspace: configured %dx%d\n", g_output_width, g_output_height);
    if (g_egl_window)
        wl_egl_window_resize(g_egl_window, g_output_width, g_output_height, 0, 0);
    precompute_proj(); // aspect ratio may have changed
}
static void layer_surface_closed(void*, struct zwlr_layer_surface_v1*) {
    fprintf(stderr, "xr-workspace: surface closed\n");
    g_running = 0;
}
static const zwlr_layer_surface_v1_listener g_layer_surf_listener = {
    .configure = layer_surface_configure,
    .closed    = layer_surface_closed,
};

static void output_geometry(void*,struct wl_output*,int,int,int,int,int,const char*,const char*,int32_t){}
static void output_mode(void*, struct wl_output *out, uint32_t flags, int32_t w, int32_t h, int32_t) {
    if (!(flags & WL_OUTPUT_MODE_CURRENT)) return;
    if (OutputInfo *o = find_output(out)) { o->width = w; o->height = h; }
}
static void output_done(void*,struct wl_output*){}
static void output_scale(void*,struct wl_output*,int32_t){}
static void output_name(void*, struct wl_output *out, const char *name) {
    if (!name) return;
    if (OutputInfo *o = find_output(out)) {
        o->str_name = name;
        fprintf(stderr, "xr-workspace: output '%s' (%dx%d)\n", name, o->width, o->height);
        if (!g_target_output_name.empty() && g_target_output_name == name) {
            g_xr_output     = out;
            if (o->width  > 0) g_output_width  = o->width;
            if (o->height > 0) g_output_height = o->height;
            fprintf(stderr, "xr-workspace: selected output '%s'\n", name);
        }
    }
}
static void output_description(void*,struct wl_output*,const char*){}

static const wl_output_listener g_output_listener = {
    .geometry    = output_geometry,
    .mode        = output_mode,
    .done        = output_done,
    .scale       = output_scale,
    .name        = output_name,
    .description = output_description,
};

// ─── Global shortcut callbacks ────────────────────────────────────────────────
static void shortcut_pressed(void*, struct hyprland_global_shortcut_v1*, uint32_t, uint32_t, uint32_t) {
    // Fired by Hyprland when Ctrl+Shift+C is pressed — queue a recenter
    g_do_recenter = 1;
}
static void shortcut_released(void*, struct hyprland_global_shortcut_v1*, uint32_t, uint32_t, uint32_t) {}

static const hyprland_global_shortcut_v1_listener g_shortcut_listener = {
    .pressed  = shortcut_pressed,
    .released = shortcut_released,
};

static void registry_global(void*, struct wl_registry *registry,
    uint32_t name, const char *interface, uint32_t version)
{
    if (!strcmp(interface, wl_compositor_interface.name)) {
        g_compositor = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface, std::min(version, 4u)));
    } else if (!strcmp(interface, zwlr_layer_shell_v1_interface.name)) {
        g_layer_shell = static_cast<zwlr_layer_shell_v1*>(
            wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, std::min(version, 4u)));
    } else if (!strcmp(interface, wl_output_interface.name)) {
        auto *out = static_cast<wl_output*>(
            wl_registry_bind(registry, name, &wl_output_interface, std::min(version, 4u)));
        wl_output_add_listener(out, &g_output_listener, nullptr);
        g_outputs.push_back({out, "", 0, 0});
        if (g_target_output_name.empty())
            g_xr_output = out; // default: last-enumerated output (glasses appear last)
    } else if (!strcmp(interface, hyprland_global_shortcuts_manager_v1_interface.name)) {
        g_shortcuts_mgr = static_cast<hyprland_global_shortcuts_manager_v1*>(
            wl_registry_bind(registry, name, &hyprland_global_shortcuts_manager_v1_interface, 1u));
    } else if (!strcmp(interface, wl_shm_interface.name)) {
        g_shm = static_cast<wl_shm*>(
            wl_registry_bind(registry, name, &wl_shm_interface, 1u));
    } else if (!strcmp(interface, zwlr_screencopy_manager_v1_interface.name)) {
        g_screencopy = static_cast<zwlr_screencopy_manager_v1*>(
            wl_registry_bind(registry, name, &zwlr_screencopy_manager_v1_interface, std::min(version, 3u)));
    } else if (!strcmp(interface, ext_image_copy_capture_manager_v1_interface.name)) {
        g_ext_copy = static_cast<ext_image_copy_capture_manager_v1*>(
            wl_registry_bind(registry, name, &ext_image_copy_capture_manager_v1_interface, 1u));
    } else if (!strcmp(interface, ext_output_image_capture_source_manager_v1_interface.name)) {
        g_ext_src = static_cast<ext_output_image_capture_source_manager_v1*>(
            wl_registry_bind(registry, name, &ext_output_image_capture_source_manager_v1_interface, 1u));
    } else if (!strcmp(interface, zwp_linux_dmabuf_v1_interface.name)) {
        // v3 is enough for create_immed; avoids the v4 feedback machinery.
        g_dmabuf = static_cast<zwp_linux_dmabuf_v1*>(
            wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface, std::min(version, 3u)));
    }
}
static void registry_global_remove(void*, struct wl_registry*, uint32_t) {}

static const wl_registry_listener g_registry_listener = {
    .global        = registry_global,
    .global_remove = registry_global_remove,
};

// ─── Control API handler ──────────────────────────────────────────────────────
static JsonValue monitor_to_json(const Monitor &m) {
    return jobj({
        {"id",        jstr(m.cfg.id)},
        {"source",    jstr(m.cfg.source)},
        {"width",     jnum(m.cfg.width)},
        {"height",    jnum(m.cfg.height)},
        {"angle_deg", jnum(m.cfg.angle_deg)},
        {"size_m",    jnum(m.cfg.size_m)},
        {"curvature", jnum(m.cfg.curvature)},
        {"color",     jnum((double)m.cfg.color)},
    });
}

static MonitorConfig monitor_from_json(const JsonValue &mv, MonitorConfig m) {
    if (auto *v = mv.find("id"))        m.id        = v->str_or(m.id);
    if (auto *v = mv.find("source"))    m.source    = v->str_or(m.source);
    if (auto *v = mv.find("width"))     m.width     = v->int_or(m.width);
    if (auto *v = mv.find("height"))    m.height    = v->int_or(m.height);
    if (auto *v = mv.find("scale"))     m.scale     = (float)v->num_or(m.scale);
    if (auto *v = mv.find("angle_deg")) m.angle_deg = (float)v->num_or(m.angle_deg);
    if (auto *v = mv.find("size_m"))    m.size_m    = (float)v->num_or(m.size_m);
    if (auto *v = mv.find("curvature")) m.curvature = (float)v->num_or(m.curvature);
    if (auto *v = mv.find("color"))     m.color     = (uint32_t)v->num_or(m.color) & 0xFFFFFFu;
    return m;
}

// Serialise the current live scene + render settings to a JSON object (used by
// the `save` control command — the smplOS settings app round-trips this).
static JsonValue config_to_json() {
    JsonValue mons = jarr();
    for (const auto &m : g_monitors) mons.arr.push_back(monitor_to_json(m));
    return jobj({
        {"output",           jstr(g_target_output_name)},
        {"fov_deg",          jnum(g_fov_deg)},
        {"smoothing",        jnum(g_smoothing)},
        {"capture_protocol", jstr(g_capture_protocol)},
        {"prefer_dmabuf",    jbool(g_prefer_dmabuf)},
        {"stereo",           jbool(g_stereo)},
        {"ipd_m",            jnum(g_ipd_m)},
        {"layout",           jobj({{"mode", jstr("arc")}, {"radius", jnum(g_arc_radius)}})},
        {"monitors",         mons},
        {"head_tracking",    jobj({
            {"enabled",      jbool(g_head_cfg.enabled)},
            {"protocol",     jstr(g_head_cfg.protocol)},
            {"host",         jstr(g_head_cfg.host)},
            {"port",         jnum(g_head_cfg.port)},
            {"rate_hz",      jnum(g_head_cfg.rate_hz)},
            {"yaw_scale",    jnum(g_head_cfg.yaw_scale)},
            {"pitch_scale",  jnum(g_head_cfg.pitch_scale)},
            {"roll_scale",   jnum(g_head_cfg.roll_scale)},
            {"invert_yaw",   jbool(g_head_cfg.invert_yaw)},
            {"invert_pitch", jbool(g_head_cfg.invert_pitch)},
            {"invert_roll",  jbool(g_head_cfg.invert_roll)},
            {"deadzone_deg", jnum(g_head_cfg.deadzone_deg)},
        })},
    });
}

// Replace the live scene + render settings from a freshly-loaded Config. Runs on
// the main thread with a current GL context, so it can rebuild capture sources.
static void apply_config(const Config &c) {
    g_fov_deg              = c.fov_deg;
    g_arc_radius           = c.radius;
    g_capture_protocol     = c.capture_protocol;
    g_prefer_dmabuf        = c.prefer_dmabuf;
    g_capture_ctx.prefer_dmabuf = c.prefer_dmabuf;
    g_stereo               = c.stereo;
    g_ipd_m                = c.ipd_m;
    g_smoothing            = c.smoothing;
    g_smoothed_ok          = false;

    for (auto &m : g_monitors) if (m.capture) { m.capture->stop(); delete m.capture; m.capture = nullptr; }
    g_monitors.clear();
    for (const auto &mc : c.monitors) {
        Monitor m; m.cfg = mc; m.capture = make_capture(mc);
        g_monitors.push_back(std::move(m));
    }
    g_monitor_count = (int)g_monitors.size();

    precompute_proj();
    precompute_monitors();

    g_profiles = c.profiles;
    apply_head_tracking(c.head_tracking);
}

// Mutates the live scene in response to a JSON request. Runs on the main thread
// between frames, so it can freely touch GL/scene state with no locking.
static JsonValue handle_control(const JsonValue &req) {
    const JsonValue *cmdv = req.find("cmd");
    const std::string cmd = cmdv ? cmdv->str_or("") : "";

    if (cmd == "list" || cmd == "get_config") {
        JsonValue mons = jarr();
        for (const auto &m : g_monitors) mons.arr.push_back(monitor_to_json(m));
        JsonValue profs = jarr();
        for (const auto &p : g_profiles) profs.arr.push_back(jstr(p.first));
        return jobj({{"ok", jbool(true)},
                     {"radius",   jnum(g_arc_radius)},
                     {"fov_deg",  jnum(g_fov_deg)},
                     {"monitors", mons},
                     {"head_tracking", jobj({
                         {"enabled",     jbool(g_head_cfg.enabled)},
                         {"protocol",    jstr(g_head_cfg.protocol)},
                         {"host",        jstr(g_head_cfg.host)},
                         {"port",        jnum(g_head_cfg.port)},
                         {"profile",     jstr(g_profile_name)},
                         {"yaw_scale",   jnum(g_head_cfg.yaw_scale)},
                         {"pitch_scale", jnum(g_head_cfg.pitch_scale)},
                         {"roll_scale",  jnum(g_head_cfg.roll_scale)},
                     })},
                     {"profiles", profs}});
    }
    if (cmd == "recenter") {
        g_do_recenter = 1;
        return jobj({{"ok", jbool(true)}});
    }
    if (cmd == "layout") {
        if (auto *v = req.find("radius")) g_arc_radius = (float)v->num_or(g_arc_radius);
        precompute_monitors();
        return jobj({{"ok", jbool(true)}});
    }
    if (cmd == "add") {
        const JsonValue *mv = req.find("monitor");
        if (!mv || mv->type != JsonValue::Object)
            return jobj({{"ok", jbool(false)}, {"error", jstr("missing 'monitor' object")}});
        Monitor m;
        m.cfg = monitor_from_json(*mv, MonitorConfig{});
        if (m.cfg.id.empty()) m.cfg.id = "monitor" + std::to_string(g_monitors.size());
        for (const auto &e : g_monitors)
            if (e.cfg.id == m.cfg.id)
                return jobj({{"ok", jbool(false)}, {"error", jstr("id already exists")}});
        m.capture = make_capture(m.cfg);
        g_monitors.push_back(std::move(m));
        precompute_monitors();
        return jobj({{"ok", jbool(true)}, {"id", jstr(g_monitors.back().cfg.id)}});
    }
    if (cmd == "remove") {
        const JsonValue *idv = req.find("id");
        if (!idv) return jobj({{"ok", jbool(false)}, {"error", jstr("missing 'id'")}});
        const std::string id = idv->str_or("");
        for (auto it = g_monitors.begin(); it != g_monitors.end(); ++it) {
            if (it->cfg.id == id) {
                if (it->capture) { it->capture->stop(); delete it->capture; }
                g_monitors.erase(it);
                precompute_monitors();
                return jobj({{"ok", jbool(true)}});
            }
        }
        return jobj({{"ok", jbool(false)}, {"error", jstr("no such monitor")}});
    }
    if (cmd == "set") {
        const JsonValue *idv = req.find("id");
        const JsonValue *pv  = req.find("props");
        if (!idv || !pv || pv->type != JsonValue::Object)
            return jobj({{"ok", jbool(false)}, {"error", jstr("missing 'id'/'props'")}});
        const std::string id = idv->str_or("");
        for (auto &m : g_monitors) {
            if (m.cfg.id == id) {
                const uint32_t    old_color  = m.cfg.color;
                const std::string old_source = m.cfg.source;
                m.cfg = monitor_from_json(*pv, m.cfg);
                if (m.cfg.color != old_color || m.cfg.source != old_source) {
                    if (m.capture) { m.capture->stop(); delete m.capture; }
                    m.capture = make_capture(m.cfg);
                }
                precompute_monitors();
                return jobj({{"ok", jbool(true)}});
            }
        }
        return jobj({{"ok", jbool(false)}, {"error", jstr("no such monitor")}});
    }
    // ── Head-tracking (mode B) ──
    if (cmd == "head_tracking") {
        HeadTrackConfig hc = g_head_cfg;
        if (auto *e = req.find("enabled")) hc.enabled = e->bool_or(hc.enabled);
        if (auto *s = req.find("set"))     headtrack_merge_json(*s, hc);
        apply_head_tracking(hc);
        return jobj({{"ok", jbool(true)}, {"enabled", jbool(g_head_cfg.enabled)}});
    }
    if (cmd == "profile") {
        const JsonValue *nv = req.find("name");
        if (!nv) return jobj({{"ok", jbool(false)}, {"error", jstr("missing 'name'")}});
        const std::string nm = nv->str_or("");
        for (const auto &p : g_profiles) {
            if (p.first == nm) {
                g_profile_name = nm;
                apply_head_tracking(p.second);
                return jobj({{"ok", jbool(true)}, {"profile", jstr(nm)},
                             {"enabled", jbool(g_head_cfg.enabled)}});
            }
        }
        return jobj({{"ok", jbool(false)}, {"error", jstr("no such profile")}});
    }
    // ── Render settings (stereo / smoothing / capture backend) ──
    if (cmd == "render") {
        bool rebuild = false;
        if (auto *v = req.find("stereo"))    g_stereo    = v->bool_or(g_stereo);
        if (auto *v = req.find("ipd_m"))     g_ipd_m     = (float)v->num_or(g_ipd_m);
        if (auto *v = req.find("smoothing")) { g_smoothing = (float)v->num_or(g_smoothing); g_smoothed_ok = false; }
        if (auto *v = req.find("fov_deg"))   { g_fov_deg   = (float)v->num_or(g_fov_deg); precompute_proj(); }
        if (auto *v = req.find("capture_protocol")) { g_capture_protocol = v->str_or(g_capture_protocol); rebuild = true; }
        if (auto *v = req.find("prefer_dmabuf")) {
            g_prefer_dmabuf = v->bool_or(g_prefer_dmabuf);
            g_capture_ctx.prefer_dmabuf = g_prefer_dmabuf;
            rebuild = true;
        }
        if (rebuild) {
            for (auto &m : g_monitors) {
                if (m.capture) { m.capture->stop(); delete m.capture; }
                m.capture = make_capture(m.cfg);
            }
        }
        return jobj({{"ok", jbool(true)},
                     {"stereo", jbool(g_stereo)},
                     {"smoothing", jnum(g_smoothing)},
                     {"capture_protocol", jstr(g_capture_protocol)}});
    }
    // ── Persistence (smplOS settings-app integration surface) ──
    if (cmd == "reload") {
        if (g_resolved_config_path.empty())
            return jobj({{"ok", jbool(false)}, {"error", jstr("no config path")}});
        Config c; c.make_default_monitors(g_monitor_count);
        std::string err;
        if (!c.load(g_resolved_config_path, err))
            return jobj({{"ok", jbool(false)}, {"error", jstr(err)}});
        apply_config(c);
        return jobj({{"ok", jbool(true)}, {"monitors", jnum((double)g_monitors.size())}});
    }
    if (cmd == "save") {
        std::string path = g_resolved_config_path;
        if (auto *pv = req.find("path")) path = pv->str_or(path);
        if (path.empty())
            return jobj({{"ok", jbool(false)}, {"error", jstr("no config path")}});
        const std::string text = json_dump(config_to_json());
        FILE *f = fopen(path.c_str(), "w");
        if (!f) return jobj({{"ok", jbool(false)}, {"error", jstr("cannot open file")}});
        fwrite(text.data(), 1, text.size(), f);
        fputc('\n', f);
        fclose(f);
        return jobj({{"ok", jbool(true)}, {"path", jstr(path)}});
    }
    // ── Hyprland orchestration (M6) ──
    if (cmd == "hypr_create_output") {
        std::string reply;
        if (!hypr::create_headless_output(reply))
            return jobj({{"ok", jbool(false)}, {"error", jstr("hyprland IPC unavailable")}});
        return jobj({{"ok", jbool(true)}, {"reply", jstr(reply)}});
    }
    if (cmd == "hypr_remove_output") {
        const JsonValue *nv = req.find("name");
        if (!nv) return jobj({{"ok", jbool(false)}, {"error", jstr("missing 'name'")}});
        return jobj({{"ok", jbool(hypr::remove_output(nv->str_or("")))}});
    }
    if (cmd == "hypr_move_window") {
        const JsonValue *ov = req.find("output");
        if (!ov) return jobj({{"ok", jbool(false)}, {"error", jstr("missing 'output'")}});
        const std::string addr = req.find("window") ? req.find("window")->str_or("") : "";
        return jobj({{"ok", jbool(hypr::move_window_to_output(ov->str_or(""), addr))}});
    }
    return jobj({{"ok", jbool(false)}, {"error", jstr("unknown cmd")}});
}

// ─── Usage ────────────────────────────────────────────────────────────────────
static void print_usage(const char *a) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  --config PATH   Path to config.json (default: XDG config dir)\n"
        "  --monitors N    Virtual monitors: 1 or 3 (default: 3)\n"
        "  --output NAME   Wayland output name e.g. DP-3 (default: last output)\n"
        "  --radius R      Arc radius in world units (default: 2.0)\n"
        "  --fov DEG       Glasses horizontal FOV in degrees (default: 46.0)\n"
        "  --recenter      Prompt to recenter before rendering\n"
        "  --profile NAME  Activate a head-tracking profile from config.json\n"
        "  --head-tracking Force-enable head-tracking output (mode B)\n"
        "  --help\n\n"
        "Keyboard shortcut (registered natively with Hyprland):\n"
        "  Ctrl+Shift+C    Snap/recenter to current head position\n"
        "  Works globally — no focus needed on the XR surface.\n\n"
        "Signals:\n"
        "  SIGINT/SIGTERM  Quit\n"
        "  SIGUSR1         Recenter (same as Ctrl+Shift+C)\n\n"
        "About drift:\n"
        "  IMU yaw drift is a hardware property of the gyroscope.\n"
        "  Small errors accumulate over time regardless of software.\n"
        "  Press Ctrl+Shift+C whenever the view has drifted to snap it back.\n", a);
}

// ─── Main ─────────────────────────────────────────────────────────────────────
int main(int argc, char **argv) {
    bool do_recenter_prompt = false;
    bool set_monitors = false, set_output = false, set_radius = false, set_fov = false;
    bool force_head_tracking = false;
    std::string cli_profile;
    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i],"--help"))                    { print_usage(argv[0]); return 0; }
        else if (!strcmp(argv[i],"--config")   && i+1 < argc)   g_config_path = argv[++i];
        else if (!strcmp(argv[i],"--monitors") && i+1 < argc) { g_monitor_count = atoi(argv[++i]); set_monitors = true; }
        else if (!strcmp(argv[i],"--output")   && i+1 < argc) { g_target_output_name = argv[++i]; set_output = true; }
        else if (!strcmp(argv[i],"--radius")   && i+1 < argc) { g_arc_radius = strtof(argv[++i], nullptr); set_radius = true; }
        else if (!strcmp(argv[i],"--fov")      && i+1 < argc) { g_fov_deg    = strtof(argv[++i], nullptr); set_fov = true; }
        else if (!strcmp(argv[i],"--profile")  && i+1 < argc)   cli_profile = argv[++i];
        else if (!strcmp(argv[i],"--head-tracking"))            force_head_tracking = true;
        else if (!strcmp(argv[i],"--recenter"))                 do_recenter_prompt = true;
        else { fprintf(stderr, "Unknown option: %s\n", argv[i]); print_usage(argv[0]); return 1; }
    }

    // ── Load config.json — CLI flags take precedence over file values ──
    Config cfg;
    cfg.make_default_monitors(g_monitor_count);
    {
        const std::string path = config_default_path(g_config_path);
        g_resolved_config_path = path;
        std::string err;
        if (!cfg.load(path, err))
            fprintf(stderr, "xr-workspace: config error in %s: %s\n", path.c_str(), err.c_str());
        else
            fprintf(stderr, "xr-workspace: config '%s'\n", path.c_str());
    }
    if (!set_fov)      g_fov_deg            = cfg.fov_deg;
    if (!set_radius)   g_arc_radius         = cfg.radius;
    if (!set_output)   g_target_output_name = cfg.output;
    if (set_monitors)  cfg.make_default_monitors(g_monitor_count); // explicit count wins

    // Capture + render settings from config.
    g_capture_protocol = cfg.capture_protocol;
    g_prefer_dmabuf    = cfg.prefer_dmabuf;
    g_stereo           = cfg.stereo;
    g_ipd_m            = cfg.ipd_m;
    g_smoothing        = cfg.smoothing;

    g_monitors.clear();
    for (const auto &mc : cfg.monitors) {
        Monitor m; m.cfg = mc; g_monitors.push_back(m);
    }
    g_monitor_count = (int)g_monitors.size();

    // ── Head-tracking output (mode B): pick active profile, CLI overrides file ──
    g_profiles = cfg.profiles;
    g_head_cfg = cfg.head_tracking;
    {
        const std::string prof = !cli_profile.empty() ? cli_profile : cfg.active_profile;
        if (!prof.empty()) {
            if (const HeadTrackConfig *p = cfg.find_profile(prof)) {
                g_profile_name = prof;
                g_head_cfg     = *p;
                fprintf(stderr, "xr-workspace: head-tracking profile '%s'\n", prof.c_str());
            } else {
                fprintf(stderr, "xr-workspace: head-tracking profile '%s' not found\n", prof.c_str());
            }
        }
    }
    if (force_head_tracking) g_head_cfg.enabled = true;

    // Signals — handlers only set atomic flags; all real work done in main thread
    signal(SIGINT,  sig_quit);
    signal(SIGTERM, sig_quit);
    signal(SIGUSR1, sig_recenter);

    fprintf(stderr, "xr-workspace: %d monitor(s), output='%s'\n",
            g_monitor_count, g_target_output_name.c_str());

    // ── Wayland connect ──
    g_display = wl_display_connect(nullptr);
    if (!g_display) { fprintf(stderr, "Cannot connect to Wayland\n"); return 1; }

    auto *registry = wl_display_get_registry(g_display);
    wl_registry_add_listener(registry, &g_registry_listener, nullptr);
    wl_display_roundtrip(g_display); // bind globals + wl_output
    wl_display_roundtrip(g_display); // receive wl_output name/mode events

    if (!g_compositor) { fprintf(stderr, "No wl_compositor\n"); return 1; }
    if (!g_layer_shell) { fprintf(stderr, "No zwlr_layer_shell_v1\n"); return 1; }
    if (!g_xr_output)   { fprintf(stderr, "No wl_output\n"); return 1; }

    // Use output-reported size if available
    for (const auto &o : g_outputs) {
        if (o.output == g_xr_output && o.width > 0 && o.height > 0) {
            g_output_width  = o.width;
            g_output_height = o.height;
        }
    }
    fprintf(stderr, "xr-workspace: using output %dx%d\n", g_output_width, g_output_height);

    // ── Pose source ──
    if (!g_pose.open())
        fprintf(stderr, "WARNING: pose source '%s' unavailable — start xrDriver first\n", g_pose.name());
    else
        fprintf(stderr, "xr-workspace: pose source '%s' opened\n", g_pose.name());

    // ── Create surface + layer-shell ──
    g_surface = wl_compositor_create_surface(g_compositor);

    g_layer_surf = zwlr_layer_shell_v1_get_layer_surface(
        g_layer_shell, g_surface, g_xr_output,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "xr-workspace");

    zwlr_layer_surface_v1_set_size(g_layer_surf, 0, 0); // 0,0 = full output size
    zwlr_layer_surface_v1_set_anchor(g_layer_surf,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP    | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT   | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(g_layer_surf, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        g_layer_surf, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    zwlr_layer_surface_v1_add_listener(g_layer_surf, &g_layer_surf_listener, nullptr);

    wl_surface_commit(g_surface);
    wl_display_roundtrip(g_display); // receive configure
    if (!g_configured) { fprintf(stderr, "Layer surface not configured\n"); return 1; }

    // ── EGL + GL ──
    if (!egl_init()) return 1;

    // ── Capture context: shared globals + zero-copy dmabuf importer ──
    // Must be ready before gl_init() builds each monitor's capture source.
    g_egl_dmabuf.init(g_egl_display); // ok if it fails — shm fallback
    g_capture_ctx.display      = g_display;
    g_capture_ctx.shm          = g_shm;
    g_capture_ctx.screencopy   = g_screencopy;
    g_capture_ctx.ext_copy     = g_ext_copy;
    g_capture_ctx.ext_src      = g_ext_src;
    g_capture_ctx.dmabuf       = g_dmabuf;
    g_capture_ctx.egl          = g_egl_dmabuf.available() ? &g_egl_dmabuf : nullptr;
    g_capture_ctx.prefer_dmabuf = g_prefer_dmabuf;
    g_capture_ctx.find_output  = [](const std::string &name) -> wl_output * {
        for (auto &o : g_outputs) if (o.str_name == name) return o.output;
        return nullptr;
    };
    fprintf(stderr, "xr-workspace: capture backends — ext:%s wlr:%s dmabuf:%s\n",
            g_ext_copy ? "yes" : "no", g_screencopy ? "yes" : "no",
            g_capture_ctx.egl ? "yes" : "no");

    gl_init();

    // ── Precompute static geometry and projection ──
    precompute_proj();
    precompute_monitors();
    recenter();

    if (do_recenter_prompt) {
        fprintf(stderr, "Press ENTER to recenter...\n");
        getchar();
        recenter();
    }

    // ── Register global shortcut (Ctrl+Shift+C) ──
    // hyprland_global_shortcuts lets us receive this key even without keyboard focus.
    if (g_shortcuts_mgr) {
        g_shortcut_recenter = hyprland_global_shortcuts_manager_v1_register_shortcut(
            g_shortcuts_mgr,
            "recenter",           // id — unique per client
            "xr-workspace",       // app_id
            "Recenter XR view",   // description shown in compositor keybind UI
            "CTRL SHIFT C");      // suggested default trigger (Hyprland format)
        hyprland_global_shortcut_v1_add_listener(g_shortcut_recenter, &g_shortcut_listener, nullptr);
        wl_display_roundtrip(g_display);
        fprintf(stderr, "xr-workspace: global shortcut registered — Ctrl+Shift+C to recenter\n");
    } else {
        fprintf(stderr, "xr-workspace: hyprland_global_shortcuts not available\n"
                        "  Fallback: add to hyprland.conf:\n"
                        "    bind = CTRL SHIFT, C, exec, kill -USR1 $(pgrep xr-workspace)\n");
    }

    // ── Control socket (runtime API) ──
    if (!g_control.start(cfg.control_socket))
        fprintf(stderr, "xr-workspace: control API disabled (socket unavailable)\n");

    // ── Head-tracking output (mode B) ──
    apply_head_tracking(g_head_cfg);

    fprintf(stderr, "xr-workspace: rendering (Ctrl+Shift+C or SIGUSR1 to recenter)\n");

    // ── Render + control loop ──
    // Frame pacing comes from wl_surface.frame callbacks. We poll the Wayland fd
    // alongside the control socket so external apps can reconfigure monitors live
    // without adding threads or latency to the render path.
    const int wl_fd = wl_display_get_fd(g_display);
    schedule_frame();
    while (g_running) {
        // Stage a Wayland read; drain any already-queued events first.
        while (wl_display_prepare_read(g_display) != 0)
            wl_display_dispatch_pending(g_display);
        wl_display_flush(g_display);

        std::vector<struct pollfd> fds;
        fds.push_back({wl_fd, POLLIN, 0});
        g_control.add_pollfds(fds);

        const int n = poll(fds.data(), fds.size(), -1);
        if (n < 0) {
            wl_display_cancel_read(g_display);
            if (errno == EINTR) continue; // interrupted by SIGUSR1/SIGINT
            break;
        }

        if (fds[0].revents & POLLIN) {
            wl_display_read_events(g_display);
            wl_display_dispatch_pending(g_display);
        } else {
            wl_display_cancel_read(g_display);
        }

        g_control.handle(fds, handle_control);
    }

    // ── Cleanup ──
    fprintf(stderr, "xr-workspace: shutting down\n");
    g_control.stop();
    g_head_sink.close();
    if (g_frame_callback) { wl_callback_destroy(g_frame_callback); g_frame_callback = nullptr; }
    g_pose.close();
    for (auto &mon : g_monitors) { if (mon.capture) { mon.capture->stop(); delete mon.capture; mon.capture = nullptr; } }
    if (g_vbo)  glDeleteBuffers(1, &g_vbo);
    if (g_prog) glDeleteProgram(g_prog);
    if (g_egl_surface != EGL_NO_SURFACE) eglDestroySurface(g_egl_display, g_egl_surface);
    if (g_egl_context != EGL_NO_CONTEXT)  eglDestroyContext(g_egl_display, g_egl_context);
    if (g_egl_display != EGL_NO_DISPLAY)  eglTerminate(g_egl_display);
    if (g_egl_window) wl_egl_window_destroy(g_egl_window);
    if (g_shortcut_recenter) hyprland_global_shortcut_v1_destroy(g_shortcut_recenter);
    if (g_shortcuts_mgr)     hyprland_global_shortcuts_manager_v1_destroy(g_shortcuts_mgr);
    if (g_screencopy) zwlr_screencopy_manager_v1_destroy(g_screencopy);
    if (g_ext_copy)   ext_image_copy_capture_manager_v1_destroy(g_ext_copy);
    if (g_ext_src)    ext_output_image_capture_source_manager_v1_destroy(g_ext_src);
    if (g_dmabuf)     zwp_linux_dmabuf_v1_destroy(g_dmabuf);
    if (g_shm)        wl_shm_destroy(g_shm);
    if (g_layer_surf)  zwlr_layer_surface_v1_destroy(g_layer_surf);
    if (g_surface)     wl_surface_destroy(g_surface);
    if (g_layer_shell) zwlr_layer_shell_v1_destroy(g_layer_shell);
    if (g_compositor)  wl_compositor_destroy(g_compositor);
    wl_registry_destroy(registry);
    wl_display_disconnect(g_display);
    return 0;
}
