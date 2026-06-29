// capture_ctx.h — shared Wayland/EGL resources handed to every capture backend.
// The renderer owns one CaptureContext and passes it to the capture factory so
// each ICaptureSource can reach the screencopy/ext managers, wl_shm, the
// linux-dmabuf factory and the EGL dmabuf importer without touching globals.
#pragma once

#include <EGL/egl.h>
#include <functional>
#include <string>

struct wl_display;
struct wl_output;
struct wl_shm;
struct zwlr_screencopy_manager_v1;
struct ext_image_copy_capture_manager_v1;
struct ext_output_image_capture_source_manager_v1;
struct zwp_linux_dmabuf_v1;
class EglDmabuf;

struct CaptureContext {
    wl_display *display = nullptr;
    wl_shm     *shm     = nullptr;

    // wlr-screencopy (compatibility fallback) and ext-image-copy-capture
    // (preferred, modern standard) managers — whichever the compositor offers.
    zwlr_screencopy_manager_v1                   *screencopy = nullptr;
    ext_image_copy_capture_manager_v1            *ext_copy   = nullptr;
    ext_output_image_capture_source_manager_v1   *ext_src    = nullptr;

    // linux-dmabuf factory for wrapping gbm buffers into wl_buffers (perf path).
    zwp_linux_dmabuf_v1 *dmabuf = nullptr;

    // EGLImage importer; null when gbm/EGL dmabuf import is unavailable, in
    // which case backends transparently fall back to the shm copy path.
    EglDmabuf *egl = nullptr;

    // Prefer the zero-copy dmabuf path when both it and the backend support it.
    bool prefer_dmabuf = true;

    // Resolve a Wayland output name (e.g. "DP-3") to its wl_output*; the
    // renderer wires this to its live output list.
    std::function<wl_output *(const std::string &)> find_output;
};
