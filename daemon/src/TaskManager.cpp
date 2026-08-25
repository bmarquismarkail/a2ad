#include "kitty_a2a/TaskManager.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace kitty_a2a {

namespace {
std::string now_iso() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

// Derive a short human title from a message (first ~40 chars, first line).
std::string titleFromMessage(const std::string& msg) {
    std::string first = msg;
    auto nl = first.find('\n');
    if (nl != std::string::npos) first = first.substr(0, nl);
    // strip leading/trailing whitespace
    size_t s = first.find_first_not_of(" \t");
    if (s == std::string::npos) return "(untitled)";
    first = first.substr(s);
    size_t e = first.find_last_not_of(" \t");
    first = first.substr(0, e + 1);
    if (first.size() > 48) first = first.substr(0, 48) + "…";
    return first.empty() ? "(untitled)" : first;
}

std::string error_kind_name(A2AResult::ErrorKind k) {
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

std::optional<std::string> decode_base64(const std::string& input) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int table[256]; std::fill(std::begin(table), std::end(table), -1);
    for (int i = 0; i < 64; ++i) table[static_cast<unsigned char>(alphabet[i])] = i;
    std::string out; int val = 0, bits = -8;
    for (unsigned char c : input) {
        if (c == '=') break;
        if (std::isspace(c)) continue;
        if (table[c] < 0) return std::nullopt;
        val = (val << 6) + table[c]; bits += 6;
        if (bits >= 0) { out.push_back(char((val >> bits) & 0xff)); bits -= 8; }
    }
    return out;
}

std::string safe_filename(std::string name, const Artifact& artifact, size_t index) {
    name = fs::path(name).filename().string();
    if (name.empty() || name == "." || name == "..") name = artifact.name;
    name = fs::path(name).filename().string();
    if (name.empty()) name = artifact.artifact_id + "-part-" + std::to_string(index);
    return name;
}
}  // namespace

TaskManager::TaskManager(Database& db, std::shared_ptr<A2AClient> a2a, const Config& config)
    : db_(db), a2a_(std::move(a2a)), config_(config) {
    for (Agent a : db_.loadAgents()) {
        agents_[a.id.value()] = std::move(a);
    }
    // Ensure config-defined agents exist in memory even if not yet discovered.
    for (const auto& [id, ac] : config_.agents) {
        if (agents_.find(id) == agents_.end()) {
            Agent a;
            a.id = id;
            a.endpoint = ac.endpoint;
            a.host_label = ac.host_label.empty() ? id : ac.host_label;
            agents_[id] = a;
        }
    }
}

TaskManager::~TaskManager() = default;

void TaskManager::startSubscriptions() {
    for (const Task& persisted : db_.listNonTerminalTasks()) {
        startSubscription(persisted);
    }
}

void TaskManager::startSubscription(const Task& task) {
    auto agent = agents_.find(task.agent.value());
    if (agent == agents_.end() || !agent->second.supports_streaming() || is_terminal(task.state)) return;
    const std::string task_id = task.id.value();
    const AgentId agent_id = task.agent;
    subscriptions_.emplace_back([this, task_id, agent_id](std::stop_token stop) {
            while (!stop.stop_requested()) {
                std::string endpoint; AuthSpec auth;
                if (!resolveEndpoint(agent_id, &endpoint, &auth)) return;
                a2a_->subscribeToTask(endpoint, auth, TaskId(task_id), [this, task_id](const Task& update) {
                    auto existing = db_.getTask(task_id);
                    if (!existing) return;
                    Task merged = update;
                    if (merged.id.empty()) merged.id = existing->id;
                    merged.agent = existing->agent; merged.title = existing->title;
                    merged.cwd = existing->cwd; merged.created_at = existing->created_at;
                    merged.updated_at = now_iso();
                    db_.updateTask(merged);
                    emitStateChanged(task_id, existing->state, merged.state);
                }, stop);
                if (stop.stop_requested()) return;
                for (int i = 0; i < 20 && !stop.stop_requested(); ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
    });
}

bool TaskManager::resolveEndpoint(const AgentId& id, std::string* endpoint, AuthSpec* auth) const {
    auto it = agents_.find(id.value());
    if (it == agents_.end()) return false;
    const Agent& a = it->second;
    *endpoint = a.effective_endpoint();

    // Resolve auth from the config's non-secret reference.
    auto cit = config_.agents.find(id.value());
    std::string auth_type = "none", auth_name;
    if (cit != config_.agents.end()) {
        auth_type = cit->second.auth_type;
        auth_name = cit->second.auth_name;
    }
    if (a2a_ && a2a_->credentials()) {
        *auth = a2a_->credentials()->resolve(auth_type, auth_name);
    }
    return true;
}

void TaskManager::reconcileOnStartup() {
    for (Task t : db_.listNonTerminalTasks()) {
        std::string endpoint;
        AuthSpec auth;
        if (!resolveEndpoint(t.agent, &endpoint, &auth)) {
            std::fprintf(stderr, "[a2ad] reconcile: agent %s for task %s no longer configured; keeping last state\n",
                         t.agent.value().c_str(), t.id.value().c_str());
            continue;
        }
        A2AResult r = a2a_->getTask(endpoint, auth, t.id);
        if (r.ok && r.task) {
            // Merge the fresh remote state into the persisted task.
            Task fresh = std::move(*r.task);
            // Preserve local bookkeeping fields.
            if (fresh.id.empty()) fresh.id = t.id;
            if (fresh.title.empty()) fresh.title = t.title;
            if (fresh.cwd.empty()) fresh.cwd = t.cwd;
            if (fresh.created_at.empty()) fresh.created_at = t.created_at;
            fresh.agent = t.agent;
            fresh.updated_at = now_iso();
            db_.updateTask(fresh);
            std::fprintf(stderr, "[a2ad] reconcile: task %s now %s\n",
                         t.id.value().c_str(), to_state_string(fresh.state).c_str());
        } else if (r.error_kind == A2AResult::ErrorKind::LocalNetwork) {
            // Keep last known state; do NOT fake completion (DESIGN.md §27).
            std::fprintf(stderr, "[a2ad] reconcile: task %s endpoint unreachable (%s); keeping last state\n",
                         t.id.value().c_str(), r.error.c_str());
        } else {
            // Protocol/auth failure: mark the local record with a note but keep
            // the last known state.
            t.error = "reconciliation failed: " + r.error;
            db_.updateTask(t);
            std::fprintf(stderr, "[a2ad] reconcile: task %s failed to refresh (%s)\n",
                         t.id.value().c_str(), r.error.c_str());
        }
    }
}

CreateTaskResponse TaskManager::createTask(const CreateTaskRequest& req) {
    CreateTaskResponse resp;
    std::string endpoint;
    AuthSpec auth;
    if (!resolveEndpoint(req.agent, &endpoint, &auth)) {
        resp.ok = false;
        resp.error_kind = "unknown_agent";
        resp.message = "unknown agent: " + req.agent.value();
        return resp;
    }

    // Attach context to the message per DESIGN.md §10 (cwd is the primary
    // terminal context carried into the A2A message text).
    std::string payload = req.message;
    if (!req.context.cwd.empty()) {
        payload += "\n[context]\nworking directory: " + req.context.cwd;
    }

    A2AResult r = a2a_->sendMessage(endpoint, auth, payload, req.continue_task_id, req.continue_context_id);

    if (!r.ok) {
        resp.ok = false;
        resp.error_kind = error_kind_name(r.error_kind);
        resp.message = r.error;
        return resp;
    }

    Task t;
    if (r.task) {
        t = std::move(*r.task);
        if (t.id.empty()) t.id = std::to_string(std::time(nullptr));
        if (t.agent.value().empty()) t.agent = req.agent;
        if (t.context.value().empty()) t.context = req.continue_context_id.value();
        if (t.title.empty()) t.title = titleFromMessage(req.message);
        if (t.cwd.empty()) t.cwd = req.context.cwd;
        if (t.created_at.empty()) t.created_at = now_iso();
    } else {
        // The agent replied with a bare Message (no task object).
        t.id = "msg-" + std::to_string(std::time(nullptr));
        t.agent = req.agent;
        t.context = r.context_id;
        t.state = TaskState::Submitted;
        t.title = titleFromMessage(req.message);
        t.cwd = req.context.cwd;
        t.created_at = now_iso();
        Message m;
        m.role = "agent";
        m.message_id = "m0";
        Part p; p.text = r.message_text;
        m.parts.push_back(p);
        t.messages.push_back(std::move(m));
        t.updated_at = now_iso();
    }
    t.updated_at = now_iso();
    t.last_state_change = now_iso();

    db_.insertTask(t);
    startSubscription(t);

    resp.ok = true;
    resp.task_id = t.id;
    resp.context_id = t.context;
    resp.state = t.state;
    resp.message = "task submitted";
    return resp;
}

CreateTaskResponse TaskManager::respondToTask(const std::string& task_id, const std::string& response_text) {
    CreateTaskResponse resp;
    auto existing = db_.getTask(task_id);
    if (!existing) {
        resp.ok = false;
        resp.error_kind = "unknown_task";
        resp.message = "no such task: " + task_id;
        return resp;
    }
    std::string endpoint;
    AuthSpec auth;
    if (!resolveEndpoint(existing->agent, &endpoint, &auth)) {
        resp.ok = false;
        resp.error_kind = "unknown_agent";
        resp.message = "unknown agent for task";
        return resp;
    }
    A2AResult r = a2a_->sendMessage(endpoint, auth, response_text, existing->id, existing->context);
    if (!r.ok) {
        resp.ok = false;
        resp.error_kind = error_kind_name(r.error_kind);
        resp.message = r.error;
        return resp;
    }
    Task t = existing ? *existing : Task{};
    if (r.task) {
        t = std::move(*r.task);
        t.title = existing->title;
        t.cwd = existing->cwd;
        t.created_at = existing->created_at;
        t.agent = existing->agent;
    }
    t.updated_at = now_iso();
    t.last_state_change = now_iso();
    db_.updateTask(t);
    emitStateChanged(t.id.value(), existing->state, t.state);
    resp.ok = true;
    resp.task_id = t.id;
    resp.context_id = t.context;
    resp.state = t.state;
    resp.message = "response sent";
    return resp;
}

A2AResult TaskManager::cancelTask(const std::string& task_id) {
    auto existing = db_.getTask(task_id);
    if (!existing) {
        A2AResult r; r.ok = false; r.error_kind = A2AResult::ErrorKind::ProtocolError;
        r.error = "no such task: " + task_id;
        return r;
    }
    std::string endpoint;
    AuthSpec auth;
    if (!resolveEndpoint(existing->agent, &endpoint, &auth)) {
        A2AResult r; r.ok = false; r.error_kind = A2AResult::ErrorKind::LocalNetwork;
        r.error = "unknown agent";
        return r;
    }
    A2AResult r = a2a_->cancelTask(endpoint, auth, existing->id);
    if (r.ok) {
        Task t = *existing;
        t.state = TaskState::Canceled;
        t.updated_at = now_iso();
        t.last_state_change = now_iso();
        db_.updateTask(t);
        emitStateChanged(t.id.value(), existing->state, t.state);
    }
    return r;
}

A2AResult TaskManager::refreshTask(const std::string& task_id) {
    auto existing = db_.getTask(task_id);
    if (!existing) {
        A2AResult r; r.ok = false; r.error_kind = A2AResult::ErrorKind::ProtocolError;
        r.error = "no such task: " + task_id;
        return r;
    }
    std::string endpoint;
    AuthSpec auth;
    if (!resolveEndpoint(existing->agent, &endpoint, &auth)) {
        A2AResult r; r.ok = false; r.error_kind = A2AResult::ErrorKind::LocalNetwork;
        r.error = "unknown agent";
        return r;
    }
    A2AResult r = a2a_->getTask(endpoint, auth, existing->id);
    if (r.ok && r.task) {
        Task t = std::move(*r.task);
        if (t.id.empty()) t.id = existing->id;
        t.title = existing->title;
        t.cwd = existing->cwd;
        t.created_at = existing->created_at;
        t.agent = existing->agent;
        t.updated_at = now_iso();
        db_.updateTask(t);
        emitStateChanged(t.id.value(), existing->state, t.state);
    }
    return r;
}

std::vector<TaskSummary> TaskManager::listTasks(bool include_terminal) {
    std::vector<TaskSummary> out;
    for (const Task& t : db_.listTasks(include_terminal)) {
        TaskSummary s;
        s.id = t.id;
        s.agent = t.agent;
        s.state = t.state;
        s.title = t.title;
        s.cwd = t.cwd;
        s.created_at = t.created_at;
        s.updated_at = t.updated_at;
        s.state_message = t.state_message;
        s.has_artifacts = !t.artifacts.empty();
        out.push_back(std::move(s));
    }
    return out;
}

A2AResult TaskManager::listRemoteTasks(const std::string& agent, const std::string& context_id,
                                       int page_size) {
    std::string endpoint; AuthSpec auth;
    if (!resolveEndpoint(AgentId(agent), &endpoint, &auth)) {
        A2AResult r; r.error_kind = A2AResult::ErrorKind::ProtocolError;
        r.error = "unknown agent: " + agent; return r;
    }
    A2AResult r = a2a_->listTasks(endpoint, auth, context_id, page_size);
    if (r.ok) for (auto& task : r.tasks) if (task.agent.empty()) task.agent = agent;
    return r;
}

bool TaskManager::materializeArtifact(const std::string& task_id, const std::string& artifact_id,
                                      size_t part_index, const std::string& output_dir,
                                      std::string* output_path, std::string* error) {
    auto task = db_.getTask(task_id);
    if (!task) { if (error) *error = "no such task: " + task_id; return false; }
    auto it = std::find_if(task->artifacts.begin(), task->artifacts.end(),
                           [&](const Artifact& a) { return a.artifact_id == artifact_id; });
    if (it == task->artifacts.end()) { if (error) *error = "no such artifact: " + artifact_id; return false; }
    if (part_index >= it->parts.size()) { if (error) *error = "artifact part index out of range"; return false; }
    const Part& part = it->parts[part_index];
    std::string bytes;
    if (!part.raw_b64.empty()) {
        auto decoded = decode_base64(part.raw_b64);
        if (!decoded) { if (error) *error = "invalid base64 artifact data"; return false; }
        bytes = std::move(*decoded);
    } else if (!part.text.empty()) bytes = part.text;
    else if (!part.data.empty()) bytes = part.data;
    else if (!part.url.empty()) {
        std::string endpoint; AuthSpec auth;
        if (!resolveEndpoint(task->agent, &endpoint, &auth)) { if (error) *error = "unknown task agent"; return false; }
        auto response = a2a_->download(part.url, auth);
        if (response.transport_error || response.status < 200 || response.status >= 300) {
            if (error) *error = response.transport_error ? response.transport_error_detail
                                                        : "artifact download returned HTTP " + std::to_string(response.status);
            return false;
        }
        bytes = std::move(response.body);
    } else { if (error) *error = "artifact part has no materializable content"; return false; }

    fs::path dir = output_dir.empty() ? fs::current_path() : fs::path(output_dir);
    std::error_code ec; fs::create_directories(dir, ec);
    if (ec) { if (error) *error = "cannot create output directory: " + ec.message(); return false; }
    fs::path path = dir / safe_filename(part.filename, *it, part_index);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out || !out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()))) {
        if (error) *error = "cannot write artifact: " + path.string();
        return false;
    }
    if (output_path) *output_path = fs::absolute(path).lexically_normal().string();
    return true;
}

std::optional<Task> TaskManager::getTask(const std::string& id) {
    return db_.getTask(id);
}

std::vector<Agent> TaskManager::listAgents() {
    std::vector<Agent> out;
    for (auto& [id, a] : agents_) out.push_back(a);
    std::sort(out.begin(), out.end(), [](const Agent& x, const Agent& y) { return x.id.value() < y.id.value(); });
    return out;
}

std::vector<std::string> TaskManager::listAgentIds() const {
    std::vector<std::string> ids;
    for (const auto& [id, a] : agents_) ids.push_back(id);
    std::sort(ids.begin(), ids.end());
    return ids;
}

Agent TaskManager::discoverAgent(const std::string& id) {
    auto it = agents_.find(id);
    if (it == agents_.end()) {
        Agent empty; empty.id = id;
        return empty;
    }
    Agent& a = it->second;
    AgentCard card = a2a_->discover(a.endpoint);
    a.card = card;
    a.available = card.valid;
    a.last_error = card.valid ? "" : card.parse_error;

    // Persist a compact card snapshot (only the interaction-relevant fields).
    nlohmann::json cj;
    cj["valid"] = card.valid;
    cj["name"] = card.name;
    cj["description"] = card.description;
    cj["version"] = card.version;
    cj["capabilities"] = {
        {"streaming", card.capabilities.streaming},
        {"pushNotifications", card.capabilities.push_notifications},
        {"extendedAgentCard", card.capabilities.extended_agent_card},
    };
    cj["interfaces"] = nlohmann::json::array();
    for (const auto& i : card.interfaces) {
        cj["interfaces"].push_back({
            {"url", i.url},
            {"protocolBinding", i.protocol_binding},
            {"protocolVersion", i.protocol_version},
        });
    }
    cj["auth_schemes"] = card.auth_schemes;
    db_.saveAgent(id, a.endpoint, a.host_label, cj.dump(), a.available, a.last_error);
    return a;
}

void TaskManager::discoverAllAgents() {
    std::vector<std::string> ids = listAgentIds();
    for (const auto& id : ids) discoverAgent(id);
}

void TaskManager::emitStateChanged(const std::string& task_id, TaskState old_state, TaskState new_state) {
    if (old_state == new_state) return;
    if (!event_sink_) return;
    event_sink_({
        {"type", "task.state_changed"},
        {"task_id", task_id},
        {"from", to_state_string(old_state)},
        {"to", to_state_string(new_state)},
    });
}

}  // namespace kitty_a2a
