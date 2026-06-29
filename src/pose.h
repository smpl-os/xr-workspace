// pose.h — swappable head-pose source interface.
// Implementations deliver the latest head orientation with minimal latency.
#pragma once

#include "linalg.h"

struct Pose {
    Quat  orientation{0.f, 0.f, 0.f, 1.f};
    float px = 0.f, py = 0.f, pz = 0.f; // position (reserved; 3DoF leaves these 0)
    bool  valid = false;                // false → driver disabled / unavailable
};

struct IPoseSource {
    virtual ~IPoseSource() = default;
    // Acquire the underlying resource (SHM, device, socket). Returns false on failure.
    virtual bool        open() = 0;
    // Return the latest pose. Must be cheap — called once per frame, late.
    virtual Pose        read() = 0;
    // Release the resource.
    virtual void        close() = 0;
    // Stable identifier for logging/config (e.g. "breezy_shm").
    virtual const char *name() const = 0;
};
