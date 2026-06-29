// capture_ext.cpp — see capture_ext.h.
#include "capture_ext.h"

#include <wayland-client.h>
#include "ext-image-capture-source-v1-client.h"
#include "ext-image-copy-capture-v1-client.h"

#include <GLES2/gl2.h>
#include <cstdio>

namespace {
bool fmt_is_bgr(uint32_t f) {
    return f == WL_SHM_FORMAT_ARGB8888 || f == WL_SHM_FORMAT_XRGB8888;
}

// ── session listener trampolines ──
void s_buffer_size(void *d, ext_image_copy_capture_session_v1 *, uint32_t w, uint32_t h) {
    static_cast<ExtImageCopyCapture *>(d)->on_buffer_size(w, h);
}
void s_shm_format(void *d, ext_image_copy_capture_session_v1 *, uint32_t fmt) {
    static_cast<ExtImageCopyCapture *>(d)->on_shm_format(fmt);
}
void s_dmabuf_device(void *, ext_image_copy_capture_session_v1 *, wl_array *) {}
void s_dmabuf_format(void *d, ext_image_copy_capture_session_v1 *, uint32_t fmt, wl_array *) {
    static_cast<ExtImageCopyCapture *>(d)->on_dmabuf_format(fmt);
}
void s_done(void *d, ext_image_copy_capture_session_v1 *) {
    static_cast<ExtImageCopyCapture *>(d)->on_constraints_done();
}
void s_stopped(void *d, ext_image_copy_capture_session_v1 *) {
    static_cast<ExtImageCopyCapture *>(d)->on_stopped();
}
const ext_image_copy_capture_session_v1_listener g_session_listener = {
    s_buffer_size, s_shm_format, s_dmabuf_device, s_dmabuf_format, s_done, s_stopped,
};

// ── frame listener trampolines ──
void fr_transform(void *d, ext_image_copy_capture_frame_v1 *, uint32_t t) {
    static_cast<ExtImageCopyCapture *>(d)->on_transform(t);
}
void fr_damage(void *, ext_image_copy_capture_frame_v1 *, int32_t, int32_t, int32_t, int32_t) {}
void fr_presentation_time(void *, ext_image_copy_capture_frame_v1 *, uint32_t, uint32_t, uint32_t) {}
void fr_ready(void *d, ext_image_copy_capture_frame_v1 *) {
    static_cast<ExtImageCopyCapture *>(d)->on_frame_ready();
}
void fr_failed(void *d, ext_image_copy_capture_frame_v1 *, uint32_t reason) {
    static_cast<ExtImageCopyCapture *>(d)->on_frame_failed(reason);
}
const ext_image_copy_capture_frame_v1_listener g_frame_listener = {
    fr_transform, fr_damage, fr_presentation_time, fr_ready, fr_failed,
};
} // namespace

ExtImageCopyCapture::ExtImageCopyCapture(CaptureContext *ctx, wl_output *output)
    : ctx_(ctx), output_(output) {}

ExtImageCopyCapture::~ExtImageCopyCapture() { stop(); }

bool ExtImageCopyCapture::start() {
    if (!ctx_ || !ctx_->ext_copy || !ctx_->ext_src || !output_) return false;
    create_session();
    return session_ != nullptr;
}

void ExtImageCopyCapture::create_session() {
    source_ = ext_output_image_capture_source_manager_v1_create_source(ctx_->ext_src, output_);
    if (!source_) { degraded_ = true; return; }
    session_ = ext_image_copy_capture_manager_v1_create_session(ctx_->ext_copy, source_, 0);
    if (!session_) { degraded_ = true; return; }
    ext_image_copy_capture_session_v1_add_listener(session_, &g_session_listener, this);
}

GLuint ExtImageCopyCapture::texture() const {
    return use_dmabuf_ ? dma_.tex : shm_tex_;
}

void ExtImageCopyCapture::on_buffer_size(uint32_t w, uint32_t h) { buf_w_ = w; buf_h_ = h; }
void ExtImageCopyCapture::on_shm_format(uint32_t fmt) {
    if (!have_shm_fmt_) { shm_format_ = fmt; have_shm_fmt_ = true; }
}
void ExtImageCopyCapture::on_dmabuf_format(uint32_t fmt) {
    if (!have_dma_fmt_) { dma_format_ = fmt; have_dma_fmt_ = true; }
}
void ExtImageCopyCapture::on_constraints_done() {
    constraints_ready_ = (buf_w_ > 0 && buf_h_ > 0 && (have_shm_fmt_ || have_dma_fmt_));
}
void ExtImageCopyCapture::on_stopped() { degraded_ = true; }
void ExtImageCopyCapture::on_transform(uint32_t) {}

void ExtImageCopyCapture::release_frame() {
    if (frame_) { ext_image_copy_capture_frame_v1_destroy(frame_); frame_ = nullptr; }
    in_flight_ = false;
}

void ExtImageCopyCapture::update() {
    if (degraded_ || in_flight_ || !constraints_ready_ || !session_) return;
    begin_frame();
}

void ExtImageCopyCapture::begin_frame() {
    // Pick a buffer: dmabuf preferred, else shm.
    wl_buffer *buffer = nullptr;

    if (ctx_->prefer_dmabuf && !dmabuf_failed_ && ctx_->egl && ctx_->egl->available() &&
        ctx_->dmabuf && have_dma_fmt_) {
        if (!dma_.valid() || (uint32_t)dma_.width != buf_w_ ||
            (uint32_t)dma_.height != buf_h_ || dma_.fourcc != dma_format_) {
            ctx_->egl->destroy(dma_);
            ctx_->egl->create((int)buf_w_, (int)buf_h_, dma_format_, ctx_->dmabuf, dma_);
        }
        if (dma_.valid()) { use_dmabuf_ = true; buffer = dma_.buffer; }
    }

    if (!buffer && have_shm_fmt_) {
        const int stride = (int)buf_w_ * 4;
        if (!shm_.valid() || (uint32_t)shm_.width != buf_w_ ||
            (uint32_t)shm_.height != buf_h_ || shm_.format != shm_format_) {
            shm_.create(ctx_->shm, (int)buf_w_, (int)buf_h_, stride, shm_format_);
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
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)buf_w_, (GLsizei)buf_h_,
                             0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                glBindTexture(GL_TEXTURE_2D, 0);
            }
            use_dmabuf_ = false;
            swizzle_    = fmt_is_bgr(shm_format_);
            buffer      = shm_.buffer;
        }
    }

    if (!buffer) { degraded_ = true; return; } // no usable buffer type

    frame_ = ext_image_copy_capture_session_v1_create_frame(session_);
    if (!frame_) { degraded_ = true; return; }
    ext_image_copy_capture_frame_v1_add_listener(frame_, &g_frame_listener, this);
    ext_image_copy_capture_frame_v1_attach_buffer(frame_, buffer);
    ext_image_copy_capture_frame_v1_damage_buffer(frame_, 0, 0, (int)buf_w_, (int)buf_h_);
    ext_image_copy_capture_frame_v1_capture(frame_);
    in_flight_ = true;
}

void ExtImageCopyCapture::upload_shm() {
    if (!shm_.valid() || !shm_tex_) return;
    glBindTexture(GL_TEXTURE_2D, shm_tex_);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, shm_.width, shm_.height,
                    GL_RGBA, GL_UNSIGNED_BYTE, shm_.data);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void ExtImageCopyCapture::on_frame_ready() {
    if (!use_dmabuf_) upload_shm();
    has_tex_ = true;
    release_frame();
}

void ExtImageCopyCapture::on_frame_failed(uint32_t reason) {
    // reason: 0 unknown, 1 buffer_constraints, 2 stopped.
    if (reason == 2) degraded_ = true;
    if (use_dmabuf_) {
        if (ctx_ && ctx_->egl) ctx_->egl->destroy(dma_);
        use_dmabuf_    = false;
        dmabuf_failed_ = true; // retry on shm
    }
    release_frame();
}

void ExtImageCopyCapture::stop() {
    release_frame();
    if (session_) { ext_image_copy_capture_session_v1_destroy(session_); session_ = nullptr; }
    if (source_)  { ext_image_capture_source_v1_destroy(source_); source_ = nullptr; }
    shm_.destroy();
    if (shm_tex_) { glDeleteTextures(1, &shm_tex_); shm_tex_ = 0; }
    if (ctx_ && ctx_->egl) ctx_->egl->destroy(dma_);
    has_tex_ = false;
}
