// capture_shm.cpp — see capture_shm.h.
#include "capture_shm.h"

#include <wayland-client.h>

#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

bool ShmBuffer::create(wl_shm *shm, int w, int h, int stride_, uint32_t shm_format) {
    if (!shm || w <= 0 || h <= 0 || stride_ <= 0) return false;
    destroy();

    const size_t sz = (size_t)stride_ * (size_t)h;

    int fd = memfd_create("xr-shm", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) { perror("memfd_create"); return false; }
    if (ftruncate(fd, (off_t)sz) < 0) { perror("ftruncate"); close(fd); return false; }

    void *map = mmap(nullptr, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { perror("mmap"); close(fd); return false; }

    wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)sz);
    wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, w, h, stride_,
                                               shm_format);
    wl_shm_pool_destroy(pool);
    close(fd); // pool keeps its own reference to the mapping

    if (!buf) { munmap(map, sz); return false; }

    buffer = buf;
    data   = static_cast<uint8_t *>(map);
    size   = sz;
    width  = w;
    height = h;
    stride = stride_;
    format = shm_format;
    return true;
}

void ShmBuffer::destroy() {
    if (buffer) { wl_buffer_destroy(buffer); buffer = nullptr; }
    if (data)   { munmap(data, size); data = nullptr; }
    size = 0; width = height = stride = 0; format = 0;
}
