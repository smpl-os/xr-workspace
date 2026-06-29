// hypr_ipc.cpp — see hypr_ipc.h.
#include "hypr_ipc.h"

#include <cstdlib>
#include <cstring>
#include <string>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace hypr {
namespace {

// Resolve the Hyprland command socket path. Newer Hyprland places sockets under
// $XDG_RUNTIME_DIR/hypr/<signature>/.socket.sock; older builds used
// /tmp/hypr/<signature>/.socket.sock.
std::string socket_path() {
    const char *his = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (!his || !*his) return "";

    if (const char *xdg = getenv("XDG_RUNTIME_DIR"); xdg && *xdg) {
        std::string p = std::string(xdg) + "/hypr/" + his + "/.socket.sock";
        return p;
    }
    return std::string("/tmp/hypr/") + his + "/.socket.sock";
}

} // namespace

bool available() { return !socket_path().empty(); }

bool request(const std::string &command, std::string &reply) {
    const std::string path = socket_path();
    if (path.empty() || path.size() >= sizeof(((sockaddr_un *)nullptr)->sun_path))
        return false;

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(fd, (sockaddr *)&addr, sizeof(addr)) < 0) { close(fd); return false; }

    if (write(fd, command.data(), command.size()) < 0) { close(fd); return false; }

    reply.clear();
    char buf[4096];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0) { close(fd); return false; }
        if (n == 0) break;
        reply.append(buf, (size_t)n);
    }
    close(fd);
    return true;
}

bool create_headless_output(std::string &name_out) {
    return request("output create headless", name_out);
}

bool remove_output(const std::string &name) {
    std::string reply;
    return request("output remove " + name, reply);
}

bool move_window_to_output(const std::string &output, const std::string &window_addr) {
    std::string reply;
    std::string cmd = window_addr.empty()
        ? "dispatch movecurrentworkspacetomonitor " + output
        : "dispatch movewindowtoworkspace address:" + window_addr + "," + output;
    return request(cmd, reply);
}

} // namespace hypr
