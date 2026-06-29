// capture_factory.cpp — see capture_factory.h.
#include "capture_factory.h"

#include "capture.h"
#include "capture_ctx.h"
#include "capture_solid.h"
#include "capture_wlr.h"
#include "capture_ext.h"
#include "config.h"

#include <cstdio>

namespace {
ICaptureSource *make_solid(const MonitorConfig &cfg) {
    const uint32_t c = cfg.color;
    const uint8_t rgba[4] = {
        (uint8_t)((c >> 16) & 0xFF),
        (uint8_t)((c >> 8) & 0xFF),
        (uint8_t)(c & 0xFF),
        255};
    auto *s = new SolidColorCapture(rgba);
    s->start();
    return s;
}

// Try a backend; on start() failure, delete and return null so the caller can
// fall through to the next option.
ICaptureSource *try_backend(ICaptureSource *cap) {
    if (cap && cap->start()) return cap;
    delete cap;
    return nullptr;
}
} // namespace

ICaptureSource *make_capture_source(CaptureContext *ctx, const MonitorConfig &cfg,
                                    const std::string &protocol) {
    // No source bound → solid colour placeholder.
    if (!ctx || cfg.source.empty() || !ctx->find_output)
        return make_solid(cfg);

    wl_output *output = ctx->find_output(cfg.source);
    if (!output) {
        fprintf(stderr, "capture: output '%s' not found — solid colour\n", cfg.source.c_str());
        return make_solid(cfg);
    }

    const bool ext_ok = ctx->ext_copy && ctx->ext_src;
    const bool wlr_ok = ctx->screencopy != nullptr;

    // Build an ordered preference list from the requested protocol.
    enum Backend { EXT, WLR };
    Backend order[2];
    int n = 0;
    if (protocol == "wlr") {
        if (wlr_ok) order[n++] = WLR;
        if (ext_ok) order[n++] = EXT;
    } else if (protocol == "ext") {
        if (ext_ok) order[n++] = EXT;
        if (wlr_ok) order[n++] = WLR;
    } else { // auto — prefer the modern standard
        if (ext_ok) order[n++] = EXT;
        if (wlr_ok) order[n++] = WLR;
    }

    for (int i = 0; i < n; i++) {
        ICaptureSource *cap = nullptr;
        if (order[i] == EXT) {
            cap = try_backend(new ExtImageCopyCapture(ctx, output));
            if (cap) { fprintf(stderr, "capture '%s': ext-image-copy-capture\n", cfg.source.c_str()); return cap; }
        } else {
            cap = try_backend(new WlrScreencopyCapture(ctx, output));
            if (cap) { fprintf(stderr, "capture '%s': wlr-screencopy\n", cfg.source.c_str()); return cap; }
        }
    }

    fprintf(stderr, "capture: no screencopy protocol for '%s' — solid colour\n", cfg.source.c_str());
    return make_solid(cfg);
}
