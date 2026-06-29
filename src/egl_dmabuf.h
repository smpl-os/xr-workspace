// egl_dmabuf.h — zero-copy capture support: allocate a gbm buffer object,
// import it as an EGLImage-backed GL texture, AND wrap the same dmabuf in a
// wl_buffer (via linux-dmabuf) so a compositor screencopy/ext capture writes
// straight into the texture the renderer samples. No CPU copy in the hot path.
//
// Compiled to a no-op when gbm is unavailable (HAVE_GBM undefined): every
// method fails gracefully so callers fall back to the shm path.
#pragma once

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <cstdint>

struct wl_buffer;
struct zwp_linux_dmabuf_v1;

// One dmabuf-backed frame: a GL texture plus the wl_buffer the compositor fills.
struct DmabufFrame {
    GLuint     tex    = 0;
    void      *image  = nullptr; // EGLImageKHR
    void      *bo     = nullptr; // gbm_bo*
    wl_buffer *buffer = nullptr;
    int        width  = 0;
    int        height = 0;
    uint32_t   fourcc = 0;
    bool valid() const { return tex != 0 && buffer != nullptr; }
};

class EglDmabuf {
public:
    ~EglDmabuf();

    // Load EGL/GLES dmabuf extension entry points and open a render node.
    // Returns false if the platform can't do zero-copy import.
    bool init(EGLDisplay dpy);
    bool available() const { return ok_; }

    // Allocate a gbm buffer of (w,h,fourcc), import it as a GL texture and wrap
    // it in a wl_buffer. Returns false on any failure (caller uses shm instead).
    bool create(int w, int h, uint32_t fourcc, zwp_linux_dmabuf_v1 *dmabuf,
                DmabufFrame &out);
    void destroy(DmabufFrame &f);

private:
    EGLDisplay dpy_    = EGL_NO_DISPLAY;
    int        drm_fd_ = -1;
    void      *gbm_    = nullptr; // gbm_device*
    bool       ok_     = false;
};
