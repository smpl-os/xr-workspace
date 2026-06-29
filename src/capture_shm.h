// capture_shm.h — a reusable shared-memory wl_buffer (the compatibility path
// used when zero-copy dmabuf import isn't available). Backs a CPU-visible
// mapping the compositor copies into; the renderer then uploads it to a GL
// texture with glTexSubImage2D.
#pragma once

#include <cstddef>
#include <cstdint>

struct wl_shm;
struct wl_buffer;

struct ShmBuffer {
    wl_buffer *buffer = nullptr;
    uint8_t   *data   = nullptr;
    size_t     size   = 0;
    int        width  = 0;
    int        height = 0;
    int        stride = 0;
    uint32_t   format = 0; // wl_shm format enum

    // Allocate a pool-backed buffer. Returns false on failure.
    bool create(wl_shm *shm, int w, int h, int stride, uint32_t shm_format);
    void destroy();
    bool valid() const { return buffer != nullptr && data != nullptr; }
};
