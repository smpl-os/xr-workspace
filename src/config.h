// config.h — load/save xr-workspace settings from config.json.
// This is the external integration surface (e.g. smplOS settings app) together
// with the runtime control API.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct JsonValue; // fwd — defined in json.h

struct MonitorConfig {
    std::string id;
    std::string source;            // wayland output name to capture; "" → solid colour
    int      width     = 1920;
    int      height    = 1080;
    float    scale     = 1.0f;
    float    angle_deg = 0.0f;     // azimuth on the arc
    float    size_m    = 1.0f;     // physical width in world units
    float    curvature = 0.0f;     // reserved
    uint32_t color     = 0x283c78; // solid fallback (0xRRGGBB)
};

// Head-tracking OUTPUT ("mode B"): feed the head orientation to a sim/game.
// Tunable per game via named profiles (see Config::profiles).
struct HeadTrackConfig {
    bool        enabled      = false;
    std::string protocol     = "opentrack"; // reserved for future: "uinput"
    std::string host         = "127.0.0.1";
    int         port         = 4242;        // OpenTrack "UDP over network" input
    float       rate_hz      = 120.0f;      // send cap (0 = every frame)
    float       yaw_scale    = 1.0f;
    float       pitch_scale  = 1.0f;
    float       roll_scale   = 1.0f;
    bool        invert_yaw   = false;
    bool        invert_pitch = false;
    bool        invert_roll  = false;
    float       deadzone_deg = 0.0f;
};

struct Config {
    std::string output;                       // physical glasses output ("" = auto)
    float       fov_deg     = 46.0f;
    std::string pose_source = "breezy_shm";
    float       smoothing   = 0.0f;
    std::string layout_mode = "arc";          // arc | flat | custom
    float       radius      = 2.0f;
    std::string control_socket;               // "" = $XDG_RUNTIME_DIR/xr-workspace.sock
    std::vector<MonitorConfig> monitors;

    // Capture backend selection + performance toggles.
    std::string capture_protocol = "auto";    // auto | ext | wlr
    bool        prefer_dmabuf    = true;       // zero-copy when available

    // Stereoscopic side-by-side output (SBS 3D on the glasses).
    bool        stereo = false;
    float       ipd_m  = 0.063f;               // interpupillary distance (world units)

    HeadTrackConfig head_tracking;            // global default head-tracking output
    std::vector<std::pair<std::string, HeadTrackConfig>> profiles; // per-game tuning
    std::string active_profile;               // profile applied at startup ("" = none)

    // Look up a named profile; nullptr if not found.
    const HeadTrackConfig *find_profile(const std::string &name) const;

    // Populate `monitors` with a default arc of `count` (1 or 3) screens.
    void make_default_monitors(int count);

    // Load + merge from a JSON file. Returns false (and fills `err`) only on a
    // present-but-invalid file; a missing file is not an error (returns true,
    // leaves defaults).
    bool load(const std::string &path, std::string &err);
};

// Resolve the config path: explicit `cli_path` if non-empty, else
// $XDG_CONFIG_HOME/xr-workspace/config.json, else ~/.config/...
std::string config_default_path(const std::string &cli_path);

// Merge head-tracking fields from a JSON object into `h` (unspecified fields
// keep their current value). Shared by the config loader and the control API.
void headtrack_merge_json(const JsonValue &v, HeadTrackConfig &h);
