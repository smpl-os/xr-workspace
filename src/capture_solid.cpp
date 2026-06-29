// capture_solid.cpp
#include "capture_solid.h"

#include <cstring>

SolidColorCapture::SolidColorCapture(const uint8_t rgba[4]) {
    memcpy(color_, rgba, 4);
}

bool SolidColorCapture::start() {
    glGenTextures(1, &tex_);
    glBindTexture(GL_TEXTURE_2D, tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, color_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);
    return true;
}

void SolidColorCapture::stop() {
    if (tex_) { glDeleteTextures(1, &tex_); tex_ = 0; }
}
