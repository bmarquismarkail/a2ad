#include "test_framework.hpp"
#include "kitty_a2a/IpcServer.hpp"

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <time.h>
#include <unistd.h>

using namespace kitty_a2a;
namespace fs = std::filesystem;

namespace {
double process_cpu_seconds() {
    timespec value{};
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &value);
    return static_cast<double>(value.tv_sec) + static_cast<double>(value.tv_nsec) / 1e9;
}
}  // namespace

ADD_TEST(idle_ipc_server_does_not_spin) {
    const fs::path dir = fs::temp_directory_path() /
                         ("a2ad_idle_ipc_" + std::to_string(getpid()));
    const fs::path socket = dir / "a2ad.sock";
    fs::remove_all(dir);

    IpcServer server(socket.string(), [](const nlohmann::json&) {
        return nlohmann::json{{"ok", true}};
    });
    std::string error;
    CHECK(server.start(&error));
    const double before = process_cpu_seconds();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const double used = process_cpu_seconds() - before;
    server.stop();

    // A spinning accept loop consumes approximately the full 200 ms. Leave
    // ample room for loaded CI hosts while still catching that regression.
    CHECK(used < 0.08);
    fs::remove_all(dir);
}
