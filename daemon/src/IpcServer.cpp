#include "kitty_a2a/IpcServer.hpp"

#include <nlohmann/json.hpp>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <set>

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
    size_t off = 0;
    while (off < n) {
        ssize_t r = ::send(fd, buf + off, n - off, MSG_NOSIGNAL);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) { usleep(1000); continue; }
            return false;
        }
        off += (size_t)r;
    }
    return true;
}
}  // namespace

struct IpcServer::Impl {
    int listen_fd = -1;
    std::set<int> clients;       // connected client fds
    std::set<int> serving;       // fds currently inside handler_ (excluded from broadcast)
    std::mutex clients_mutex;
};

IpcServer::IpcServer(std::string socket_path, Handler handler)
    : impl_(std::make_unique<Impl>()), socket_path_(std::move(socket_path)), handler_(std::move(handler)) {}

IpcServer::~IpcServer() { stop(); }

bool IpcServer::start(std::string* error) {
    // Remove any stale socket from a previous run.
    if (fs::exists(socket_path_)) {
        fs::remove(socket_path_);
    }
    // Ensure the parent directory exists (0700 so the socket isn't exposed).
    auto parent = fs::path(socket_path_).parent_path();
    std::error_code ec;
    fs::create_directories(parent, ec);
    if (parent.empty() || fs::exists(parent)) {
        ::chmod(parent.c_str(), 0700);
    }

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
    if (::chmod(socket_path_.c_str(), 0600) < 0) { /* warn-level */ }

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
    while (total < kMaxMessage) {
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
        impl_->listen_fd = -1;
    }
    if (accept_thread_.joinable()) accept_thread_.join();

    std::lock_guard lk(impl_->clients_mutex);
    for (int cfd : impl_->clients) ::close(cfd);
    impl_->clients.clear();

    fs::remove(socket_path_);
}

}  // namespace kitty_a2a
