#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

namespace kitty_a2a {

// Local IPC server over a Unix domain socket (DESIGN.md §5).
//
// Protocol: newline-delimited JSON (NDJSON) requests. Each request is a single
// JSON object on one line ending with '\n'. Each response is one JSON object on
// one line. This is deliberately boring and easy to debug with `nc`/curl-style
// tooling, and matches the DESIGN.md examples.
//
// The server is NOT exposed over TCP by default (DESIGN.md §5). The socket file
// lives in $XDG_RUNTIME_DIR/kitty-a2a/a2ad.sock with 0700 perms on the parent
// dir and 0600 on the socket.
//
// Threading: one listening thread + a short-lived thread per connection (a
// Kitty kitten connection is short-lived). Handlers run on the connection
// thread; they must coordinate with TaskManager via its own locks.
class IpcServer {
public:
    // A request handler receives the parsed request JSON and returns the
    // response JSON to be written back. Thrown exceptions / handler failures
    // produce an {"ok":false,...} envelope so the client never sees a bare
    // close-without-response.
    using Handler = std::function<nlohmann::json(const nlohmann::json& req)>;

    IpcServer(std::string socket_path, Handler handler);
    ~IpcServer();
    IpcServer(const IpcServer&) = delete;
    IpcServer& operator=(const IpcServer&) = delete;

    // Create the socket, bind, and start the accept loop in a background thread.
    bool start(std::string* error = nullptr);
    void stop();

    bool running() const { return running_.load(); }
    const std::string& socketPath() const { return socket_path_; }

    // Send a single JSON event to every currently connected client (used for
    // task.state_changed push notifications, DESIGN.md §18). Best-effort.
    void broadcastEvent(const nlohmann::json& event);

private:
    struct Impl;
    void acceptLoop();
    void handleConnection(int cfd);

    std::unique_ptr<Impl> impl_;
    std::string socket_path_;
    std::atomic<bool> running_{false};
    std::thread accept_thread_;
    Handler handler_;
};

}  // namespace kitty_a2a
