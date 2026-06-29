// posesink.h — swappable head-tracking OUTPUT interface ("mode B").
//
// Mirror of IPoseSource: instead of READING a head pose, an IPoseSink EMITS the
// (already recentered) head orientation to an external consumer — e.g. a flight
// or space sim via OpenTrack. Keeping this behind an interface means new
// transports (uinput virtual joystick, native protocols, …) can be dropped in
// without touching the render loop.
#pragma once

#include "linalg.h"

struct IPoseSink {
    virtual ~IPoseSink() = default;

    // Open the transport (socket/device). Returns false on failure.
    virtual bool open() = 0;

    // Emit the head orientation, expressed RELATIVE to the recenter origin
    // (i.e. identity == looking straight ahead). Cheap; called every frame.
    virtual void send(Quat relative_orientation) = 0;

    // Release the transport. Safe to call when not open.
    virtual void close() = 0;

    virtual const char *name() const = 0;
};
