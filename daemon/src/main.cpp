// a2ad — persistent A2A daemon for the Kitty terminal.
//
// Wires together: config, database, HTTP transport, A2A client, task manager,
// and the Unix-domain-socket IPC server. The kitten is a thin client of the
// IPC protocol.
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <chrono>
#include <atomic>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "kitty_a2a/A2AClient.hpp"
#include "kitty_a2a/Config.hpp"
#include "kitty_a2a/Credentials.hpp"
#include "kitty_a2a/Database.hpp"
#include "kitty_a2a/HttpTransport.hpp"
#include "kitty_a2a/IpcServer.hpp"
#include "kitty_a2a/TaskManager.hpp"

namespace fs = std::filesystem;

using namespace kitty_a2a;

static IpcServer* g_ipc = nullptr;   // for signal-handler wakeup
static std::atomic<bool> g_running{true};

static void onSignal(int) { g_running = false; }

static std::string error_kind_str(A2AResult::ErrorKind k) {
    switch (k) {
        case A2AResult::ErrorKind::None: return "";
        case A2AResult::ErrorKind::LocalNetwork: return "local_network";
        case A2AResult::ErrorKind::AuthRequired: return "auth_required";
        case A2AResult::ErrorKind::ProtocolError: return "protocol_error";
        case A2AResult::ErrorKind::TaskFailure: return "task_failure";
        case A2AResult::ErrorKind::MalformedResponse: return "malformed_response";
    }
    return "unknown";
}

static nlohmann::json taskToJson(const Task& t) {
    nlohmann::json tj;
    tj["task_id"] = t.id.value();
    tj["agent"] = t.agent.value();
    tj["context_id"] = t.context.value();
    tj["state"] = to_state_string(t.state);
    tj["state_message"] = t.state_message;
    tj["title"] = t.title;
    tj["cwd"] = t.cwd;
    tj["created_at"] = t.created_at;
    tj["updated_at"] = t.updated_at;
    tj["last_state_change"] = t.last_state_change;
    tj["error"] = t.error;
    nlohmann::json msgs = nlohmann::json::array();
    for (const auto& m : t.messages) {
        nlohmann::json mj;
        mj["message_id"] = m.message_id;
        mj["role"] = m.role;
        mj["text"] = m.text();
        msgs.push_back(mj);
    }
    tj["messages"] = msgs;
    nlohmann::json arts = nlohmann::json::array();
    for (const auto& a : t.artifacts) {
        nlohmann::json aj;
        aj["artifact_id"] = a.artifact_id;
        aj["name"] = a.name;
        aj["description"] = a.description;
        aj["content"] = a.content_summary();
        arts.push_back(aj);
    }
    tj["artifacts"] = arts;
    return tj;
}

static nlohmann::json summaryToJson(const TaskSummary& s) {
    return {
        {"id", s.id.value()},
        {"agent", s.agent.value()},
        {"state", to_state_string(s.state)},
        {"title", s.title},
        {"cwd", s.cwd},
        {"created_at", s.created_at},
        {"updated_at", s.updated_at},
        {"state_message", s.state_message},
        {"has_artifacts", s.has_artifacts},
    };
}

static nlohmann::json agentToJson(const Agent& a) {
    nlohmann::json aj;
    aj["id"] = a.id.value();
    aj["endpoint"] = a.endpoint;
    aj["host_label"] = a.host_label;
    aj["available"] = a.available;
    aj["last_error"] = a.last_error;
    if (a.card) {
        aj["name"] = a.card->name;
        aj["description"] = a.card->description;
        aj["version"] = a.card->version;
        aj["supports_streaming"] = a.card->capabilities.streaming;
        nlohmann::json skills = nlohmann::json::array();
        for (const auto& s : a.card->skills) {
            skills.push_back({{"id", s.id}, {"name", s.name}, {"description", s.description}});
        }
        aj["skills"] = skills;
    }
    return aj;
}

int main(int argc, char** argv) {
    std::string config_arg;
    bool foreground = false;
    bool no_reconcile = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--config" && i + 1 < argc) config_arg = argv[++i];
        else if (a == "--foreground") foreground = true;
        else if (a == "--no-reconcile") no_reconcile = true;
        else if (a == "--version") { std::printf("a2ad 0.1.0\n"); return 0; }
        else if (a == "--help") {
            std::printf("a2ad — persistent A2A daemon for Kitty\n"
                        "  --config PATH   agents.yaml to load (default: XDG config)\n"
                        "  --foreground    run in foreground (log to stderr)\n"
                        "  --no-reconcile  skip startup task reconciliation\n");
            return 0;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            return 2;
        }
    }

    auto paths = Paths::resolve();
    std::string config_path = config_arg.empty() ? paths.agents_yaml : config_arg;

    // Load config.
    std::string cfg_err;
    Config config = load_config(config_path, /*require=*/false, &cfg_err);
    if (!cfg_err.empty()) std::fprintf(stderr, "[a2ad] config: %s\n", cfg_err.c_str());
    if (config.agents.empty()) {
        std::fprintf(stderr, "[a2ad] no agents configured (config: %s); daemon idle\n", config_path.c_str());
    }

    // An explicit ipc.socket / ipc.db in the config overrides the XDG defaults.
    if (!config.ipc.socket.empty()) paths.socket = config.ipc.socket;
    if (!config.ipc.db.empty()) paths.db = config.ipc.db;

    // Open database.
    Database db;
    std::string db_err;
    std::error_code ec;
    fs::create_directories(fs::path(paths.db).parent_path(), ec);
    if (!db.open(paths.db, &db_err)) {
        std::fprintf(stderr, "[a2ad] fatal: %s\n", db_err.c_str());
        return 1;
    }

    // Build transport + credential provider + A2A client.
    auto creds = make_credential_provider();
    auto creds_shared = std::shared_ptr<CredentialProvider>(std::move(creds));
    auto transport = make_curl_transport(30000);
    auto a2a = std::make_shared<A2AClient>(transport, creds_shared);

    TaskManager tm(db, a2a, config);

    // Build the IPC server. The handler is a reference into a shared struct so
    // the event sink (wired after construction) and the task manager are both
    // reachable without capturing the (non-copyable) server itself.
    struct IpcCtx {
        TaskManager* tm = nullptr;
        IpcServer* ipc = nullptr;   // set after the server is constructed
    };
    IpcCtx ctx{&tm, nullptr};

    auto ipc_ptr = std::make_unique<IpcServer>(paths.socket, [&](const nlohmann::json& req) -> nlohmann::json {
        TaskManager& tm = *ctx.tm;
        std::string op = req.value("op", "");
        if (op == "submit") {
            CreateTaskRequest r;
            r.agent = req.value("agent", "");
            r.message = req.value("message", "");
            r.context.cwd = req.value("cwd", "");
            r.continue_task_id = req.value("task_id", "");
            r.continue_context_id = req.value("context_id", "");
            auto resp = tm.createTask(r);
            if (!resp.ok) return {{"ok", false}, {"error", resp.message}, {"error_kind", resp.error_kind}};
            return {{"ok", true},
                    {"task_id", resp.task_id.value()},
                    {"context_id", resp.context_id.value()},
                    {"state", to_state_string(resp.state)},
                    {"message", resp.message}};
        }
        if (op == "list") {
            bool include_terminal = req.value("include_terminal", true);
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& s : tm.listTasks(include_terminal)) arr.push_back(summaryToJson(s));
            return {{"ok", true}, {"tasks", arr}};
        }
        if (op == "status") {
            std::string id = req.value("task_id", "");
            bool refresh = req.value("refresh", false);
            if (refresh) {
                auto rr = tm.refreshTask(id);
                if (!rr.ok) return {{"ok", false}, {"error", rr.error}, {"error_kind", error_kind_str(rr.error_kind)}};
            }
            auto t = tm.getTask(id);
            if (!t) return {{"ok", false}, {"error", "no such task: " + id}};
            return {{"ok", true}, {"task", taskToJson(*t)}};
        }
        if (op == "respond") {
            std::string id = req.value("task_id", "");
            std::string text = req.value("message", "");
            auto resp = tm.respondToTask(id, text);
            if (!resp.ok) return {{"ok", false}, {"error", resp.message}, {"error_kind", resp.error_kind}};
            return {{"ok", true}, {"task_id", resp.task_id.value()}, {"state", to_state_string(resp.state)}};
        }
        if (op == "cancel") {
            std::string id = req.value("task_id", "");
            auto rr = tm.cancelTask(id);
            if (!rr.ok) return {{"ok", false}, {"error", rr.error}, {"error_kind", error_kind_str(rr.error_kind)}};
            return {{"ok", true}, {"task_id", id}, {"state", "CANCELED"}};
        }
        if (op == "agents") {
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& a : tm.listAgents()) arr.push_back(agentToJson(a));
            return {{"ok", true}, {"agents", arr}};
        }
        if (op == "discover") {
            std::string id = req.value("agent", "");
            auto a = tm.discoverAgent(id);
            return {{"ok", true}, {"agent", agentToJson(a)}};
        }
        if (op == "ping") {
            return {{"ok", true}, {"pong", true}};
        }
        return {{"ok", false}, {"error", "unknown op: " + op}};
    });

    std::string ipc_err;
    if (!ipc_ptr->start(&ipc_err)) {
        std::fprintf(stderr, "[a2ad] fatal: IPC: %s\n", ipc_err.c_str());
        return 1;
    }
    ctx.ipc = ipc_ptr.get();
    g_ipc = ipc_ptr.get();
    tm.setEventSink([ctx](const nlohmann::json& ev) {
        if (ctx.ipc) ctx.ipc->broadcastEvent(ev);
    });

    // Agent discovery at startup (best-effort; failures mark the agent
    // unavailable and are surfaced in `agents`).
    tm.discoverAllAgents();

    // Reconcile non-terminal tasks against their endpoints.
    if (!no_reconcile) tm.reconcileOnStartup();

    std::fprintf(stderr, "[a2ad] ready: socket=%s db=%s agents=%zu\n",
                 paths.socket.c_str(), paths.db.c_str(), config.agents.size());

    if (!foreground) {
        // Minimal double-fork daemonize.
        pid_t pid = fork();
        if (pid < 0) { std::perror("fork"); return 1; }
        if (pid > 0) return 0;   // parent exits
        if (setsid() < 0) { /* warn */ }
        pid = fork();
        if (pid < 0) { std::perror("fork"); return 1; }
        if (pid > 0) return 0;   // first child exits
        // Second child: redirect std fds to /dev/null.
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) { dup2(devnull, 0); dup2(devnull, 1); dup2(devnull, 2); }
    }

    std::signal(SIGTERM, onSignal);
    std::signal(SIGINT, onSignal);
    std::signal(SIGCHLD, SIG_IGN);

    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::fprintf(stderr, "[a2ad] shutting down\n");
    ipc_ptr->stop();
    db.close();
    return 0;
}
