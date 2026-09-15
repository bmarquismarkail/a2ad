#include "kitty_a2a/IpcServer.hpp"

#include <nlohmann/json.hpp>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <poll.h>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <set>
#include <chrono>

namespace fs = std::filesystem;

namespace kitty_a2a {

namespace {
constexpr size_t kMaxMessage = 4 * 1024 * 1024;  // 4 MiB request cap

int make_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// Send a full buffer, retrying on EINTR/EAGAIN. Returns true on full send.
bool send_all(int fd, const char* buf, size_t n) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    size_t off = 0;
    while (off < n && std::chrono::steady_clock::now() < deadline) {
        ssize_t r = ::send(fd, buf + off, n - off, MSG_NOSIGNAL);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) { usleep(1000); continue; }
            return false;
        }
        if (r == 0) return false;
        off += (size_t)r;
    }
    return off == n;
}
}  // namespace

struct IpcServer::Impl {
    int listen_fd = -1;
    bool bound = false;
    std::set<int> clients;       // connected client fds
    std::set<int> serving;       // fds currently inside handler_ (excluded from broadcast)
    std::mutex clients_mutex;
};

IpcServer::IpcServer(std::string socket_path, Handler handler)
    : impl_(std::make_unique<Impl>()), socket_path_(std::move(socket_path)), handler_(std::move(handler)) {}

IpcServer::~IpcServer() { stop(); }

bool IpcServer::start(std::string* error) {
    // Refuse to remove arbitrary paths or a socket owned by another user.
    struct stat existing{};
    if (::lstat(socket_path_.c_str(), &existing) == 0) {
        if (!S_ISSOCK(existing.st_mode) || existing.st_uid != ::geteuid()) {
            if (error) *error = "socket path exists and is not an owned socket";
            return false;
        }
        int probe = ::socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un address{}; address.sun_family = AF_UNIX;
        if (socket_path_.size() >= sizeof(address.sun_path)) { if (probe >= 0) ::close(probe); return false; }
        std::strncpy(address.sun_path, socket_path_.c_str(), sizeof(address.sun_path) - 1);
        if (probe < 0) return false;
        int connected = ::connect(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        int connect_error = errno; ::close(probe);
        if (connected == 0 || connect_error != ECONNREFUSED) {
            if (error) *error = "socket is active or cannot be safely replaced";
            return false;
        }
        if (::unlink(socket_path_.c_str()) != 0) return false;
    }
    auto parent = fs::path(socket_path_).parent_path();
    std::error_code ec;
    bool created = fs::create_directories(parent, ec);
    if (ec) { if (error) *error = "cannot create socket directory: " + ec.message(); return false; }
    // Never chmod a caller's pre-existing directory (e.g. /tmp).
    if (created && ::chmod(parent.c_str(), 0700) != 0) return false;

    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { if (error) *error = "socket() failed: " + std::string(std::strerror(errno)); return false; }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socket_path_.size() >= sizeof(addr.sun_path)) {
        ::close(fd);
        if (error) *error = "socket path too long: " + socket_path_;
        return false;
    }
    std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    socklen_t len = sizeof(addr);
    if (::bind(fd, (struct sockaddr*)&addr, len) < 0) {
        ::close(fd);
        if (error) *error = "bind() failed on " + socket_path_ + ": " + std::string(std::strerror(errno));
        return false;
    }
    impl_->bound = true;
    if (::chmod(socket_path_.c_str(), 0600) < 0) { ::close(fd); if (error) *error = "cannot secure socket permissions"; return false; }

    if (::listen(fd, 16) < 0) {
        ::close(fd);
        if (error) *error = "listen() failed: " + std::string(std::strerror(errno));
        return false;
    }
    make_nonblock(fd);
    impl_->listen_fd = fd;
    running_ = true;

    accept_thread_ = std::thread([this] { this->acceptLoop(); });
    return true;
}

void IpcServer::acceptLoop() {
    while (running_.load()) {
        pollfd ready{impl_->listen_fd, POLLIN, 0};
        int polled = ::poll(&ready, 1, -1);
        if (polled < 0) {
            if (errno == EINTR) continue;
            if (running_.load()) continue;
            break;
        }
        if (!running_.load()) break;
        if (!(ready.revents & POLLIN)) {
            // POLLNVAL/POLLHUP is expected when stop() closes the listener.
            if (ready.revents & (POLLNVAL | POLLHUP | POLLERR)) break;
            continue;
        }
        int cfd = ::accept(impl_->listen_fd, nullptr, nullptr);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            if (running_.load()) continue;
            break;
        }
        handleConnection(cfd);
    }
}

void IpcServer::handleConnection(int cfd) {
#ifdef SO_PEERCRED
    struct ucred peer{}; socklen_t peer_size = sizeof(peer);
    if (::getsockopt(cfd, SOL_SOCKET, SO_PEERCRED, &peer, &peer_size) != 0 || peer.uid != ::geteuid()) {
        ::close(cfd); return;
    }
#endif
    make_nonblock(cfd);
    {
        std::lock_guard lk(impl_->clients_mutex);
        impl_->clients.insert(cfd);
    }

    // Read one NDJSON request line, dispatch, write one NDJSON response line,
    // close. A kitten connection is a single short-lived exchange.
    std::string line;
    char buf[4096];
    size_t total = 0;
    bool got_request = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (running_.load() && total < kMaxMessage && std::chrono::steady_clock::now() < deadline) {
        ssize_t r = ::recv(cfd, buf, sizeof(buf), 0);
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) { usleep(2000); continue; }
            break;
        }
        if (r == 0) break;  // peer closed
        for (ssize_t i = 0; i < r; ++i) {
            char c = buf[i];
            if (c == '\n') { got_request = true; goto done_read; }
            line.push_back(c);
            if (line.size() >= kMaxMessage) goto done_read;
        }
        total += (size_t)r;
    }
done_read:
    nlohmann::json resp;
    if (got_request && !line.empty()) {
        // Mark this fd as "serving" so a broadcast triggered *inside* the
        // handler (e.g. a state-change event from the very request we are
        // answering) does NOT corrupt this connection's NDJSON framing. The
        // response is the single line this client expects.
        {
            std::lock_guard lk(impl_->clients_mutex);
            impl_->serving.insert(cfd);
        }
        try {
            nlohmann::json req = nlohmann::json::parse(line);
            resp = handler_(req);
        } catch (const std::exception& e) {
            resp = {{"ok", false}, {"error", std::string("bad request: ") + e.what()}};
        }
        {
            std::lock_guard lk(impl_->clients_mutex);
            impl_->serving.erase(cfd);
        }
    } else {
        resp = {{"ok", false}, {"error", "no request received"}};
    }
    std::string out = resp.dump() + "\n";
    send_all(cfd, out.data(), out.size());

    {
        std::lock_guard lk(impl_->clients_mutex);
        impl_->clients.erase(cfd);
    }
    ::close(cfd);
}

void IpcServer::broadcastEvent(const nlohmann::json& event) {
    std::string out = event.dump() + "\n";
    std::lock_guard lk(impl_->clients_mutex);
    for (int cfd : impl_->clients) {
        // Never inject an event into a connection that is mid-request — that
        // would interleave the event line with the pending response and break
        // the one-request-one-response NDJSON contract.
        if (impl_->serving.count(cfd)) continue;
        send_all(cfd, out.data(), out.size());
    }
}

void IpcServer::stop() {
    if (!running_.exchange(false)) {
        // Still clean up if it was never started or already stopped.
    }
    if (impl_->listen_fd >= 0) {
        // close() unblocks accept()
        ::shutdown(impl_->listen_fd, SHUT_RDWR);
        ::close(impl_->listen_fd);

    }
    {
        std::lock_guard lk(impl_->clients_mutex);
        for (int cfd : impl_->clients) ::shutdown(cfd, SHUT_RDWR);
    }
    if (accept_thread_.joinable()) accept_thread_.join();
    impl_->listen_fd = -1;

    std::lock_guard lk(impl_->clients_mutex);
    for (int cfd : impl_->clients) ::close(cfd);
    impl_->clients.clear();

    std::error_code ec;
    if (impl_->bound) { fs::remove(socket_path_, ec); impl_->bound = false; }
}

}  // namespace kitty_a2a
