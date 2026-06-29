// control.h — runtime control API over a Unix-domain socket.
// Newline-delimited JSON requests in, JSON responses out. Integrated into the
// main poll() loop (no threads). This is what external apps (e.g. the smplOS
// settings app) use to add/remove/configure monitors live.
#pragma once

#include "json.h"

#include <functional>
#include <poll.h>
#include <string>
#include <vector>

class ControlServer {
public:
    // Handler receives a parsed request object and returns a response object.
    using Handler = std::function<JsonValue(const JsonValue &)>;

    // Create + bind + listen. `socket_path` empty → default
    // ($XDG_RUNTIME_DIR/xr-workspace.sock). Returns false on failure.
    bool start(const std::string &socket_path);
    void stop();
    bool active() const { return listen_fd_ >= 0; }
    const std::string &path() const { return path_; }

    // Append listening + client fds (POLLIN) for the caller's poll() set.
    void add_pollfds(std::vector<struct pollfd> &fds);
    // After poll() returns, service any of our fds that are ready.
    void handle(const std::vector<struct pollfd> &fds, const Handler &handler);

private:
    struct Client { int fd; std::string buf; };

    int                 listen_fd_ = -1;
    std::vector<Client> clients_;
    std::string         path_;

    void accept_clients();
    bool service_client(Client &c, const Handler &handler); // false → closed
};
