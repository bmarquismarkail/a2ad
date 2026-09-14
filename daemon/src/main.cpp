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
#include <fstream>
#include <string>
#include <sys/types.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <chrono>
#include <atomic>
#include <algorithm>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "kitty_a2a/A2AClient.hpp"
#include "kitty_a2a/Config.hpp"
#include "kitty_a2a/ControlPlane.hpp"
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
    tj["remote_task_id"] = t.remote_task_id ? t.remote_task_id->value() : "";
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
        aj["supports_push_notifications"] = a.card->capabilities.push_notifications;
        aj["supports_extended_card"] = a.card->capabilities.extended_agent_card;
        aj["warnings"] = a.card->warnings;
        nlohmann::json interfaces = nlohmann::json::array();
        for (const auto& i : a.card->interfaces) interfaces.push_back({{"url", i.url}, {"protocol_binding", i.protocol_binding},
            {"protocol_version", i.protocol_version}, {"tenant", i.tenant}});
        aj["interfaces"] = interfaces;
        nlohmann::json skills = nlohmann::json::array();
        for (const auto& s : a.card->skills) {
            skills.push_back({{"id", s.id}, {"name", s.name}, {"description", s.description}});
        }
        aj["skills"] = skills;
    }
    return aj;
}

static std::string readSecretFile(const std::string& path) {
    if (path.empty()) return {};
    std::ifstream input(path); std::string value;
    std::getline(input, value); if (!value.empty() && value.back() == '\r') value.pop_back();
    return value;
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

    if (!foreground) {
        // Daemonize before opening SQLite or constructing anything that may
        // own worker threads. Only the calling thread survives fork().
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

    // Hold an instance lock before any recovery writes. A second daemon must
    // not mark the live daemon's in-flight executions uncertain.
    std::error_code ec;
    fs::create_directories(fs::path(paths.db).parent_path(), ec);
    struct InstanceLock {
        int fd = -1;
        ~InstanceLock() { if (fd >= 0) ::close(fd); }
    } instance;
    instance.fd = ::open((paths.db + ".lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    struct stat lock_stat{};
    if (instance.fd < 0 || ::fstat(instance.fd, &lock_stat) != 0 || !S_ISREG(lock_stat.st_mode) ||
        lock_stat.st_uid != ::geteuid() || ::flock(instance.fd, LOCK_EX | LOCK_NB) != 0) {
        std::fprintf(stderr, "[a2ad] cannot acquire database instance lock\n");
        return 1;
    }
    Database db;
    std::string db_err;
    if (!db.open(paths.db, &db_err)) {
        std::fprintf(stderr, "[a2ad] fatal: %s\n", db_err.c_str());
        return 1;
    }

    // Build transport + credential provider + A2A client.
    auto creds = make_credential_provider();
    auto creds_shared = std::shared_ptr<CredentialProvider>(std::move(creds));
    auto transport = make_curl_transport(30000);
    auto a2a = std::make_shared<A2AClient>(transport, creds_shared);

    ControlPlane control(paths.db, config.enforce_policy);
    TaskManager tm(db, a2a, config);

    // Build the IPC server. The handler is a reference into a shared struct so
    // the event sink (wired after construction) and the task manager are both
    // reachable without capturing the (non-copyable) server itself.
    struct IpcCtx {
        TaskManager* tm = nullptr;
        IpcServer* ipc = nullptr;   // set after the server is constructed
    };
    IpcCtx ctx{&tm, nullptr};

    auto dispatch = [&](const nlohmann::json& req) -> nlohmann::json {
        TaskManager& tm = *ctx.tm;
        std::string op = req.value("op", "");
        if (op == "submit") {
            CreateTaskRequest r;
            std::string requested_agent = req.value("agent", "");
            const std::string cwd = req.value("cwd", "");
            if (requested_agent.empty()) {
                auto routed = config.default_agent_for(cwd);
                if (!routed) return {{"ok", false}, {"error", "no agent supplied and no project default matches cwd"},
                                    {"error_kind", "no_route"}};
                requested_agent = *routed;
            }
            r.agent = requested_agent;
            r.message = req.value("message", "");
            r.request_id = req.value("request_id", "");
            r.context.cwd = cwd;
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
        if (op == "remote_list") {
            ListTasksFilter filter;
            filter.context_id = req.value("context_id", ""); filter.page_size = req.value("page_size", 100);
            filter.page_token = req.value("page_token", ""); filter.status_timestamp_after = req.value("status_timestamp_after", "");
            if (req.contains("status") && req["status"].is_string()) filter.status = parse_task_state(req["status"].get<std::string>());
            if (req.contains("history_length") && req["history_length"].is_number_integer()) filter.history_length = req["history_length"].get<int>();
            if (req.contains("include_artifacts") && req["include_artifacts"].is_boolean()) filter.include_artifacts = req["include_artifacts"].get<bool>();
            auto rr = tm.listRemoteTasks(req.value("agent", ""), filter);
            if (!rr.ok) return {{"ok", false}, {"error", rr.error},
                                {"error_kind", error_kind_str(rr.error_kind)}};
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& task : rr.tasks) arr.push_back(taskToJson(task));
            return {{"ok", true}, {"tasks", arr}, {"next_page_token", rr.next_page_token},
                    {"page_size", rr.page_size}, {"total_size", rr.total_size}};
        }
        if (op == "stream_submit") {
            nlohmann::json events = nlohmann::json::array();
            auto rr = tm.streamMessage(req.value("agent", ""), req.value("message", ""),
                TaskId(req.value("task_id", "")), ContextId(req.value("context_id", "")),
                [&](const nlohmann::json& event) { events.push_back(event); });
            if (!rr.ok) return {{"ok", false}, {"error", rr.error}, {"error_kind", error_kind_str(rr.error_kind)}, {"events", events}};
            return {{"ok", true}, {"events", events}};
        }
        if (op == "subscribe") {
            nlohmann::json updates = nlohmann::json::array();
            auto rr = tm.subscribeTask(req.value("task_id", ""), [&](const Task& task) { updates.push_back(taskToJson(task)); });
            if (!rr.ok) return {{"ok", false}, {"error", rr.error}, {"error_kind", error_kind_str(rr.error_kind)}, {"updates", updates}};
            return {{"ok", true}, {"updates", updates}};
        }
        if (op == "push.create") {
            PushNotificationConfig pc;
            pc.id = req.value("id", ""); pc.task_id = req.value("task_id", ""); pc.url = req.value("url", "");
            if (pc.url.rfind("https://", 0) != 0) return {{"ok", false}, {"error", "push callback URL must use HTTPS"}};
            pc.token = readSecretFile(req.value("token_file", "")); pc.auth_scheme = req.value("auth_scheme", "");
            pc.auth_credentials = readSecretFile(req.value("auth_file", ""));
            auto rr = tm.createPushConfig(req.value("agent", ""), pc);
            if (!rr.ok) return {{"ok", false}, {"error", rr.error}, {"error_kind", error_kind_str(rr.error_kind)}};
            nlohmann::json value = rr.value;
            if (value.is_object()) { value.erase("token"); if (value.contains("authentication")) value["authentication"].erase("credentials"); }
            return {{"ok", true}, {"config", value}};
        }
        if (op == "push.get" || op == "push.list" || op == "push.delete") {
            A2AResult rr;
            if (op == "push.get") rr = tm.getPushConfig(req.value("agent", ""), req.value("task_id", ""), req.value("id", ""));
            else if (op == "push.list") rr = tm.listPushConfigs(req.value("agent", ""), req.value("task_id", ""), req.value("page_size", 50), req.value("page_token", ""));
            else rr = tm.deletePushConfig(req.value("agent", ""), req.value("task_id", ""), req.value("id", ""));
            if (!rr.ok) return {{"ok", false}, {"error", rr.error}, {"error_kind", error_kind_str(rr.error_kind)}};
            nlohmann::json value = rr.value;
            auto redact = [](nlohmann::json& item) { if (!item.is_object()) return; item.erase("token"); if (item.contains("authentication")) item["authentication"].erase("credentials"); };
            if (value.contains("configs") && value["configs"].is_array()) for (auto& item : value["configs"]) redact(item); else redact(value);
            return {{"ok", true}, {"result", value}};
        }
        if (op == "agent.extended_card") {
            auto rr = tm.getExtendedAgentCard(req.value("agent", ""));
            if (!rr.ok) return {{"ok", false}, {"error", rr.error}, {"error_kind", error_kind_str(rr.error_kind)}};
            return {{"ok", true}, {"card", rr.value}};
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
            auto resp = tm.respondToTask(id, text, req.value("request_id", ""));
            if (!resp.ok) return {{"ok", false}, {"error", resp.message}, {"error_kind", resp.error_kind}};
            return {{"ok", true}, {"task_id", resp.task_id.value()}, {"state", to_state_string(resp.state)}};
        }
        if (op == "cancel") {
            std::string id = req.value("task_id", "");
            auto rr = tm.cancelTask(id);
            if (!rr.ok) return {{"ok", false}, {"error", rr.error}, {"error_kind", error_kind_str(rr.error_kind)}};
            return {{"ok", true}, {"task_id", id}, {"state", rr.task ? to_state_string(rr.task->state) : "UNKNOWN"}};
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
        if (op == "artifact.materialize") {
            std::string path, error;
            if (!tm.materializeArtifact(req.value("task_id", ""), req.value("artifact_id", ""),
                                        req.value("part", size_t{0}), req.value("output_dir", ""),
                                        &path, &error, req.value("sha256", ""))) return {{"ok", false}, {"error", error}};
            return {{"ok", true}, {"path", path}};
        }
        if (op == "ping") {
            return {{"ok", true}, {"pong", true}};
        }
        return {{"ok", false}, {"error", "unknown op: " + op}};
    };
    auto ipc_ptr = std::make_unique<IpcServer>(paths.socket, [&](nlohmann::json req) -> nlohmann::json {
        // Resolve routing before hashing the request so approval is bound to
        // the actual target; a configuration change invalidates prior approval.
        auto op = req.value("op", "");
        if (op == "submit" && req.value("agent", "").empty()) {
            auto routed = config.default_agent_for(req.value("cwd", ""));
            if (routed) req["agent"] = *routed;
        }
        if ((op == "respond" || op == "cancel" || op == "artifact.materialize") && req.contains("task_id")) {
            if (auto task = tm.getTask(req.value("task_id", ""))) req["agent"] = task->agent.value();
        }
        if (op == "route.explain") {
            auto selected = config.default_agent_for(req.value("cwd", ""));
            return {{"ok", true}, {"agent", selected.value_or("")},
                {"reason", selected ? "longest matching configured project path" : "no matching project rule"},
                {"health", selected ? control.health(*selected) : nlohmann::json()},
                {"fallback", "none; unavailable or quarantined targets are never silently replaced"}};
        }
        return control.handle(req, dispatch);
    });

    tm.setEventSink([&control](const nlohmann::json& ev) { control.event(ev); });
    std::string ipc_err;
    if (!ipc_ptr->start(&ipc_err)) {
        std::fprintf(stderr, "[a2ad] fatal: IPC: %s\n", ipc_err.c_str());
        return 1;
    }
    ctx.ipc = ipc_ptr.get();
    g_ipc = ipc_ptr.get();

    // Agent discovery at startup (best-effort; failures mark the agent
    // unavailable and are surfaced in `agents`).
    // Discover lazily on the first operation for each agent. Reconciliation
    // below only discovers agents that own outstanding tasks.

    // Reconcile non-terminal tasks against their endpoints.
    if (!no_reconcile) tm.reconcileOnStartup();
    tm.startSubscriptions();

    std::fprintf(stderr, "[a2ad] ready: socket=%s db=%s agents=%zu\n",
                 paths.socket.c_str(), paths.db.c_str(), config.agents.size());

    std::signal(SIGTERM, onSignal);
    std::signal(SIGINT, onSignal);
    std::signal(SIGCHLD, SIG_IGN);

    auto next_reconcile = std::chrono::steady_clock::now() +
        std::chrono::seconds(std::max(1, config.ipc.reconcile_interval_sec));
    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (config.ipc.reconcile_interval_sec > 0 && std::chrono::steady_clock::now() >= next_reconcile) {
            for (const auto& summary : tm.listTasks(false)) tm.refreshTask(summary.id.value());
            next_reconcile = std::chrono::steady_clock::now() +
                std::chrono::seconds(config.ipc.reconcile_interval_sec);
        }
    }

    std::fprintf(stderr, "[a2ad] shutting down\n");
    ipc_ptr->stop();
    // TaskManager joins subscriptions before Database is destroyed.
    return 0;
}
