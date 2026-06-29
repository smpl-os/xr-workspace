// sink_opentrack.cpp — see sink_opentrack.h.
#include "sink_opentrack.h"

#include <arpa/inet.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

static double monotonic_seconds() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

// Intrinsic Tait-Bryan extraction: pitch about X, yaw about Y, roll about Z.
// Exact axis signs depend on the glasses' frame; users fine-tune with the
// invert_*/[*]_scale config knobs (and OpenTrack's own mapping curves).
static void quat_to_euler_deg(Quat q, double &yaw, double &pitch, double &roll) {
    double sinp = 2.0 * ((double)q.w * q.x - (double)q.y * q.z);
    if (sinp > 1.0) sinp = 1.0;
    else if (sinp < -1.0) sinp = -1.0;
    const double p = asin(sinp);
    const double y = atan2(2.0 * ((double)q.w * q.y + (double)q.x * q.z),
                           1.0 - 2.0 * ((double)q.x * q.x + (double)q.y * q.y));
    const double r = atan2(2.0 * ((double)q.w * q.z + (double)q.x * q.y),
                           1.0 - 2.0 * ((double)q.x * q.x + (double)q.z * q.z));
    const double R2D = 180.0 / M_PI;
    pitch = p * R2D;
    yaw   = y * R2D;
    roll  = r * R2D;
}

bool OpenTrackSink::open() {
    close();
    fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) { perror("xr-workspace: opentrack socket"); return false; }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)cfg_.port);
    if (inet_pton(AF_INET, cfg_.host.c_str(), &addr.sin_addr) != 1) {
        fprintf(stderr, "xr-workspace: invalid opentrack host '%s'\n", cfg_.host.c_str());
        ::close(fd_); fd_ = -1; return false;
    }
    // connect() a datagram socket → fixes the peer so send() is enough and a
    // dead listener surfaces as ECONNREFUSED instead of silently dropping.
    if (connect(fd_, (sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("xr-workspace: opentrack connect");
        ::close(fd_); fd_ = -1; return false;
    }
    last_send_ = 0.0;
    return true;
}

void OpenTrackSink::send(Quat rel) {
    if (fd_ < 0) return;

    if (cfg_.rate_hz > 0.f) {
        const double now = monotonic_seconds();
        if ((now - last_send_) < 1.0 / (double)cfg_.rate_hz) return;
        last_send_ = now;
    }

    double yaw, pitch, roll;
    quat_to_euler_deg(rel, yaw, pitch, roll);

    auto axis = [](double v, float scale, bool inv, float dz) {
        if (dz > 0.f && fabs(v) < (double)dz) v = 0.0;
        v *= (double)scale;
        return inv ? -v : v;
    };
    yaw   = axis(yaw,   cfg_.yaw_scale,   cfg_.invert_yaw,   cfg_.deadzone_deg);
    pitch = axis(pitch, cfg_.pitch_scale, cfg_.invert_pitch, cfg_.deadzone_deg);
    roll  = axis(roll,  cfg_.roll_scale,  cfg_.invert_roll,  cfg_.deadzone_deg);

    // OpenTrack UDP receiver: 6 little-endian doubles {x, y, z, yaw, pitch, roll}.
    // We drive rotation only (head-look); translation stays at the origin.
    double pkt[6] = {0.0, 0.0, 0.0, yaw, pitch, roll};
    ssize_t n = ::send(fd_, pkt, sizeof(pkt), MSG_DONTWAIT);
    (void)n; // best-effort; a missing listener just means the game isn't up yet
}

void OpenTrackSink::close() {
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
}
