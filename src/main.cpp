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

// ─── SHM layout (from breezy-desktop/kwin/src/breezydesktopeffect.cpp) ───────
static constexpr int SHM_VERSION_OFF     = 0;
static constexpr int SHM_ENABLED_OFF     = 1;
static constexpr int SHM_POSE_ORIENT_OFF = 121; // float[16]: 4 rows × 4 floats
static constexpr int SHM_LENGTH          = 186;

// ─── Math ────────────────────────────────────────────────────────────────────
struct Quat { float x, y, z, w; };

struct Mat4 {
    float m[16]; // column-major
    static Mat4 identity() {
        Mat4 r{}; r.m[0]=r.m[5]=r.m[10]=r.m[15]=1.f; return r;
    }
};

// Compute C = A * B (column-major 4×4)
static inline Mat4 mat4_mul(const Mat4 &a, const Mat4 &b) {
    Mat4 r{};
    for (int col = 0; col < 4; col++) {
        const float *bc = &b.m[col*4];
        float *rc = &r.m[col*4];
        for (int row = 0; row < 4; row++) {
            rc[row] = a.m[0*4+row]*bc[0] + a.m[1*4+row]*bc[1]
                    + a.m[2*4+row]*bc[2] + a.m[3*4+row]*bc[3];
        }
    }
    return r;
}

static inline Mat4 quat_to_mat4(Quat q) {
    const float x=q.x, y=q.y, z=q.z, w=q.w;
    Mat4 r = Mat4::identity();
    r.m[0]  = 1.f-2.f*(y*y+z*z);  r.m[4]  =     2.f*(x*y-z*w);  r.m[8]  =     2.f*(x*z+y*w);
    r.m[1]  =     2.f*(x*y+z*w);  r.m[5]  = 1.f-2.f*(x*x+z*z);  r.m[9]  =     2.f*(y*z-x*w);
    r.m[2]  =     2.f*(x*z-y*w);  r.m[6]  =     2.f*(y*z+x*w);  r.m[10] = 1.f-2.f*(x*x+y*y);
    return r;
}

// Inverse of a pure rotation matrix = transpose of the 3×3 block
static inline Mat4 mat4_rot_inverse(const Mat4 &m) {
    Mat4 r = Mat4::identity();
    r.m[0]=m.m[0]; r.m[4]=m.m[1]; r.m[8] =m.m[2];
    r.m[1]=m.m[4]; r.m[5]=m.m[5]; r.m[9] =m.m[6];
    r.m[2]=m.m[8]; r.m[6]=m.m[9]; r.m[10]=m.m[10];
    return r;
}

static inline Mat4 mat4_perspective(float fovy_rad, float aspect, float near, float far) {
    Mat4 r{};
    const float f = 1.f / tanf(fovy_rad * 0.5f);
    r.m[0]  = f / aspect;
    r.m[5]  = f;
    r.m[10] = (far + near) / (near - far);
    r.m[11] = -1.f;
    r.m[14] = (2.f * far * near) / (near - far);
    return r;
}

static inline Quat quat_mul(Quat a, Quat b) {
    return {
        a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
        a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
        a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
        a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z
    };
}

static inline Quat quat_conj(Quat q) { return {-q.x, -q.y, -q.z, q.w}; }

// ─── Config ───────────────────────────────────────────────────────────────────
static int         g_monitor_count = 3;
static float       g_arc_radius    = 2.0f;
static float       g_monitor_width = 1.0f;
static float       g_monitor_height= 0.5625f; // 16:9
static float       g_fov_deg       = 46.0f;
static std::string g_target_output_name;

// ─── Wayland globals ──────────────────────────────────────────────────────────
static struct wl_display    *g_display    = nullptr;
static struct wl_compositor *g_compositor = nullptr;
static struct zwlr_layer_shell_v1 *g_layer_shell = nullptr;
static struct wl_output     *g_xr_output  = nullptr;

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
static GLuint g_tex[3]    = {0, 0, 0};

// ─── Signal flags — async-signal-safe ────────────────────────────────────────
static volatile sig_atomic_t g_running     = 1;
static volatile sig_atomic_t g_do_recenter = 0;

static void sig_quit(int)     { g_running = 0; }
static void sig_recenter(int) { g_do_recenter = 1; } // main thread does the work

// ─── Precomputed per-monitor geometry ─────────────────────────────────────────
// These are all STATIC (monitor positions on the arc never change at runtime).
// Built once in precompute_monitors(). Each monitor: 2 draw calls (quad + border).
struct MonitorStatic {
    Mat4 model;        // full model matrix for the screen quad
    Mat4 border_model; // model matrix for the slightly-larger border quad
};
static MonitorStatic g_mons[3];
static int           g_mon_count = 0;

// Cached projection matrix — rebuilt only when output size changes
static Mat4 g_proj;

// ─── IMU shared memory ────────────────────────────────────────────────────────
static int   g_shm_fd  = -1;
static void *g_shm_ptr = nullptr;

static bool imu_open() {
    g_shm_fd = open("/dev/shm/breezy_desktop_imu", O_RDONLY);
    if (g_shm_fd < 0) return false;
    g_shm_ptr = mmap(nullptr, SHM_LENGTH, PROT_READ, MAP_SHARED, g_shm_fd, 0);
    if (g_shm_ptr == MAP_FAILED) { close(g_shm_fd); g_shm_fd = -1; return false; }
    return true;
}

// Read quaternion right before rendering — maximum freshness, minimum latency.
// xrDriver delivers a unit quaternion so no normalization needed.
static inline Quat imu_read_quat() {
    if (!g_shm_ptr) return {0.f, 0.f, 0.f, 1.f};
    const uint8_t *data = static_cast<const uint8_t*>(g_shm_ptr);
    if (!data[SHM_ENABLED_OFF]) return {0.f, 0.f, 0.f, 1.f};
    // T0 quaternion: first 4 floats of POSE_ORIENT block
    // Raw NWU (x,y,z,w) → EUS: (w=raw[3], x=-raw[1], y=raw[2], z=-raw[0])
    float raw[4];
    memcpy(raw, data + SHM_POSE_ORIENT_OFF, 16u);
    return { -raw[1], raw[2], -raw[0], raw[3] };
}

// ─── Recenter ─────────────────────────────────────────────────────────────────
static Quat g_origin_inv = {0.f, 0.f, 0.f, 1.f};

static void recenter() {
    g_origin_inv = quat_conj(imu_read_quat());
    fprintf(stderr, "xr-workspace: recentered\n");
}

// ─── Static data precomputation ───────────────────────────────────────────────
// Monitor model matrices are built by hand (cheaper than chained mat4_mul).
// Column-major layout: T * Ry(-a) * Scale, computed analytically.
static void precompute_monitors() {
    static const float angles_3[] = { -45.f, 0.f, 45.f };
    static const float angles_1[] = {   0.f };
    const float *angles = (g_monitor_count >= 3) ? angles_3 : angles_1;
    g_mon_count = (g_monitor_count >= 3) ? 3 : 1;

    const float to_rad = (float)M_PI / 180.f;

    for (int i = 0; i < g_mon_count; i++) {
        const float a   = angles[i] * to_rad;
        const float px  = g_arc_radius * sinf(a);
        const float pz  = -g_arc_radius * cosf(a);
        const float ca  = cosf(-a), sa = sinf(-a);

        // Model = Translate(px,0,pz) * RotY(-a) * Scale(w,h,1)
        // Computed in one shot without intermediate matrix objects:
        //   Col 0: (ca*w,  0, -sa*w, 0)
        //   Col 1: (0,     h,  0,    0)
        //   Col 2: (sa,    0,  ca,   0)
        //   Col 3: (px,    0,  pz,   1)
        Mat4 &mod = g_mons[i].model;
        mod = Mat4::identity();
        mod.m[0]=ca*g_monitor_width;  mod.m[4]=0.f;              mod.m[8] =sa;  mod.m[12]=px;
        mod.m[1]=0.f;                 mod.m[5]=g_monitor_height;  mod.m[9] =0.f; mod.m[13]=0.f;
        mod.m[2]=-sa*g_monitor_width; mod.m[6]=0.f;              mod.m[10]=ca;  mod.m[14]=pz;
        mod.m[3]=0.f;                 mod.m[7]=0.f;              mod.m[11]=0.f; mod.m[15]=1.f;

        // Border: 4% larger in screen-plane, pushed 0.002 behind the monitor.
        // "Behind" = along the monitor's local +Z which maps to world (sa, 0, ca).
        // So world position shifts: px += sa*push, pz += ca*push.
        const float push = 0.002f;
        const float bw   = g_monitor_width  * 1.04f;
        const float bh   = g_monitor_height * 1.04f;
        const float bpx  = px + sa * push;
        const float bpz  = pz + ca * push;

        Mat4 &bm = g_mons[i].border_model;
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
}

// ─── GL init ──────────────────────────────────────────────────────────────────
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
        varying vec2 v_uv;
        void main() {
            v_uv = a_uv;
            gl_Position = u_mvp * vec4(a_pos, 1.0);
        }
    )glsl";

    const char *frag = R"glsl(
        precision mediump float;
        varying vec2 v_uv;
        uniform vec4 u_color;
        uniform sampler2D u_tex;
        uniform int u_use_tex;
        void main() {
            if (u_use_tex != 0) {
                gl_FragColor = texture2D(u_tex, v_uv) * u_color;
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

    g_u_mvp     = glGetUniformLocation(g_prog, "u_mvp");
    g_u_color   = glGetUniformLocation(g_prog, "u_color");
    g_u_tex     = glGetUniformLocation(g_prog, "u_tex");
    g_u_use_tex = glGetUniformLocation(g_prog, "u_use_tex");

    // Unit quad: positions + UVs interleaved — static, uploaded once
    static const float quad[] = {
        -0.5f,-0.5f,0.f, 0.f,1.f,
         0.5f,-0.5f,0.f, 1.f,1.f,
         0.5f, 0.5f,0.f, 1.f,0.f,
        -0.5f,-0.5f,0.f, 0.f,1.f,
         0.5f, 0.5f,0.f, 1.f,0.f,
        -0.5f, 0.5f,0.f, 0.f,0.f,
    };
    glGenBuffers(1, &g_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    // Set vertex attribute pointers ONCE — constant for the lifetime of the app
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5*4, (void*)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5*4, (void*)(3*4));
    // Leave the VBO bound permanently — we never rebind a different one

    // Placeholder 1×1 solid-colour textures
    static const uint8_t colors[3][4] = {
        { 40,  60, 120, 255},
        { 30,  90,  30, 255},
        { 90,  40,  40, 255},
    };
    for (int i = 0; i < 3; i++) {
        glGenTextures(1, &g_tex[i]);
        glBindTexture(GL_TEXTURE_2D, g_tex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, colors[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
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
static void render_frame() {
    // Deferred recenter from SIGUSR1 — safe to call here in main thread
    if (g_do_recenter) { g_do_recenter = 0; recenter(); }

    // IMU read as LATE as possible before vertex transform = minimum motion-to-photon latency
    const Quat head   = imu_read_quat();
    const Quat rel    = quat_mul(g_origin_inv, head);
    const Mat4 head_m = quat_to_mat4(rel);
    const Mat4 view   = mat4_rot_inverse(head_m);
    const Mat4 pv     = mat4_mul(g_proj, view);

    glViewport(0, 0, g_output_width, g_output_height);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    for (int i = 0; i < g_mon_count; i++) {
        const MonitorStatic &ms = g_mons[i];

        // Screen quad
        const Mat4 mvp = mat4_mul(pv, ms.model);
        glUniformMatrix4fv(g_u_mvp, 1, GL_FALSE, mvp.m);
        glUniform4f(g_u_color, 1.f, 1.f, 1.f, 1.f);
        glBindTexture(GL_TEXTURE_2D, g_tex[i]);
        glUniform1i(g_u_use_tex, 1);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        // Border quad (precomputed — border_model already has correct size + push)
        const Mat4 bmvp = mat4_mul(pv, ms.border_model);
        glUniformMatrix4fv(g_u_mvp, 1, GL_FALSE, bmvp.m);
        glUniform4f(g_u_color, 0.2f, 0.2f, 0.2f, 0.9f);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUniform1i(g_u_use_tex, 0);
        glDrawArrays(GL_TRIANGLES, 0, 6);
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
    }
}
static void registry_global_remove(void*, struct wl_registry*, uint32_t) {}

static const wl_registry_listener g_registry_listener = {
    .global        = registry_global,
    .global_remove = registry_global_remove,
};

// ─── Usage ────────────────────────────────────────────────────────────────────
static void print_usage(const char *a) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  --monitors N    Virtual monitors: 1 or 3 (default: 3)\n"
        "  --output NAME   Wayland output name e.g. DP-3 (default: last output)\n"
        "  --radius R      Arc radius in world units (default: 2.0)\n"
        "  --fov DEG       Glasses horizontal FOV in degrees (default: 46.0)\n"
        "  --recenter      Prompt to recenter before rendering\n"
        "  --help\n\n"
        "Signals:\n"
        "  SIGINT/SIGTERM  Quit\n"
        "  SIGUSR1         Recenter view to current head position\n", a);
}

// ─── Main ─────────────────────────────────────────────────────────────────────
int main(int argc, char **argv) {
    bool do_recenter_prompt = false;
    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i],"--help"))                    { print_usage(argv[0]); return 0; }
        else if (!strcmp(argv[i],"--monitors") && i+1 < argc)  g_monitor_count = atoi(argv[++i]);
        else if (!strcmp(argv[i],"--output")   && i+1 < argc)  g_target_output_name = argv[++i];
        else if (!strcmp(argv[i],"--radius")   && i+1 < argc)  g_arc_radius = strtof(argv[++i], nullptr);
        else if (!strcmp(argv[i],"--fov")      && i+1 < argc)  g_fov_deg    = strtof(argv[++i], nullptr);
        else if (!strcmp(argv[i],"--recenter"))                 do_recenter_prompt = true;
        else { fprintf(stderr, "Unknown option: %s\n", argv[i]); print_usage(argv[0]); return 1; }
    }

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

    // ── IMU ──
    if (!imu_open())
        fprintf(stderr, "WARNING: no /dev/shm/breezy_desktop_imu — start xrDriver first\n");
    else
        fprintf(stderr, "xr-workspace: IMU shared memory opened\n");

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

    fprintf(stderr, "xr-workspace: rendering (SIGUSR1=recenter, SIGINT=quit)\n");

    // ── Render loop — driven by wl_surface.frame callbacks ──
    // wl_display_dispatch() blocks until the compositor sends a frame callback
    // (i.e. at vsync), then on_frame() runs render_frame() and schedules the next.
    // This gives compositor-synchronised rendering with minimum busy-wait overhead
    // and minimum motion-to-photon latency (IMU read happens inside on_frame).
    schedule_frame();
    while (g_running) {
        if (wl_display_dispatch(g_display) < 0) break;
    }

    // ── Cleanup ──
    fprintf(stderr, "xr-workspace: shutting down\n");
    if (g_frame_callback) { wl_callback_destroy(g_frame_callback); g_frame_callback = nullptr; }
    if (g_shm_ptr && g_shm_ptr != MAP_FAILED) munmap(g_shm_ptr, SHM_LENGTH);
    if (g_shm_fd >= 0) close(g_shm_fd);
    for (int i = 0; i < 3; i++) if (g_tex[i]) glDeleteTextures(1, &g_tex[i]);
    if (g_vbo)  glDeleteBuffers(1, &g_vbo);
    if (g_prog) glDeleteProgram(g_prog);
    if (g_egl_surface != EGL_NO_SURFACE) eglDestroySurface(g_egl_display, g_egl_surface);
    if (g_egl_context != EGL_NO_CONTEXT)  eglDestroyContext(g_egl_display, g_egl_context);
    if (g_egl_display != EGL_NO_DISPLAY)  eglTerminate(g_egl_display);
    if (g_egl_window) wl_egl_window_destroy(g_egl_window);
    if (g_layer_surf)  zwlr_layer_surface_v1_destroy(g_layer_surf);
    if (g_surface)     wl_surface_destroy(g_surface);
    if (g_layer_shell) zwlr_layer_shell_v1_destroy(g_layer_shell);
    if (g_compositor)  wl_compositor_destroy(g_compositor);
    wl_registry_destroy(registry);
    wl_display_disconnect(g_display);
    return 0;
}
