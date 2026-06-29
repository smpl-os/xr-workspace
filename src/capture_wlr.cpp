// capture_wlr.cpp — see capture_wlr.h.
#include "capture_wlr.h"

#include <wayland-client.h>
#include "wlr-screencopy-unstable-v1-client.h"

#include <GLES2/gl2.h>
#include <cstdio>
#include <cstring>

namespace {
// ARGB8888 / XRGB8888 store bytes B,G,R,A in memory → the shader swaps R/B.
bool fmt_is_bgr(uint32_t f) {
    return f == WL_SHM_FORMAT_ARGB8888 || f == WL_SHM_FORMAT_XRGB8888;
}

// ── frame listener trampolines ──
void f_buffer(void *d, zwlr_screencopy_frame_v1 *, uint32_t fmt, uint32_t w, uint32_t h, uint32_t s) {
    static_cast<WlrScreencopyCapture *>(d)->on_buffer(fmt, w, h, s);
}
void f_flags(void *d, zwlr_screencopy_frame_v1 *, uint32_t flags) {
    static_cast<WlrScreencopyCapture *>(d)->on_flags(flags);
}
void f_ready(void *d, zwlr_screencopy_frame_v1 *, uint32_t, uint32_t, uint32_t) {
    static_cast<WlrScreencopyCapture *>(d)->on_ready();
}
void f_failed(void *d, zwlr_screencopy_frame_v1 *) {
    static_cast<WlrScreencopyCapture *>(d)->on_failed();
}
void f_damage(void *, zwlr_screencopy_frame_v1 *, uint32_t, uint32_t, uint32_t, uint32_t) {}
void f_linux_dmabuf(void *d, zwlr_screencopy_frame_v1 *, uint32_t fmt, uint32_t w, uint32_t h) {
    static_cast<WlrScreencopyCapture *>(d)->on_linux_dmabuf(fmt, w, h);
}
void f_buffer_done(void *d, zwlr_screencopy_frame_v1 *) {
    static_cast<WlrScreencopyCapture *>(d)->on_buffer_done();
}

const zwlr_screencopy_frame_v1_listener g_frame_listener = {
    f_buffer, f_flags, f_ready, f_failed, f_damage, f_linux_dmabuf, f_buffer_done,
};
} // namespace

WlrScreencopyCapture::WlrScreencopyCapture(CaptureContext *ctx, wl_output *output)
    : ctx_(ctx), output_(output) {}

WlrScreencopyCapture::~WlrScreencopyCapture() { stop(); }

bool WlrScreencopyCapture::start() {
    if (!ctx_ || !ctx_->screencopy || !output_) return false;
    mgr_version_ = wl_proxy_get_version((wl_proxy *)ctx_->screencopy);
    return true;
}

GLuint WlrScreencopyCapture::texture() const {
    return use_dmabuf_ ? dma_.tex : shm_tex_;
}

void WlrScreencopyCapture::release_frame() {
    if (frame_) { zwlr_screencopy_frame_v1_destroy(frame_); frame_ = nullptr; }
    in_flight_ = false;
}

void WlrScreencopyCapture::request_frame() {
    have_shm_ = have_dma_ = false;
    frame_ = zwlr_screencopy_manager_v1_capture_output(ctx_->screencopy, 0, output_);
    if (!frame_) { degraded_ = true; return; }
    zwlr_screencopy_frame_v1_add_listener(frame_, &g_frame_listener, this);
    in_flight_ = true;
}

void WlrScreencopyCapture::update() {
    if (degraded_ || in_flight_ || !ctx_ || !ctx_->screencopy || !output_) return;
    request_frame();
}

void WlrScreencopyCapture::on_buffer(uint32_t fmt, uint32_t w, uint32_t h, uint32_t stride) {
    shm_format_ = fmt; shm_w_ = w; shm_h_ = h; shm_stride_ = stride;
    have_shm_ = true;
    // Versions < 3 send no buffer_done and guarantee shm support: copy now.
    if (mgr_version_ < 3) choose_and_copy();
}

void WlrScreencopyCapture::on_linux_dmabuf(uint32_t fmt, uint32_t w, uint32_t h) {
    dma_format_ = fmt; dma_w_ = w; dma_h_ = h;
    have_dma_ = true;
}

void WlrScreencopyCapture::on_buffer_done() { choose_and_copy(); }

void WlrScreencopyCapture::on_flags(uint32_t flags) {
    flip_ = (flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT) != 0;
}

void WlrScreencopyCapture::choose_and_copy() {
    if (!frame_) return;

    // Preferred: zero-copy dmabuf.
    if (ctx_->prefer_dmabuf && !dmabuf_failed_ && ctx_->egl && ctx_->egl->available() &&
        ctx_->dmabuf && have_dma_) {
        if (!dma_.valid() || (uint32_t)dma_.width != dma_w_ ||
            (uint32_t)dma_.height != dma_h_ || dma_.fourcc != dma_format_) {
            ctx_->egl->destroy(dma_);
            ctx_->egl->create((int)dma_w_, (int)dma_h_, dma_format_, ctx_->dmabuf, dma_);
        }
        if (dma_.valid()) {
            use_dmabuf_ = true;
            zwlr_screencopy_frame_v1_copy(frame_, dma_.buffer);
            return;
        }
    }

    // Fallback: shm copy.
    if (have_shm_) {
        if (!shm_.valid() || (uint32_t)shm_.width != shm_w_ ||
            (uint32_t)shm_.height != shm_h_ || shm_.format != shm_format_) {
            shm_.create(ctx_->shm, (int)shm_w_, (int)shm_h_, (int)shm_stride_, shm_format_);
            if (shm_tex_) { glDeleteTextures(1, &shm_tex_); shm_tex_ = 0; }
        }
        if (shm_.valid()) {
            if (!shm_tex_) {
                glGenTextures(1, &shm_tex_);
                glBindTexture(GL_TEXTURE_2D, shm_tex_);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)shm_w_, (GLsizei)shm_h_,
                             0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                glBindTexture(GL_TEXTURE_2D, 0);
            }
            use_dmabuf_ = false;
            swizzle_    = fmt_is_bgr(shm_format_);
            zwlr_screencopy_frame_v1_copy(frame_, shm_.buffer);
            return;
        }
    }

    // Nothing usable — drop this frame.
    release_frame();
}

void WlrScreencopyCapture::upload_shm() {
    if (!shm_.valid() || !shm_tex_) return;
    glBindTexture(GL_TEXTURE_2D, shm_tex_);
    const int tight = shm_.width * 4;
    if (shm_.stride == tight) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, shm_.width, shm_.height,
                        GL_RGBA, GL_UNSIGNED_BYTE, shm_.data);
    } else {
        // GLES2 has no UNPACK_ROW_LENGTH — upload row by row (slow fallback).
        for (int y = 0; y < shm_.height; y++) {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y, shm_.width, 1, GL_RGBA,
                            GL_UNSIGNED_BYTE, shm_.data + (size_t)y * shm_.stride);
        }
    }
    glBindTexture(GL_TEXTURE_2D, 0);
}

void WlrScreencopyCapture::on_ready() {
    if (!use_dmabuf_) upload_shm();
    has_tex_ = true;
    release_frame();
}

void WlrScreencopyCapture::on_failed() {
    // A dmabuf format the compositor advertised may still be unusable; fall back
    // to shm for this source on subsequent frames.
    if (use_dmabuf_) {
        if (ctx_ && ctx_->egl) ctx_->egl->destroy(dma_);
        use_dmabuf_   = false;
        dmabuf_failed_ = true;
    }
    release_frame();
}

void WlrScreencopyCapture::stop() {
    release_frame();
    shm_.destroy();
    if (shm_tex_) { glDeleteTextures(1, &shm_tex_); shm_tex_ = 0; }
    if (ctx_ && ctx_->egl) ctx_->egl->destroy(dma_);
    has_tex_ = false;
}
