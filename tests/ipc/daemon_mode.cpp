#include <cerrno>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <signal.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;

#ifndef A2AD_BIN
#define A2AD_BIN "a2ad"
#endif

int main() {
    auto tmp = fs::temp_directory_path() /
               ("a2ad_daemon_mode_" + std::to_string(getpid()));
    fs::create_directories(tmp);
    auto socket_path = tmp / "a2ad.sock";
    auto db_path = tmp / "a2ad.db";
    auto config_path = tmp / "agents.yaml";

    {
        std::ofstream config(config_path);
        config << "agents: {}\n"
                  "projects: {}\n"
                  "ipc:\n"
                  "  socket: " << socket_path.string() << "\n"
                  "  db: " << db_path.string() << "\n";
    }

    pid_t launcher = fork();
    if (launcher == 0) {
        execl(A2AD_BIN, "a2ad", "--config", config_path.c_str(),
              "--no-reconcile", static_cast<char*>(nullptr));
        _exit(127);
    }
    if (launcher < 0) {
        std::perror("fork");
        fs::remove_all(tmp);
        return 1;
    }

    int launcher_status = 0;
    if (waitpid(launcher, &launcher_status, 0) < 0 ||
        !WIFEXITED(launcher_status) || WEXITSTATUS(launcher_status) != 0) {
        std::fprintf(stderr, "daemon launcher did not exit successfully\n");
        fs::remove_all(tmp);
        return 1;
    }

    pid_t daemon_pid = -1;
    std::string response;
    for (int attempt = 0; attempt < 100; ++attempt) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd >= 0) {
            sockaddr_un address{};
            address.sun_family = AF_UNIX;
            std::snprintf(address.sun_path, sizeof(address.sun_path), "%s",
                          socket_path.c_str());
            if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
#ifdef SO_PEERCRED
                ucred credentials{};
                socklen_t credentials_len = sizeof(credentials);
                if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials,
                               &credentials_len) == 0) {
                    daemon_pid = credentials.pid;
                }
#endif
                const std::string request = "{\"op\":\"ping\"}\n";
                if (send(fd, request.data(), request.size(), 0) >= 0) {
                    char buffer[512];
                    ssize_t count = recv(fd, buffer, sizeof(buffer), 0);
                    if (count > 0) response.assign(buffer, static_cast<size_t>(count));
                }
                close(fd);
                if (response.find("\"pong\":true") != std::string::npos) break;
            } else {
                close(fd);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    bool passed = response.find("\"pong\":true") != std::string::npos && daemon_pid > 0;
    if (!passed) std::fprintf(stderr, "detached daemon did not answer ping\n");
    if (daemon_pid > 0) kill(daemon_pid, SIGTERM);
    for (int attempt = 0; attempt < 40 && fs::exists(socket_path); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    fs::remove_all(tmp);
    return passed ? 0 : 1;
}
