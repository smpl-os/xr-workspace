// sink_opentrack.h — OpenTrack UDP head-tracking output (mode B).
//
// Converts the recentered head quaternion to yaw/pitch/roll degrees and pushes
// them to OpenTrack's "UDP over network" input (default 127.0.0.1:4242) as six
// little-endian doubles {x,y,z,yaw,pitch,roll}. OpenTrack then re-emits to the
// game via its TrackIR/freetrack output — which Elite Dangerous, DCS, Star
// Citizen, etc. consume natively.
//
// Linux/Proton note: install OpenTrack to /opt/opentrack or ~/.local (NOT /usr),
// otherwise Steam's pressure-vessel runtime shadows it and Proton games won't
// see the tracker. (Documented on the OpenTrack wiki "common issues".)
#pragma once

#include "config.h"     // HeadTrackConfig
#include "posesink.h"

class OpenTrackSink : public IPoseSink {
public:
    void configure(const HeadTrackConfig &c) { cfg_ = c; }
    const HeadTrackConfig &config() const { return cfg_; }

    bool open() override;
    void send(Quat rel) override;
    void close() override;
    const char *name() const override { return "opentrack"; }

private:
    HeadTrackConfig cfg_;
    int    fd_        = -1;
    double last_send_ = 0.0;
};
