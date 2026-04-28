// xr-workspace: SpaceWalker-like virtual monitor renderer for VITURE XR glasses
// on bare Wayland/Hyprland. Reads IMU pose from /dev/shm/breezy_desktop_imu
// (written by xrDriver), renders head-locked virtual monitors via wlr-layer-shell
// using EGL/GLES2.
//
// Architecture:
//   1. Connect to Wayland, bind globals (wl_compositor, layer_shell, wl_output, shm)
//   2. Enumerate outputs, find the XR glasses output (first or named one)
//   3. Create layer-shell overlay surface on that output (OVERLAY layer, fullscreen)
//   4. Create EGL surface on the wl_egl_window
//   5. Each frame: read IMU quaternion → compute view matrix → render quads
//      representing N virtual monitors arranged in an arc
//   6. wl_shm path NOT used — EGL/GLES2 directly on native window

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
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <algorithm>
#include <chrono>

// ─── SHM layout (from breezy-desktop/kwin/src/breezydesktopeffect.cpp) ───────
// Byte offsets computed sequentially (OFFSET = prev_OFFSET + prev_SIZE * prev_COUNT)
static constexpr int SHM_VERSION_OFF          = 0;   // uint8
static constexpr int SHM_ENABLED_OFF          = 1;   // uint8
static constexpr int SHM_LOOK_AHEAD_CFG_OFF   = 2;   // float[4] = 16
static constexpr int SHM_DISPLAY_RES_OFF      = 18;  // uint32[2] = 8
static constexpr int SHM_DISPLAY_FOV_OFF      = 26;  // float[1] = 4
static constexpr int SHM_LENS_DIST_OFF        = 30;  // float[1] = 4
static constexpr int SHM_SBS_ENABLED_OFF      = 34;  // uint8
static constexpr int SHM_CUSTOM_BANNER_OFF    = 35;  // uint8
static constexpr int SHM_SMOOTH_FOLLOW_OFF    = 36;  // uint8
static constexpr int SHM_SF_ORIGIN_DATA_OFF   = 37;  // float[16] = 64
static constexpr int SHM_POSE_POS_OFF         = 101; // float[3] = 12
static constexpr int SHM_POSE_DATE_MS_OFF     = 113; // uint32[2] = 8
static constexpr int SHM_POSE_ORIENT_OFF      = 121; // float[16] = 64  (4 rows × 4 floats)
static constexpr int SHM_POSE_PARITY_OFF      = 185; // uint8
static constexpr int SHM_LENGTH               = 186;

// ─── Math types ──────────────────────────────────────────────────────────────
struct Vec3 { float x, y, z; };
struct Vec4 { float x, y, z, w; };
struct Quat { float x, y, z, w; }; // xyzw

struct Mat4 {
    float m[16]; // column-major
    static Mat4 identity() {
        Mat4 r{}; r.m[0]=r.m[5]=r.m[10]=r.m[15]=1.f; return r;
    }
};

static Mat4 mat4_mul(const Mat4 &a, const Mat4 &b) {
    Mat4 r{};
    for (int col = 0; col < 4; col++)
        for (int row = 0; row < 4; row++) {
            float v = 0;
            for (int k = 0; k < 4; k++)
                v += a.m[k*4+row] * b.m[col*4+k];
            r.m[col*4+row] = v;
        }
    return r;
}

static Mat4 quat_to_mat4(Quat q) {
    float x=q.x, y=q.y, z=q.z, w=q.w;
    Mat4 r = Mat4::identity();
    r.m[0]  = 1-2*(y*y+z*z);
    r.m[1]  =   2*(x*y+z*w);
    r.m[2]  =   2*(x*z-y*w);
    r.m[4]  =   2*(x*y-z*w);
    r.m[5]  = 1-2*(x*x+z*z);
    r.m[6]  =   2*(y*z+x*w);
    r.m[8]  =   2*(x*z+y*w);
    r.m[9]  =   2*(y*z-x*w);
    r.m[10] = 1-2*(x*x+y*y);
    return r;
}

static Mat4 mat4_perspective(float fovy_rad, float aspect, float near, float far) {
    Mat4 r{};
    float f = 1.f / tanf(fovy_rad * 0.5f);
    r.m[0]  = f / aspect;
    r.m[5]  = f;
    r.m[10] = (far + near) / (near - far);
    r.m[11] = -1.f;
    r.m[14] = (2.f * far * near) / (near - far);
    return r;
}

static Mat4 mat4_translate(float tx, float ty, float tz) {
    Mat4 r = Mat4::identity();
    r.m[12] = tx; r.m[13] = ty; r.m[14] = tz;
    return r;
}

static Mat4 mat4_rotate_y(float angle_rad) {
    Mat4 r = Mat4::identity();
    float c = cosf(angle_rad), s = sinf(angle_rad);
    r.m[0] = c; r.m[2] = s; r.m[8] = -s; r.m[10] = c;
    return r;
}

static Mat4 mat4_scale(float sx, float sy, float sz) {
    Mat4 r = Mat4::identity();
    r.m[0] = sx; r.m[5] = sy; r.m[10] = sz;
    return r;
}

static Mat4 mat4_inverse_rotation(const Mat4 &m) {
    // For pure rotation matrix, inverse = transpose
    Mat4 r{};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r.m[i*4+j] = m.m[j*4+i];
    r.m[3] = r.m[7] = r.m[11] = 0;
    r.m[15] = 1;
    return r;
}

// ─── Globals ─────────────────────────────────────────────────────────────────
static struct wl_display    *g_display   = nullptr;
static struct wl_registry   *g_registry  = nullptr;
static struct wl_compositor *g_compositor= nullptr;
static struct zwlr_layer_shell_v1 *g_layer_shell = nullptr;
static struct wl_output     *g_xr_output = nullptr;  // target output for glasses
static struct zxdg_output_manager_v1 *g_xdg_output_mgr = nullptr;

static int   g_output_width  = 1920;
static int   g_output_height = 1080;
static bool  g_configured    = false;
static bool  g_running       = true;

// EGL
static EGLDisplay g_egl_display = EGL_NO_DISPLAY;
static EGLContext g_egl_context = EGL_NO_CONTEXT;
static EGLSurface g_egl_surface = EGL_NO_SURFACE;
static struct wl_egl_window *g_egl_window = nullptr;

// Layer shell surface
static struct wl_surface        *g_surface       = nullptr;
static struct zwlr_layer_surface_v1 *g_layer_surf = nullptr;

// GL objects
static GLuint g_prog   = 0;
static GLuint g_vbo    = 0;
static GLint  g_u_mvp  = -1;
static GLint  g_u_color= -1;
static GLint  g_u_tex  = -1;
static GLint  g_u_use_tex = -1;

// Screen-capture texture per monitor (populated when screencopy is added later)
// For now we just draw colored placeholder quads.
static GLuint g_tex[3] = {0, 0, 0};

// Config (can be loaded from file later)
static int   g_monitor_count = 3;      // 1 or 3
static float g_arc_radius    = 2.0f;   // metres equivalent in world space
static float g_monitor_width = 1.0f;   // world units
static float g_monitor_height= 0.5625f;// 16:9
static float g_fov_deg       = 46.0f;  // VITURE XR glasses FOV
static std::string g_target_output_name = ""; // empty = first output found

// ─── IMU / Shared Memory ─────────────────────────────────────────────────────
static int   g_shm_fd   = -1;
static void *g_shm_ptr  = nullptr;

static bool imu_open() {
    g_shm_fd = open("/dev/shm/breezy_desktop_imu", O_RDONLY);
    if (g_shm_fd < 0) return false;
    g_shm_ptr = mmap(nullptr, SHM_LENGTH, PROT_READ, MAP_SHARED, g_shm_fd, 0);
    if (g_shm_ptr == MAP_FAILED) { close(g_shm_fd); g_shm_fd = -1; return false; }
    return true;
}

static Quat imu_read_quat() {
    if (!g_shm_ptr) return {0,0,0,1};
    const uint8_t *data = static_cast<const uint8_t*>(g_shm_ptr);

    // Check version and enabled flag
    uint8_t version = data[SHM_VERSION_OFF];
    uint8_t enabled = data[SHM_ENABLED_OFF];
    (void)version;
    if (!enabled) return {0,0,0,1};

    // Read T0 quaternion (first 4 floats of POSE_ORIENT)
    // Raw orientation is NWU: (x,y,z,w)
    // breezy converts NWU→EUS with: quat(w, -y, z, -x)
    float raw[4];
    memcpy(raw, data + SHM_POSE_ORIENT_OFF, sizeof(raw));

    // Convert NWU to EUS as breezy does
    Quat q;
    q.w =  raw[3];
    q.x = -raw[1];
    q.y =  raw[2];
    q.z = -raw[0];

    // Normalize
    float len = sqrtf(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
    if (len > 1e-6f) { q.x/=len; q.y/=len; q.z/=len; q.w/=len; }
    return q;
}

// ─── GL helpers ──────────────────────────────────────────────────────────────
static GLuint compile_shader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char buf[512]; glGetShaderInfoLog(s, 512, nullptr, buf);
        fprintf(stderr, "Shader compile error: %s\n", buf);
    }
    return s;
}

static GLuint link_program(const char *vert, const char *frag) {
    GLuint v = compile_shader(GL_VERTEX_SHADER,   vert);
    GLuint f = compile_shader(GL_FRAGMENT_SHADER, frag);
    GLuint p = glCreateProgram();
    glAttachShader(p, v); glAttachShader(p, f);
    glBindAttribLocation(p, 0, "a_pos");
    glBindAttribLocation(p, 1, "a_uv");
    glLinkProgram(p);
    GLint ok; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char buf[512]; glGetProgramInfoLog(p, 512, nullptr, buf);
        fprintf(stderr, "Program link error: %s\n", buf);
    }
    glDeleteShader(v); glDeleteShader(f);
    return p;
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

    g_prog = link_program(vert, frag);
    g_u_mvp     = glGetUniformLocation(g_prog, "u_mvp");
    g_u_color   = glGetUniformLocation(g_prog, "u_color");
    g_u_tex     = glGetUniformLocation(g_prog, "u_tex");
    g_u_use_tex = glGetUniformLocation(g_prog, "u_use_tex");

    // Unit quad: positions + UVs interleaved (x,y,z, u,v)
    // Two triangles for a centered [-0.5,+0.5] x [-0.5,+0.5] quad
    static const float quad[] = {
        -0.5f, -0.5f, 0.f,   0.f, 1.f,
         0.5f, -0.5f, 0.f,   1.f, 1.f,
         0.5f,  0.5f, 0.f,   1.f, 0.f,
        -0.5f, -0.5f, 0.f,   0.f, 1.f,
         0.5f,  0.5f, 0.f,   1.f, 0.f,
        -0.5f,  0.5f, 0.f,   0.f, 0.f,
    };
    glGenBuffers(1, &g_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    // Placeholder colored textures for monitors (RGB solid)
    static const uint8_t colors[3][4] = {
        {40, 60, 120, 255},   // left: dark blue
        {30, 90,  30, 255},   // center: dark green
        {90, 40,  40, 255},   // right: dark red
    };
    for (int i = 0; i < 3; i++) {
        glGenTextures(1, &g_tex[i]);
        glBindTexture(GL_TEXTURE_2D, g_tex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, colors[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
}

static void draw_quad(const Mat4 &mvp, float r, float g, float b, float a, GLuint tex) {
    glUseProgram(g_prog);
    glUniformMatrix4fv(g_u_mvp, 1, GL_FALSE, mvp.m);
    glUniform4f(g_u_color, r, g, b, a);

    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5*4, (void*)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5*4, (void*)(3*4));

    if (tex) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex);
        glUniform1i(g_u_tex, 0);
        glUniform1i(g_u_use_tex, 1);
    } else {
        glUniform1i(g_u_use_tex, 0);
    }

    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

// ─── Recenter (reset origin) ──────────────────────────────────────────────────
static Quat g_origin_inv = {0, 0, 0, 1}; // inverse of the pose at recenter

static Quat quat_mul(Quat a, Quat b) {
    return {
        a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
        a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
        a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
        a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z
    };
}

static Quat quat_conj(Quat q) { return {-q.x, -q.y, -q.z, q.w}; }

static void recenter(Quat current) {
    g_origin_inv = quat_conj(current);
    fprintf(stderr, "xr-workspace: recentered\n");
}

// ─── Render frame ─────────────────────────────────────────────────────────────
static void render_frame() {
    glViewport(0, 0, g_output_width, g_output_height);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Build projection matrix
    float fov_rad = g_fov_deg * (float)M_PI / 180.f;
    float aspect  = (float)g_output_width / (float)g_output_height;
    Mat4 proj = mat4_perspective(fov_rad, aspect, 0.1f, 100.f);

    // Read head orientation
    Quat head_quat = imu_read_quat();
    Quat relative  = quat_mul(g_origin_inv, head_quat);
    Mat4 head_rot  = quat_to_mat4(relative);
    // View matrix = inverse of head rotation (pure rotation, so transpose)
    Mat4 view = mat4_inverse_rotation(head_rot);
    Mat4 pv   = mat4_mul(proj, view);

    // Draw N monitors in an arc at arc_radius, centered around Z=-arc_radius
    // Monitors are evenly spaced ±(n-1)/2 * angle_step apart in yaw
    // With 1 monitor: straight ahead at (0,0,-arc_radius)
    // With 3 monitors: -45°, 0°, +45°

    static const float monitor_angles_3[] = { -45.f, 0.f, 45.f };
    static const float monitor_angles_1[] = { 0.f };

    const float *angles = (g_monitor_count >= 3) ? monitor_angles_3 : monitor_angles_1;
    int count = (g_monitor_count >= 3) ? 3 : 1;

    for (int i = 0; i < count; i++) {
        float angle_rad = angles[i] * (float)M_PI / 180.f;
        // Position the monitor on the arc
        float px = g_arc_radius * sinf(angle_rad);
        float py = 0.f;
        float pz = -g_arc_radius * cosf(angle_rad);

        // Model: place, rotate so face points inward
        Mat4 translate = mat4_translate(px, py, pz);
        Mat4 rotate    = mat4_rotate_y(-angle_rad); // face center
        Mat4 scale     = mat4_scale(g_monitor_width, g_monitor_height, 1.f);
        Mat4 model     = mat4_mul(translate, mat4_mul(rotate, scale));
        Mat4 mvp       = mat4_mul(pv, model);

        GLuint tex = (i < 3) ? g_tex[i] : 0;
        draw_quad(mvp, 1.f, 1.f, 1.f, 1.f, tex);

        // Draw a thin border around the monitor
        // Scale slightly larger, darker color, no depth write
        Mat4 border_scale = mat4_scale(1.04f, 1.04f, 1.f);
        Mat4 border_model = mat4_mul(translate, mat4_mul(rotate, mat4_mul(border_scale, scale)));
        Mat4 border_mvp   = mat4_mul(pv, border_model);
        // Push back slightly to avoid z-fighting
        Mat4 border_push  = mat4_translate(0,0,0.001f);
        border_mvp = mat4_mul(border_mvp, border_push);
        draw_quad(border_mvp, 0.2f, 0.2f, 0.2f, 0.8f, 0);
    }

    eglSwapBuffers(g_egl_display, g_egl_surface);
}

// ─── EGL setup ────────────────────────────────────────────────────────────────
static bool egl_init() {
    g_egl_display = eglGetDisplay((EGLNativeDisplayType)g_display);
    if (g_egl_display == EGL_NO_DISPLAY) {
        fprintf(stderr, "eglGetDisplay failed\n"); return false;
    }
    EGLint major, minor;
    if (!eglInitialize(g_egl_display, &major, &minor)) {
        fprintf(stderr, "eglInitialize failed\n"); return false;
    }
    fprintf(stderr, "EGL %d.%d\n", major, minor);

    eglBindAPI(EGL_OPENGL_ES_API);

    static const EGLint cfg_attrs[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE,    8,
        EGL_GREEN_SIZE,  8,
        EGL_BLUE_SIZE,   8,
        EGL_ALPHA_SIZE,  8,
        EGL_DEPTH_SIZE,  16,
        EGL_NONE
    };
    EGLConfig cfg;
    EGLint num;
    if (!eglChooseConfig(g_egl_display, cfg_attrs, &cfg, 1, &num) || num < 1) {
        fprintf(stderr, "eglChooseConfig failed\n"); return false;
    }

    static const EGLint ctx_attrs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
    g_egl_context = eglCreateContext(g_egl_display, cfg, EGL_NO_CONTEXT, ctx_attrs);
    if (g_egl_context == EGL_NO_CONTEXT) {
        fprintf(stderr, "eglCreateContext failed\n"); return false;
    }

    g_egl_window = wl_egl_window_create(g_surface, g_output_width, g_output_height);
    g_egl_surface = eglCreateWindowSurface(g_egl_display, cfg,
        (EGLNativeWindowType)g_egl_window, nullptr);
    if (g_egl_surface == EGL_NO_SURFACE) {
        fprintf(stderr, "eglCreateWindowSurface failed\n"); return false;
    }

    eglMakeCurrent(g_egl_display, g_egl_surface, g_egl_surface, g_egl_context);
    return true;
}

// ─── Wayland callbacks ────────────────────────────────────────────────────────
static void layer_surface_configure(void */*data*/,
    struct zwlr_layer_surface_v1 *surf,
    uint32_t serial, uint32_t width, uint32_t height)
{
    if (width  > 0) g_output_width  = (int)width;
    if (height > 0) g_output_height = (int)height;
    zwlr_layer_surface_v1_ack_configure(surf, serial);
    g_configured = true;
    fprintf(stderr, "xr-workspace: layer surface configured %dx%d\n",
            g_output_width, g_output_height);

    // Resize EGL window if already created
    if (g_egl_window)
        wl_egl_window_resize(g_egl_window, g_output_width, g_output_height, 0, 0);
}

static void layer_surface_closed(void */*data*/,
    struct zwlr_layer_surface_v1 */*surf*/)
{
    fprintf(stderr, "xr-workspace: layer surface closed\n");
    g_running = false;
}

static const zwlr_layer_surface_v1_listener g_layer_surf_listener = {
    .configure = layer_surface_configure,
    .closed    = layer_surface_closed,
};

// Output listener — capture geometry
static void output_geometry(void*,struct wl_output*,int,int,int,int,int,const char*,const char*,int32_t){}
static void output_mode(void*,struct wl_output*,uint32_t,int32_t w,int32_t h,int32_t){
    // Only use if not yet configured by layer-shell
    if (!g_configured) {
        g_output_width  = w;
        g_output_height = h;
    }
}
static void output_done(void*,struct wl_output*){}
static void output_scale(void*,struct wl_output*,int32_t){}
static void output_name(void*,struct wl_output*,const char *name){
    if (!g_target_output_name.empty() && name &&
        g_target_output_name == name && !g_xr_output) {
        // We identified the right output — but we already stored the pointer
        // in registry_global. This is informational only.
        fprintf(stderr, "xr-workspace: found target output '%s'\n", name);
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

// Registry listener
static int g_output_count = 0;

static void registry_global(void */*data*/, struct wl_registry *registry,
    uint32_t name, const char *interface, uint32_t version)
{
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        g_compositor = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface,
                             std::min(version, 4u)));
    } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        g_layer_shell = static_cast<zwlr_layer_shell_v1*>(
            wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface,
                             std::min(version, 4u)));
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        struct wl_output *out = static_cast<wl_output*>(
            wl_registry_bind(registry, name, &wl_output_interface,
                             std::min(version, 4u)));
        wl_output_add_listener(out, &g_output_listener, nullptr);
        // First output: use as default. Last output: use as XR (glasses usually
        // appear as an additional output after the main monitor)
        g_output_count++;
        // We take the last output advertised, assuming that's the glasses.
        // This will be overridden by --output NAME flag.
        g_xr_output = out;
    } else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
        g_xdg_output_mgr = static_cast<zxdg_output_manager_v1*>(
            wl_registry_bind(registry, name, &zxdg_output_manager_v1_interface,
                             std::min(version, 3u)));
    }
}

static void registry_global_remove(void */*data*/, struct wl_registry */*registry*/,
    uint32_t /*name*/) {}

static const wl_registry_listener g_registry_listener = {
    .global        = registry_global,
    .global_remove = registry_global_remove,
};

// ─── Signal handler ───────────────────────────────────────────────────────────
#include <signal.h>
static void sig_handler(int) { g_running = false; }

// ─── Main ─────────────────────────────────────────────────────────────────────
static void print_usage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  --monitors N       Number of virtual monitors: 1 or 3 (default: 3)\n"
        "  --output NAME      Wayland output name to use (e.g. DP-3, HDMI-A-1)\n"
        "  --radius R         Arc radius in world units (default: 2.0)\n"
        "  --fov DEG          Glasses FOV degrees (default: 46.0)\n"
        "  --recenter         Press Enter to recenter after launch\n"
        "  --help             Show this help\n\n"
        "Controls:\n"
        "  SIGINT (Ctrl+C)    Quit\n"
        "  SIGUSR1            Recenter\n",
        argv0);
}

int main(int argc, char **argv) {
    // Parse args
    bool do_recenter_prompt = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) { print_usage(argv[0]); return 0; }
        else if (strcmp(argv[i], "--monitors") == 0 && i+1 < argc)
            g_monitor_count = atoi(argv[++i]);
        else if (strcmp(argv[i], "--output") == 0 && i+1 < argc)
            g_target_output_name = argv[++i];
        else if (strcmp(argv[i], "--radius") == 0 && i+1 < argc)
            g_arc_radius = strtof(argv[++i], nullptr);
        else if (strcmp(argv[i], "--fov") == 0 && i+1 < argc)
            g_fov_deg = strtof(argv[++i], nullptr);
        else if (strcmp(argv[i], "--recenter") == 0)
            do_recenter_prompt = true;
        else { fprintf(stderr, "Unknown option: %s\n", argv[i]); print_usage(argv[0]); return 1; }
    }

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGUSR1, [](int){ recenter(imu_read_quat()); });

    fprintf(stderr, "xr-workspace starting: %d monitor(s), output='%s'\n",
            g_monitor_count, g_target_output_name.c_str());

    // ── Wayland connect ──
    g_display = wl_display_connect(nullptr);
    if (!g_display) { fprintf(stderr, "Cannot connect to Wayland display\n"); return 1; }

    g_registry = wl_display_get_registry(g_display);
    wl_registry_add_listener(g_registry, &g_registry_listener, nullptr);
    wl_display_roundtrip(g_display);
    wl_display_roundtrip(g_display); // second pass for output events

    if (!g_compositor) { fprintf(stderr, "No wl_compositor\n"); return 1; }
    if (!g_layer_shell) { fprintf(stderr, "No zwlr_layer_shell_v1 — Hyprland must support it\n"); return 1; }
    if (!g_xr_output)   { fprintf(stderr, "No wl_output found\n"); return 1; }

    fprintf(stderr, "xr-workspace: %d output(s) found, using last one. "
            "Pass --output NAME to choose specific output.\n", g_output_count);

    // ── IMU ──
    if (!imu_open()) {
        fprintf(stderr, "WARNING: /dev/shm/breezy_desktop_imu not found. "
                "Start xrDriver first and plug in glasses. Running with static view.\n");
    } else {
        fprintf(stderr, "xr-workspace: IMU shared memory opened\n");
    }

    // ── Create surface ──
    g_surface = wl_compositor_create_surface(g_compositor);

    // ── Layer shell surface ──
    // Use OVERLAY layer so it sits above everything
    g_layer_surf = zwlr_layer_shell_v1_get_layer_surface(
        g_layer_shell, g_surface, g_xr_output,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
        "xr-workspace");

    zwlr_layer_surface_v1_set_size(g_layer_surf, 0, 0); // 0,0 = full output
    zwlr_layer_surface_v1_set_anchor(g_layer_surf,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP   |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM|
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT  |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(g_layer_surf, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        g_layer_surf, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    zwlr_layer_surface_v1_add_listener(g_layer_surf, &g_layer_surf_listener, nullptr);

    wl_surface_commit(g_surface);
    wl_display_roundtrip(g_display); // triggers configure

    if (!g_configured) {
        fprintf(stderr, "ERROR: Layer surface not configured — compositor did not send configure\n");
        return 1;
    }

    // ── EGL + GL ──
    if (!egl_init()) return 1;
    gl_init();

    // ── Recenter at startup ──
    recenter(imu_read_quat());
    if (do_recenter_prompt) {
        fprintf(stderr, "Press ENTER to recenter now...\n");
        getchar();
        recenter(imu_read_quat());
    }

    fprintf(stderr, "xr-workspace: rendering loop started (send SIGUSR1 to recenter)\n");

    // ── Render loop ──
    auto last_frame = std::chrono::steady_clock::now();
    while (g_running) {
        if (wl_display_dispatch_pending(g_display) < 0) break;
        wl_display_flush(g_display);

        render_frame();

        // Cap at ~60fps
        auto now = std::chrono::steady_clock::now();
        auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(now - last_frame).count();
        if (elapsed_us < 16667)
            usleep((useconds_t)(16667 - elapsed_us));
        last_frame = std::chrono::steady_clock::now();
    }

    // ── Cleanup ──
    fprintf(stderr, "xr-workspace: shutting down\n");
    if (g_shm_ptr && g_shm_ptr != MAP_FAILED)
        munmap(g_shm_ptr, SHM_LENGTH);
    if (g_shm_fd >= 0) close(g_shm_fd);

    if (g_egl_surface != EGL_NO_SURFACE)
        eglDestroySurface(g_egl_display, g_egl_surface);
    if (g_egl_context != EGL_NO_CONTEXT)
        eglDestroyContext(g_egl_display, g_egl_context);
    if (g_egl_display != EGL_NO_DISPLAY)
        eglTerminate(g_egl_display);
    if (g_egl_window)
        wl_egl_window_destroy(g_egl_window);

    if (g_layer_surf)  zwlr_layer_surface_v1_destroy(g_layer_surf);
    if (g_surface)     wl_surface_destroy(g_surface);
    if (g_layer_shell) zwlr_layer_shell_v1_destroy(g_layer_shell);
    if (g_compositor)  wl_compositor_destroy(g_compositor);
    wl_registry_destroy(g_registry);
    wl_display_disconnect(g_display);
    return 0;
}
