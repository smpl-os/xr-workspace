// capture.h — swappable per-monitor content source.
// Each virtual monitor owns one ICaptureSource that yields a GL texture,
// refreshed each frame. Implementations: solid colour, wlr-screencopy, dmabuf.
#pragma once

#include <GLES2/gl2.h>

struct ICaptureSource {
    virtual ~ICaptureSource() = default;
    // Allocate GL resources / bind to the underlying source. Needs a current
    // GL context. Returns false on failure.
    virtual bool   start() = 0;
    // Pull a new frame if one is available; cheap no-op otherwise.
    virtual void   update() = 0;
    // The texture to sample for this monitor (valid after start()).
    virtual GLuint texture() const = 0;
    // Release GL resources.
    virtual void   stop() = 0;

    // ── Sampling hints for the fragment shader ────────────────────────────────
    // True when the texture stores pixels in BGRA order (e.g. shm captures of
    // DRM_FORMAT_XRGB8888) and the shader must swap R/B channels. dmabuf imports
    // carry their format in the EGLImage, so they return false.
    virtual bool   swizzle_bgr() const { return false; }
    // True when the captured image is bottom-up (compositor set the y_invert
    // flag) and the shader must flip the V coordinate.
    virtual bool   flip_y() const { return false; }
};
