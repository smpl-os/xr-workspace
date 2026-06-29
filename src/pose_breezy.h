// pose_breezy.h — IPoseSource backed by XRLinuxDriver's shared-memory block
// at /dev/shm/breezy_desktop_imu (Breezy Desktop layout).
#pragma once

#include "pose.h"

class BreezyImuSource : public IPoseSource {
public:
    bool        open() override;
    Pose        read() override;
    void        close() override;
    const char *name() const override { return "breezy_shm"; }

private:
    int   fd_  = -1;
    void *ptr_ = nullptr;
};
