// capture_factory.h — selects the best capture backend for a monitor.
// Preference order honours "most compatible + most performant": the modern
// ext-image-copy-capture standard first, then wlr-screencopy, each preferring a
// zero-copy dmabuf buffer with an shm fallback. Monitors with no source render
// a solid colour.
#pragma once

#include <string>

struct CaptureContext;
struct MonitorConfig;
struct ICaptureSource;

// `protocol` is "auto" | "ext" | "wlr" (from config.json capture_protocol).
ICaptureSource *make_capture_source(CaptureContext *ctx, const MonitorConfig &cfg,
                                    const std::string &protocol);
