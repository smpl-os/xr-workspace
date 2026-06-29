// egl_dmabuf.cpp — see egl_dmabuf.h.
#include "egl_dmabuf.h"

#include <cstdio>
#include <cstring>

#if HAVE_GBM

#include <EGL/eglext.h>
#include <GLES2/gl2ext.h>
#include <fcntl.h>
#include <unistd.h>
#include <gbm.h>

// linux-dmabuf client stubs (generated).
#include "linux-dmabuf-unstable-v1-client.h"

#ifndef DRM_FORMAT_MOD_INVALID
#define DRM_FORMAT_MOD_INVALID ((1ULL << 56) - 1)
#endif

namespace {
PFNEGLCREATEIMAGEKHRPROC            p_eglCreateImageKHR  = nullptr;
PFNEGLDESTROYIMAGEKHRPROC           p_eglDestroyImageKHR = nullptr;
PFNGLEGLIMAGETARGETTEXTURE2DOESPROC p_glEGLImageTex      = nullptr;
} // namespace

EglDmabuf::~EglDmabuf() {
    if (gbm_)            gbm_device_destroy(static_cast<gbm_device *>(gbm_));
    if (drm_fd_ >= 0)    close(drm_fd_);
}

bool EglDmabuf::init(EGLDisplay dpy) {
    dpy_ = dpy;

    // Need the EGLImage + dmabuf import extensions.
    const char *exts = eglQueryString(dpy, EGL_EXTENSIONS);
    if (!exts ||
        !strstr(exts, "EGL_KHR_image_base") ||
        !strstr(exts, "EGL_EXT_image_dma_buf_import")) {
        fprintf(stderr, "egl_dmabuf: dma_buf_import EGL extension missing\n");
        return false;
    }

    p_eglCreateImageKHR  = (PFNEGLCREATEIMAGEKHRPROC) eglGetProcAddress("eglCreateImageKHR");
    p_eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC) eglGetProcAddress("eglDestroyImageKHR");
    p_glEGLImageTex      = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC) eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if (!p_eglCreateImageKHR || !p_eglDestroyImageKHR || !p_glEGLImageTex) {
        fprintf(stderr, "egl_dmabuf: required entry points unavailable\n");
        return false;
    }

    // Open a render node for gbm allocations.
    static const char *nodes[] = {"/dev/dri/renderD128", "/dev/dri/renderD129", "/dev/dri/card0"};
    for (const char *n : nodes) {
        drm_fd_ = open(n, O_RDWR | O_CLOEXEC);
        if (drm_fd_ >= 0) break;
    }
    if (drm_fd_ < 0) { fprintf(stderr, "egl_dmabuf: no DRM render node\n"); return false; }

    gbm_ = gbm_create_device(drm_fd_);
    if (!gbm_) { fprintf(stderr, "egl_dmabuf: gbm_create_device failed\n"); close(drm_fd_); drm_fd_ = -1; return false; }

    ok_ = true;
    fprintf(stderr, "egl_dmabuf: zero-copy dmabuf path ready\n");
    return true;
}

bool EglDmabuf::create(int w, int h, uint32_t fourcc, zwp_linux_dmabuf_v1 *dmabuf,
                       DmabufFrame &out) {
    if (!ok_ || !dmabuf || w <= 0 || h <= 0) return false;

    auto *dev = static_cast<gbm_device *>(gbm_);
    gbm_bo *bo = gbm_bo_create(dev, (uint32_t)w, (uint32_t)h, fourcc,
                               GBM_BO_USE_RENDERING);
    if (!bo) { fprintf(stderr, "egl_dmabuf: gbm_bo_create failed (fourcc %.4s)\n", (char *)&fourcc); return false; }

    const int      planes   = gbm_bo_get_plane_count(bo);
    const uint64_t modifier = gbm_bo_get_modifier(bo);
    if (planes <= 0 || planes > 4) { gbm_bo_destroy(bo); return false; }

    int      fds[4]     = {-1, -1, -1, -1};
    uint32_t strides[4] = {0};
    uint32_t offsets[4] = {0};
    for (int i = 0; i < planes; i++) {
        fds[i]     = gbm_bo_get_fd_for_plane(bo, i);
        strides[i] = gbm_bo_get_stride_for_plane(bo, i);
        offsets[i] = gbm_bo_get_offset(bo, i);
        if (fds[i] < 0) {
            for (int j = 0; j < i; j++) close(fds[j]);
            gbm_bo_destroy(bo);
            return false;
        }
    }

    // ── Import as EGLImage → GL texture ──
    // eglCreateImageKHR takes an EGLint attribute list.
    EGLint attribs[64];
    int a = 0;
    attribs[a++] = EGL_WIDTH;                 attribs[a++] = w;
    attribs[a++] = EGL_HEIGHT;                attribs[a++] = h;
    attribs[a++] = EGL_LINUX_DRM_FOURCC_EXT;  attribs[a++] = (EGLint)fourcc;

    static const EGLint fd_attr[4]   = {EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE3_FD_EXT};
    static const EGLint off_attr[4]  = {EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT, EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT};
    static const EGLint pit_attr[4]  = {EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT, EGL_DMA_BUF_PLANE2_PITCH_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT};
    static const EGLint modlo_attr[4]= {EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT};
    static const EGLint modhi_attr[4]= {EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT};

    for (int i = 0; i < planes; i++) {
        attribs[a++] = fd_attr[i];  attribs[a++] = fds[i];
        attribs[a++] = off_attr[i]; attribs[a++] = (EGLint)offsets[i];
        attribs[a++] = pit_attr[i]; attribs[a++] = (EGLint)strides[i];
        if (modifier != DRM_FORMAT_MOD_INVALID) {
            attribs[a++] = modlo_attr[i]; attribs[a++] = (EGLint)(modifier & 0xFFFFFFFF);
            attribs[a++] = modhi_attr[i]; attribs[a++] = (EGLint)(modifier >> 32);
        }
    }
    attribs[a++] = EGL_NONE;

    EGLImageKHR image = p_eglCreateImageKHR(dpy_, EGL_NO_CONTEXT,
                                            EGL_LINUX_DMA_BUF_EXT, nullptr, attribs);

    GLuint tex = 0;
    if (image != EGL_NO_IMAGE_KHR) {
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        p_glEGLImageTex(GL_TEXTURE_2D, (GLeglImageOES)image);
        glBindTexture(GL_TEXTURE_2D, 0);
    } else {
        fprintf(stderr, "egl_dmabuf: eglCreateImageKHR failed\n");
        for (int i = 0; i < planes; i++) close(fds[i]);
        gbm_bo_destroy(bo);
        return false;
    }

    // ── Wrap the same dmabuf in a wl_buffer for the compositor to write into ──
    zwp_linux_buffer_params_v1 *params = zwp_linux_dmabuf_v1_create_params(dmabuf);
    for (int i = 0; i < planes; i++) {
        // The compositor's socket dups the fd; we still own ours (closed below).
        zwp_linux_buffer_params_v1_add(params, fds[i], i, offsets[i], strides[i],
                                       (uint32_t)(modifier >> 32),
                                       (uint32_t)(modifier & 0xFFFFFFFF));
    }
    wl_buffer *buf = zwp_linux_buffer_params_v1_create_immed(params, w, h, fourcc, 0);
    zwp_linux_buffer_params_v1_destroy(params);

    for (int i = 0; i < planes; i++) close(fds[i]);

    if (!buf) {
        fprintf(stderr, "egl_dmabuf: linux-dmabuf create_immed failed\n");
        p_eglDestroyImageKHR(dpy_, image);
        glDeleteTextures(1, &tex);
        gbm_bo_destroy(bo);
        return false;
    }

    out.tex    = tex;
    out.image  = image;
    out.bo     = bo;
    out.buffer = buf;
    out.width  = w;
    out.height = h;
    out.fourcc = fourcc;
    return true;
}

void EglDmabuf::destroy(DmabufFrame &f) {
    if (f.buffer) wl_buffer_destroy(f.buffer);
    if (f.tex)    glDeleteTextures(1, &f.tex);
    if (f.image)  p_eglDestroyImageKHR(dpy_, (EGLImageKHR)f.image);
    if (f.bo)     gbm_bo_destroy(static_cast<gbm_bo *>(f.bo));
    f = DmabufFrame{};
}

#else  // !HAVE_GBM — stubs: no zero-copy path, callers fall back to shm.

EglDmabuf::~EglDmabuf() {}
bool EglDmabuf::init(EGLDisplay) { return false; }
bool EglDmabuf::create(int, int, uint32_t, zwp_linux_dmabuf_v1 *, DmabufFrame &) { return false; }
void EglDmabuf::destroy(DmabufFrame &f) { f = DmabufFrame{}; }

#endif // HAVE_GBM
