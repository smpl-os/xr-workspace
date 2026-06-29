// config.cpp
#include "config.h"
#include "json.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

void Config::make_default_monitors(int count) {
    monitors.clear();
    if (count >= 3) {
        const float       angles[3] = {-45.f, 0.f, 45.f};
        const uint32_t    colors[3] = {0x283c78, 0x1e5a1e, 0x5a2828};
        const char *const ids[3]    = {"left", "center", "right"};
        for (int i = 0; i < 3; i++) {
            MonitorConfig m;
            m.id = ids[i]; m.angle_deg = angles[i]; m.color = colors[i];
            monitors.push_back(m);
        }
    } else {
        MonitorConfig m;
        m.id = "center"; m.angle_deg = 0.f; m.color = 0x283c78;
        monitors.push_back(m);
    }
}

static uint32_t parse_color(const JsonValue &v, uint32_t fallback) {
    if (v.type == JsonValue::Number) return (uint32_t)v.num & 0xFFFFFFu;
    if (v.type == JsonValue::String) {
        std::string s = v.str;
        if (!s.empty() && s[0] == '#') s.erase(0, 1);
        return (uint32_t)strtoul(s.c_str(), nullptr, 16) & 0xFFFFFFu;
    }
    return fallback;
}

void headtrack_merge_json(const JsonValue &v, HeadTrackConfig &h) {
    if (v.type != JsonValue::Object) return;
    if (auto *p = v.find("enabled"))      h.enabled      = p->bool_or(h.enabled);
    if (auto *p = v.find("protocol"))     h.protocol     = p->str_or(h.protocol);
    if (auto *p = v.find("host"))         h.host         = p->str_or(h.host);
    if (auto *p = v.find("port"))         h.port         = p->int_or(h.port);
    if (auto *p = v.find("rate_hz"))      h.rate_hz      = (float)p->num_or(h.rate_hz);
    if (auto *p = v.find("yaw_scale"))    h.yaw_scale    = (float)p->num_or(h.yaw_scale);
    if (auto *p = v.find("pitch_scale"))  h.pitch_scale  = (float)p->num_or(h.pitch_scale);
    if (auto *p = v.find("roll_scale"))   h.roll_scale   = (float)p->num_or(h.roll_scale);
    if (auto *p = v.find("invert_yaw"))   h.invert_yaw   = p->bool_or(h.invert_yaw);
    if (auto *p = v.find("invert_pitch")) h.invert_pitch = p->bool_or(h.invert_pitch);
    if (auto *p = v.find("invert_roll"))  h.invert_roll  = p->bool_or(h.invert_roll);
    if (auto *p = v.find("deadzone_deg")) h.deadzone_deg = (float)p->num_or(h.deadzone_deg);
}

const HeadTrackConfig *Config::find_profile(const std::string &name) const {
    for (const auto &p : profiles)
        if (p.first == name) return &p.second;
    return nullptr;
}

bool Config::load(const std::string &path, std::string &err) {
    std::ifstream f(path);
    if (!f.good()) return true; // missing file → keep defaults, not an error

    std::stringstream ss;
    ss << f.rdbuf();
    JsonValue root;
    if (!json_parse(ss.str(), root, err)) return false;
    if (root.type != JsonValue::Object) { err = "config root must be an object"; return false; }

    if (auto *v = root.find("output"))         output       = v->str_or(output);
    if (auto *v = root.find("fov_deg"))        fov_deg      = (float)v->num_or(fov_deg);
    if (auto *v = root.find("pose_source"))    pose_source  = v->str_or(pose_source);
    if (auto *v = root.find("smoothing"))      smoothing    = (float)v->num_or(smoothing);
    if (auto *v = root.find("control_socket")) control_socket = v->str_or(control_socket);
    if (auto *v = root.find("capture_protocol")) capture_protocol = v->str_or(capture_protocol);
    if (auto *v = root.find("prefer_dmabuf"))  prefer_dmabuf = v->bool_or(prefer_dmabuf);
    if (auto *v = root.find("stereo"))         stereo       = v->bool_or(stereo);
    if (auto *v = root.find("ipd_m"))          ipd_m        = (float)v->num_or(ipd_m);

    if (auto *lay = root.find("layout")) {
        if (auto *v = lay->find("mode"))   layout_mode = v->str_or(layout_mode);
        if (auto *v = lay->find("radius")) radius      = (float)v->num_or(radius);
    }

    if (auto *mons = root.find("monitors"); mons && mons->type == JsonValue::Array) {
        monitors.clear();
        for (const auto &mv : mons->arr) {
            if (mv.type != JsonValue::Object) continue;
            MonitorConfig m;
            if (auto *v = mv.find("id"))        m.id        = v->str_or(m.id);
            if (auto *v = mv.find("source"))    m.source    = v->str_or(m.source);
            if (auto *v = mv.find("width"))     m.width     = v->int_or(m.width);
            if (auto *v = mv.find("height"))    m.height    = v->int_or(m.height);
            if (auto *v = mv.find("scale"))     m.scale     = (float)v->num_or(m.scale);
            if (auto *v = mv.find("angle_deg")) m.angle_deg = (float)v->num_or(m.angle_deg);
            if (auto *v = mv.find("size_m"))    m.size_m    = (float)v->num_or(m.size_m);
            if (auto *v = mv.find("curvature")) m.curvature = (float)v->num_or(m.curvature);
            if (auto *v = mv.find("color"))     m.color     = parse_color(*v, m.color);
            monitors.push_back(m);
        }
    }

    // Head-tracking output (mode B) + per-game profiles.
    if (auto *ht = root.find("head_tracking"))
        headtrack_merge_json(*ht, head_tracking);

    if (auto *profs = root.find("profiles"); profs && profs->type == JsonValue::Object) {
        for (const auto &kv : profs->obj) {
            HeadTrackConfig h = head_tracking; // inherit global defaults
            if (auto *pht = kv.second.find("head_tracking"))
                headtrack_merge_json(*pht, h);   // nested form
            else
                headtrack_merge_json(kv.second, h); // flat form (profile == head_tracking)
            profiles.emplace_back(kv.first, h);
        }
    }
    if (auto *ap = root.find("active_profile")) active_profile = ap->str_or(active_profile);

    return true;
}

std::string config_default_path(const std::string &cli_path) {
    if (!cli_path.empty()) return cli_path;
    if (const char *xdg = getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return std::string(xdg) + "/xr-workspace/config.json";
    if (const char *home = getenv("HOME"); home && *home)
        return std::string(home) + "/.config/xr-workspace/config.json";
    return "config.json";
}
