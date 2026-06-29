// pose_breezy.cpp — read head orientation from the Breezy Desktop SHM block.
#include "pose_breezy.h"

#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

// ─── SHM layout (from breezy-desktop/kwin/src/breezydesktopeffect.cpp) ───────
static constexpr int   SHM_ENABLED_OFF     = 1;
static constexpr int   SHM_POSE_ORIENT_OFF = 121; // float[16]: 4 rows × 4 floats
static constexpr int   SHM_LENGTH          = 186;
static const char     *SHM_PATH            = "/dev/shm/breezy_desktop_imu";

bool BreezyImuSource::open() {
    fd_ = ::open(SHM_PATH, O_RDONLY);
    if (fd_ < 0) return false;
    ptr_ = mmap(nullptr, SHM_LENGTH, PROT_READ, MAP_SHARED, fd_, 0);
    if (ptr_ == MAP_FAILED) {
        ::close(fd_);
        fd_  = -1;
        ptr_ = nullptr;
        return false;
    }
    return true;
}

// Read the T0 quaternion right before rendering — maximum freshness.
// xrDriver delivers a unit quaternion, so no normalization is needed here.
// Raw NWU (x,y,z,w) → EUS: (x=-raw[1], y=raw[2], z=-raw[0], w=raw[3]).
Pose BreezyImuSource::read() {
    Pose p;
    if (!ptr_) return p; // valid=false, identity orientation
    const uint8_t *data = static_cast<const uint8_t *>(ptr_);
    if (!data[SHM_ENABLED_OFF]) return p;
    float raw[4];
    memcpy(raw, data + SHM_POSE_ORIENT_OFF, 16u);
    p.orientation = { -raw[1], raw[2], -raw[0], raw[3] };
    p.valid       = true;
    return p;
}

void BreezyImuSource::close() {
    if (ptr_ && ptr_ != MAP_FAILED) munmap(ptr_, SHM_LENGTH);
    if (fd_ >= 0) ::close(fd_);
    ptr_ = nullptr;
    fd_  = -1;
}
