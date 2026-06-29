// capture_solid.h — placeholder/test capture: a 1×1 solid-colour texture.
// Used when a monitor has no real source bound, and for development/testing.
#pragma once

#include "capture.h"

#include <cstdint>

class SolidColorCapture : public ICaptureSource {
public:
    explicit SolidColorCapture(const uint8_t rgba[4]);
    bool   start() override;
    void   update() override {}
    GLuint texture() const override { return tex_; }
    void   stop() override;

private:
    uint8_t color_[4];
    GLuint  tex_ = 0;
};
