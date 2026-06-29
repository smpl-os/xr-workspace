// capture_wlr.h — capture backend using wlr-screencopy-unstable-v1.
// This is the broad-compatibility path (sway, older wlroots compositors, and
// Hyprland's wlr-* compatibility layer). Prefers a zero-copy dmabuf buffer when
// the compositor (v3) and EGL import allow it, otherwise copies via shm.
#pragma once

#include "capture.h"
#include "capture_ctx.h"
#include "capture_shm.h"
#include "egl_dmabuf.h"

struct wl_output;
struct zwlr_screencopy_frame_v1;

class WlrScreencopyCapture : public ICaptureSource {
public:
    WlrScreencopyCapture(CaptureContext *ctx, wl_output *output);
    ~WlrScreencopyCapture() override;

    bool   start() override;
    void   update() override;
    GLuint texture() const override;
    void   stop() override;
    bool   swizzle_bgr() const override { return use_dmabuf_ ? false : swizzle_; }
    bool   flip_y() const override { return flip_; }

    // ── frame event handlers (called from C trampolines) ──
    void on_buffer(uint32_t format, uint32_t w, uint32_t h, uint32_t stride);
    void on_linux_dmabuf(uint32_t format, uint32_t w, uint32_t h);
    void on_buffer_done();
    void on_flags(uint32_t flags);
    void on_ready();
    void on_failed();

private:
    void request_frame();
    void choose_and_copy();
    void upload_shm();
    void release_frame();

    CaptureContext *ctx_;
    wl_output      *output_;
    unsigned        mgr_version_ = 1;

    zwlr_screencopy_frame_v1 *frame_ = nullptr;
    bool in_flight_ = false;
    bool degraded_  = false; // disable after a hard failure

    // shm path
    ShmBuffer shm_;
    GLuint    shm_tex_ = 0;
    bool      have_shm_ = false;
    uint32_t  shm_format_ = 0, shm_w_ = 0, shm_h_ = 0, shm_stride_ = 0;

    // dmabuf path
    DmabufFrame dma_;
    bool        have_dma_ = false;
    uint32_t    dma_format_ = 0, dma_w_ = 0, dma_h_ = 0;

    bool use_dmabuf_ = false;
    bool swizzle_    = true;
    bool flip_       = false;
    bool has_tex_    = false;
    bool dmabuf_failed_ = false;
};
