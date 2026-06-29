// control.cpp
#include "control.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static std::string default_socket_path() {
    if (const char *xdg = getenv("XDG_RUNTIME_DIR"); xdg && *xdg)
        return std::string(xdg) + "/xr-workspace.sock";
    return "/tmp/xr-workspace.sock";
}

static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

bool ControlServer::start(const std::string &socket_path) {
    path_ = socket_path.empty() ? default_socket_path() : socket_path;

    if (path_.size() >= sizeof(((sockaddr_un *)nullptr)->sun_path)) {
        fprintf(stderr, "xr-workspace: control socket path too long: %s\n", path_.c_str());
        return false;
    }

    listen_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd_ < 0) { perror("xr-workspace: socket"); return false; }

    unlink(path_.c_str()); // remove a stale socket from a previous run

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path_.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(listen_fd_, (sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("xr-workspace: bind");
        close(listen_fd_); listen_fd_ = -1; return false;
    }
    chmod(path_.c_str(), 0600); // user-only

    if (listen(listen_fd_, 8) < 0) {
        perror("xr-workspace: listen");
        close(listen_fd_); listen_fd_ = -1; unlink(path_.c_str()); return false;
    }
    set_nonblocking(listen_fd_);
    fprintf(stderr, "xr-workspace: control socket at %s\n", path_.c_str());
    return true;
}

void ControlServer::stop() {
    for (auto &c : clients_) close(c.fd);
    clients_.clear();
    if (listen_fd_ >= 0) { close(listen_fd_); listen_fd_ = -1; }
    if (!path_.empty()) unlink(path_.c_str());
}

void ControlServer::add_pollfds(std::vector<struct pollfd> &fds) {
    if (listen_fd_ < 0) return;
    fds.push_back({listen_fd_, POLLIN, 0});
    for (auto &c : clients_) fds.push_back({c.fd, POLLIN, 0});
}

void ControlServer::accept_clients() {
    for (;;) {
        int fd = accept(listen_fd_, nullptr, nullptr);
        if (fd < 0) break; // EAGAIN / no more pending
        set_nonblocking(fd);
        clients_.push_back({fd, {}});
    }
}

bool ControlServer::service_client(Client &c, const Handler &handler) {
    char tmp[4096];
    for (;;) {
        ssize_t n = read(c.fd, tmp, sizeof(tmp));
        if (n > 0) {
            c.buf.append(tmp, (size_t)n);
            if (c.buf.size() > (1u << 20)) { c.buf.clear(); return false; } // 1 MB guard
            continue;
        }
        if (n == 0) return false; // peer closed
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        if (errno == EINTR) continue;
        return false; // hard error
    }

    // Process all complete, newline-terminated requests.
    size_t nl;
    while ((nl = c.buf.find('\n')) != std::string::npos) {
        std::string line = c.buf.substr(0, nl);
        c.buf.erase(0, nl + 1);
        if (line.empty()) continue;

        JsonValue req, resp;
        std::string err;
        if (!json_parse(line, req, err)) {
            resp = jobj({{"ok", jbool(false)}, {"error", jstr("parse error: " + err)}});
        } else {
            resp = handler(req);
        }
        std::string out = json_dump(resp);
        out += '\n';
        // Best-effort write; ignore short writes for this simple control channel.
        ssize_t w = write(c.fd, out.data(), out.size());
        (void)w;
    }
    return true;
}

void ControlServer::handle(const std::vector<struct pollfd> &fds, const Handler &handler) {
    if (listen_fd_ < 0) return;

    for (const auto &p : fds) {
        if (p.fd == listen_fd_ && (p.revents & POLLIN))
            accept_clients();
    }

    for (auto it = clients_.begin(); it != clients_.end();) {
        bool keep = true;
        for (const auto &p : fds) {
            if (p.fd == it->fd && (p.revents & (POLLIN | POLLHUP | POLLERR))) {
                keep = service_client(*it, handler);
                break;
            }
        }
        if (!keep) { close(it->fd); it = clients_.erase(it); }
        else       ++it;
    }
}
