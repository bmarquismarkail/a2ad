// End-to-end integration test: spin up a mock A2A JSON-RPC server on a local
// port, launch the daemon as a subprocess pointed at it, and drive the full
// IPC protocol (submit / list / status / agents) over the real Unix socket.
//
// The mock server is a tiny Python http.server subprocess (no extra deps).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <chrono>
#include <thread>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cerrno>

namespace fs = std::filesystem;

// Baked in by CMake (A2AD_BIN) so the test can exec the daemon regardless of
// the current working directory. Falls back to "a2ad" on PATH if undefined.
#ifndef A2AD_BIN
#define A2AD_BIN "a2ad"
#endif

static int g_passed = 0, g_failed = 0;
#define CHECK(cond) do { \
    if (cond) ++g_passed; \
    else { ++g_failed; std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

static std::string slurp(const fs::path& p) {
    std::ifstream f(p);
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return s;
}

// Find a free port by binding a socket to port 0 and reading back the assigned
// port. Returns 0 on failure.
static int find_free_port() {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return 0;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(s, (sockaddr*)&addr, sizeof(addr)) < 0) { close(s); return 0; }
    socklen_t len = sizeof(addr);
    if (getsockname(s, (sockaddr*)&addr, &len) < 0) { close(s); return 0; }
    int port = ntohs(addr.sin_port);
    close(s);
    return port;
}

// Minimal JSON over the socket: send one NDJSON request, read one NDJSON line.
// Returns "" on failure.
static std::string ipc_roundtrip(const std::string& sock, const std::string& request) {
    int cfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (cfd < 0) return "";
    sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    if (snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", sock.c_str()) >= (int)sizeof(sa.sun_path)) {
        close(cfd); return "";
    }
    if (connect(cfd, (sockaddr*)&sa, sizeof(sa)) < 0) { close(cfd); return ""; }
    // Send request + newline.
    std::string msg = request + "\n";
    if (send(cfd, msg.data(), msg.size(), 0) < 0) { close(cfd); return ""; }
    // Read a line (NDJSON).
    std::string line;
    char buf[4096];
    while (true) {
        ssize_t n = recv(cfd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) break;
        buf[n] = 0;
        line.append(buf, n);
        if (line.find('\n') != std::string::npos) break;
        if (line.size() > 1024 * 1024) break;
    }
    close(cfd);
    // Strip trailing newline.
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    return line;
}

int main() {
    auto tmp = fs::temp_directory_path() / ("a2ad_e2e_" + std::to_string(getpid()));
    fs::create_directories(tmp);

    int port = find_free_port();
    CHECK(port > 0);
    if (port <= 0) { std::fprintf(stderr, "no free port\n"); return 1; }

    // --- Mock A2A server (Python) ------------------------------------------
    auto mockpy = tmp / "mock_a2a.py";
    auto sockpath = tmp / "a2ad.sock";
    auto dbpath = tmp / "a2ad.db";
    auto logpath = tmp / "daemon.log";
    {
        std::ofstream f(mockpy);
        f <<
        "#!/usr/bin/env python3\n"
        "import json, sys, uuid\n"
        "from http.server import BaseHTTPRequestHandler, HTTPServer\n"
        "\n"
        "PORT = int(sys.argv[1])\n"
        "TASKS = {}\n"
        "COUNTER = [0]\n"
        "\n"
        "class H(BaseHTTPRequestHandler):\n"
        "    def log_message(self, *a): pass\n"
        "    def _send(self, code, obj):\n"
        "        body = json.dumps(obj).encode()\n"
        "        self.send_response(code)\n"
        "        self.send_header('Content-Type','application/json')\n"
        "        self.send_header('Content-Length', str(len(body)))\n"
        "        self.end_headers()\n"
        "        self.wfile.write(body)\n"
        "    def do_GET(self):\n"
        "        if self.path == '/.well-known/agent-card.json':\n"
        "            self._send(200, {\n"
        "                'name':'mock-agent','description':'e2e mock','version':'1.0.0',\n"
        "                'capabilities':{'streaming':False},\n"
        "                'supportedInterfaces':[{'url':'http://127.0.0.1:%d/a2a','protocolBinding':'jsonrpc','protocolVersion':'1.0.0'}],\n"
        "                'securitySchemes':{}})\n"
        "        else:\n"
        "            self._send(404, {'error':'not found'})\n"
        "    def do_POST(self):\n"
        "        ln = int(self.headers.get('Content-Length','0'))\n"
        "        raw = self.rfile.read(ln)\n"
        "        try: req = json.loads(raw)\n"
        "        except Exception: self._send(400, {'error':'bad json'}); return\n"
        "        rid = req.get('id')\n"
        "        m = req.get('method')\n"
        "        p = req.get('params', {})\n"
        "        if m == 'SendMessage':\n"
        "            COUNTER[0] += 1\n"
        "            tid = 'e2e-task-%d' % COUNTER[0]\n"
        "            ctx = 'e2e-ctx-%d' % COUNTER[0]\n"
        "            TASKS[tid] = {'id': tid, 'contextId': ctx,\n"
        "                'status':{'state':'TASK_STATE_WORKING','message':'started'},\n"
        "                'history':[], 'artifacts':[]}\n"
        "            self._send(200, {'jsonrpc':'2.0','id':rid,'result':{'task':TASKS[tid]}})\n"
        "        elif m == 'GetTask':\n"
        "            tid = p.get('id')\n"
        "            if tid in TASKS: self._send(200, {'jsonrpc':'2.0','id':rid,'result':{'task':TASKS[tid]}})\n"
        "            else: self._send(200, {'jsonrpc':'2.0','id':rid,'error':{'code':-32001,'message':'Task not found'}})\n"
        "        elif m == 'CancelTask':\n"
        "            tid = p.get('id')\n"
        "            if tid in TASKS: TASKS[tid]['status']={'state':'TASK_STATE_CANCELED','message':'canceled'}\n"
        "            self._send(200, {'jsonrpc':'2.0','id':rid,'result':{'task':TASKS.get(tid,{})}})\n"
        "        else:\n"
        "            self._send(200, {'jsonrpc':'2.0','id':rid,'result':{}})\n"
        "if __name__=='__main__':\n"
        "    HTTPServer(('127.0.0.1', PORT), H).serve_forever()\n"
        << port;
        f.close();
    }

    // --- agents.yaml pointing at the mock ----------------------------------
    auto agents = tmp / "agents.yaml";
    {
        std::ofstream f(agents);
        f << "agents:\n"
              "  mock:\n"
              "    endpoint: http://127.0.0.1:" << port << "/a2a\n"
              "    host_label: mock\n"
              "projects: {}\n"
              "ipc:\n"
              "  socket: " << sockpath.string() << "\n"
              "  db: " << dbpath.string() << "\n"
              "  reconcile_interval_sec: 0\n";
        f.close();
    }

    // --- Launch the mock server (background) --------------------------------
    pid_t mockpid = fork();
    if (mockpid == 0) {
        // child: run python mock
        std::string pylog = slurp("/dev/null");
        (void)pylog;
        int lf = open((tmp / "mock.log").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (lf >= 0) { dup2(lf, 1); dup2(lf, 2); }
        execlp("python3", "python3", mockpy.c_str(), std::to_string(port).c_str(), (char*)nullptr);
        _exit(127);
    }
    // --- Launch the daemon (background) -------------------------------------
    pid_t dpid = fork();
    if (dpid == 0) {
        int lf = open(logpath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (lf >= 0) { dup2(lf, 1); dup2(lf, 2); }
        execlp(A2AD_BIN, "a2ad",
               "--config", agents.c_str(),
               "--foreground", "--no-reconcile",
               (char*)nullptr);
        _exit(127);
    }

    // Wait for the mock server to accept TCP connections on its port.
    bool mock_up = false;
    for (int i = 0; i < 100; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        int s = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (s >= 0 && connect(s, (sockaddr*)&a, sizeof(a)) == 0) { mock_up = true; close(s); break; }
        if (s >= 0) close(s);
    }
    CHECK(mock_up);
    if (!mock_up) { std::fprintf(stderr, "mock server did not come up\n"); }

    // Wait for both the socket and the daemon's ready line.
    bool daemon_ready = false;
    for (int i = 0; i < 100; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (fs::exists(sockpath)) {
            std::string log = slurp(logpath);
            if (log.find("[a2ad] ready") != std::string::npos) { daemon_ready = true; break; }
        }
    }
    CHECK(daemon_ready);
    if (!daemon_ready) {
        std::fprintf(stderr, "daemon did not become ready. log:\n%s\n", slurp(logpath).c_str());
    }

    int rc = 1;
    if (daemon_ready) {
        // --- ping -----------------------------------------------------------
        std::string r = ipc_roundtrip(sockpath.string(), R"({"op":"ping"})");
        CHECK(r.find("\"pong\":true") != std::string::npos);

        // --- submit ---------------------------------------------------------
        r = ipc_roundtrip(sockpath.string(),
            R"({"op":"submit","agent":"mock","message":"run the emulator build","cwd":"/data/projects/proto-time"})");
        std::string task_id;
        // Extract "task_id":"..."
        auto pos = r.find("\"task_id\":\"");
        if (pos != std::string::npos) {
            pos += 11;
            auto end = r.find('"', pos);
            task_id = r.substr(pos, end - pos);
        }
        CHECK(!task_id.empty());
        CHECK(r.find("\"ok\":true") != std::string::npos);
        std::fprintf(stderr, "  submitted task_id=%s (resp=%s)\n", task_id.c_str(), r.c_str());

        // --- list -----------------------------------------------------------
        r = ipc_roundtrip(sockpath.string(), R"({"op":"list"})");
        CHECK(r.find("\"ok\":true") != std::string::npos);
        CHECK(!task_id.empty() && r.find(task_id) != std::string::npos);

        // --- status (no refresh) --------------------------------------------
        r = ipc_roundtrip(sockpath.string(),
            std::string(R"({"op":"status","task_id":")") + task_id + R"("})");
        CHECK(r.find("\"ok\":true") != std::string::npos);
        CHECK(r.find("WORKING") != std::string::npos);

        // --- status (refresh, hits the mock GetTask) ------------------------
        r = ipc_roundtrip(sockpath.string(),
            std::string(R"({"op":"status","task_id":")") + task_id + R"(","refresh":true})");
        CHECK(r.find("\"ok\":true") != std::string::npos);

        // --- cancel ---------------------------------------------------------
        r = ipc_roundtrip(sockpath.string(),
            std::string(R"({"op":"cancel","task_id":")") + task_id + R"("})");
        CHECK(r.find("\"ok\":true") != std::string::npos);
        // After cancel, status should show CANCELED.
        r = ipc_roundtrip(sockpath.string(),
            std::string(R"({"op":"status","task_id":")") + task_id + R"("})");
        CHECK(r.find("CANCELED") != std::string::npos);

        // --- agents ---------------------------------------------------------
        r = ipc_roundtrip(sockpath.string(), R"({"op":"agents"})");
        CHECK(r.find("\"ok\":true") != std::string::npos);
        CHECK(r.find("mock-agent") != std::string::npos || r.find("mock") != std::string::npos);

        if (g_failed == 0) rc = 0;
    }

    // --- Cleanup ------------------------------------------------------------
    kill(dpid, SIGTERM);
    kill(mockpid, SIGTERM);
    int status;
    waitpid(dpid, &status, 0);
    waitpid(mockpid, &status, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    fs::remove(sockpath);
    fs::remove(dbpath);
    fs::remove(logpath);
    fs::remove(mockpy);
    fs::remove(tmp / "mock.log");
    fs::remove_all(tmp);

    std::fprintf(stderr, "\nE2E: %d passed, %d failed\n", g_passed, g_failed);
    if (g_failed > 0) {
        std::fprintf(stderr, "daemon log:\n%s\n", slurp(logpath).c_str());
    }
    return rc;
}
