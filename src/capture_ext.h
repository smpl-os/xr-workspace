// capture_ext.h — capture backend using ext-image-copy-capture-v1, the modern
// cross-compositor standard (preferred over wlr-screencopy). Session based:
// the compositor advertises buffer constraints once, then the client recycles a
// buffer across frames. Prefers zero-copy dmabuf, falls back to shm.
#pragma once

#include "capture.h"
#include "capture_ctx.h"
#include "capture_shm.h"
#include "egl_dmabuf.h"

struct wl_output;
struct ext_image_capture_source_v1;
struct ext_image_copy_capture_session_v1;
struct ext_image_copy_capture_frame_v1;

class ExtImageCopyCapture : public ICaptureSource {
public:
    ExtImageCopyCapture(CaptureContext *ctx, wl_output *output);
    ~ExtImageCopyCapture() override;

    bool   start() override;
    void   update() override;
    GLuint texture() const override;
    void   stop() override;
    bool   swizzle_bgr() const override { return use_dmabuf_ ? false : swizzle_; }
    bool   flip_y() const override { return flip_; }

    // ── session event handlers ──
    void on_buffer_size(uint32_t w, uint32_t h);
    void on_shm_format(uint32_t fmt);
    void on_dmabuf_format(uint32_t fmt);
    void on_constraints_done();
    void on_stopped();
    // ── frame event handlers ──
    void on_transform(uint32_t transform);
    void on_frame_ready();
    void on_frame_failed(uint32_t reason);

private:
    void create_session();
    void begin_frame();
    void upload_shm();
    void release_frame();

    CaptureContext *ctx_;
    wl_output      *output_;

    ext_image_capture_source_v1        *source_  = nullptr;
    ext_image_copy_capture_session_v1  *session_ = nullptr;
    ext_image_copy_capture_frame_v1    *frame_   = nullptr;

    bool constraints_ready_ = false;
    bool in_flight_ = false;
    bool degraded_  = false;

    uint32_t buf_w_ = 0, buf_h_ = 0;
    bool     have_shm_fmt_ = false, have_dma_fmt_ = false;
    uint32_t shm_format_ = 0, dma_format_ = 0;

    // shm path
    ShmBuffer shm_;
    GLuint    shm_tex_ = 0;

    // dmabuf path
    DmabufFrame dma_;

    bool use_dmabuf_    = false;
    bool dmabuf_failed_ = false;
    bool swizzle_       = true;
    bool flip_          = false;
    bool has_tex_       = false;
};
