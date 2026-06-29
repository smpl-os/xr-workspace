// hypr_ipc.h — minimal Hyprland IPC client (M6 orchestration).
//
// Hyprland left wlroots for its own Aquamarine backend but still speaks the
// wlr-* Wayland protocols, so capture/layer-shell keep working. For compositor
// orchestration (creating headless outputs to host virtual screens, moving
// windows onto them) we talk to Hyprland's native socket at
// $XDG_RUNTIME_DIR/hypr/$HIS/.socket.sock instead of wlr-output-management.
#pragma once

#include <string>

namespace hypr {

// True when a Hyprland instance socket is reachable ($HYPRLAND_INSTANCE_SIGNATURE).
bool available();

// Send one hyprctl-style command (e.g. "output create headless", "dispatch ...")
// and capture the reply. Returns false if the socket can't be reached.
bool request(const std::string &command, std::string &reply);

// Convenience: create a headless output (a virtual monitor Hyprland will render
// into; we then capture it). `name_out` receives Hyprland's reply text.
bool create_headless_output(std::string &name_out);

// Remove a headless output previously created.
bool remove_output(const std::string &name);

// Move the focused (or addressed) window onto a given output's workspace.
bool move_window_to_output(const std::string &output, const std::string &window_addr);

} // namespace hypr
