#include "kitty_a2a/A2AClient.hpp"

#include <atomic>
#include <cctype>
#include <string>

namespace json = nlohmann;

namespace kitty_a2a {

namespace {
std::atomic<unsigned> id_counter{1};

// Split an endpoint URL into scheme://host and path for well-known URI
// construction. Returns (origin, rest). origin has no trailing slash.
std::pair<std::string, std::string> split_origin(const std::string& url) {
    auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return {url, ""};
    auto host_start = scheme_end + 3;
    auto path_start = url.find('/', host_start);
    if (path_start == std::string::npos) return {url, ""};
    std::string origin = url.substr(0, path_start);
    std::string rest = url.substr(path_start);
    return {origin, rest};
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

std::string role_internal(std::string_view wire) {
    if (wire == "ROLE_AGENT" || wire == "agent") return "agent";
    return "user";
}
}  // namespace

namespace {
// Convenience alias for the JSON value type used throughout the client.
using J = nlohmann::json;
}  // namespace

AgentCard A2AClient::discover(const std::string& endpoint) {
    AgentCard card;
    auto [origin, rest] = split_origin(endpoint);
    // Primary: origin + /.well-known/agent-card.json  (A2A 1.0 well-known URI).
    std::vector<std::string> candidates = {origin + "/.well-known/agent-card.json"};
    // Fallback: some servers serve the card at the endpoint base path.
    if (rest.empty() || rest == "/") candidates.push_back(origin);
    else candidates.push_back(endpoint);

    for (const auto& url : candidates) {
        HttpResponse r = transport_->request(url, "GET", "", {},
            {{"Accept", "application/json"}, {"User-Agent", opts_.user_agent}});
        if (r.transport_error) continue;
        if (r.status < 200 || r.status >= 300) continue;
        J j;
        try { j = J::parse(r.body); } catch (...) { continue; }
        if (j.is_object() && j.contains("name")) {
            card = AgentCard();
            card.valid = true;
            auto s = [&j](const char* k) { return j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : std::string(); };
            card.name = s("name");
            card.description = s("description");
            card.version = s("version");
            card.name = s("name");

            if (j.contains("capabilities") && j["capabilities"].is_object()) {
                const auto& c = j["capabilities"];
                card.capabilities.streaming = c.value("streaming", false);
                card.capabilities.push_notifications = c.value("pushNotifications", false);
                card.capabilities.extended_agent_card = c.value("extendedAgentCard", false);
            }
            if (j.contains("skills") && j["skills"].is_array()) {
                for (const auto& sk : j["skills"]) {
                    AgentSkill skill;
                    if (sk.is_object()) {
                        skill.id = sk.value("id", "");
                        skill.name = sk.value("name", "");
                        skill.description = sk.value("description", "");
                        if (sk.contains("tags") && sk["tags"].is_array())
                            for (const auto& t : sk["tags"]) if (t.is_string()) skill.tags.push_back(t.get<std::string>());
                    }
                    card.skills.push_back(std::move(skill));
                }
            }
            if (j.contains("supportedInterfaces") && j["supportedInterfaces"].is_array()) {
                for (const auto& it : j["supportedInterfaces"]) {
                    if (!it.is_object()) continue;
                    AgentInterface ai;
                    ai.url = it.value("url", "");
                    ai.protocol_binding = it.value("protocolBinding", "jsonrpc");
                    ai.protocol_version = it.value("protocolVersion", "");
                    card.interfaces.push_back(std::move(ai));
                }
            }
            if (j.contains("defaultInputModes") && j["defaultInputModes"].is_array())
                for (const auto& m : j["defaultInputModes"]) if (m.is_string()) card.input_modes.push_back(m.get<std::string>());
            if (j.contains("defaultOutputModes") && j["defaultOutputModes"].is_array())
                for (const auto& m : j["defaultOutputModes"]) if (m.is_string()) card.output_modes.push_back(m.get<std::string>());
            if (j.contains("securitySchemes") && j["securitySchemes"].is_object())
                for (auto it = j["securitySchemes"].begin(); it != j["securitySchemes"].end(); ++it)
                    card.auth_schemes.push_back(it.key());
            return card;
        }
    }
    card.valid = false;
    card.parse_error = "agent card not found at well-known URI or endpoint";
    return card;
}

Part A2AClient::parsePart(const nlohmann::json& j) {
    Part p;
    if (!j.is_object()) return p;
    if (j.contains("text") && j["text"].is_string()) p.text = j["text"].get<std::string>();
    if (j.contains("raw") && j["raw"].is_string()) p.raw_b64 = j["raw"].get<std::string>();
    if (j.contains("url") && j["url"].is_string()) p.url = j["url"].get<std::string>();
    if (j.contains("data") && j["data"].is_object()) p.data = j["data"].dump();
    p.media_type = j.value("mediaType", "");
    p.filename = j.value("filename", "");
    return p;
}

Message A2AClient::parseMessage(const nlohmann::json& j) {
    Message m;
    if (!j.is_object()) return m;
    m.message_id = j.value("messageId", "");
    m.role = role_internal(j.value("role", "ROLE_USER"));
    if (j.contains("taskId")) m.task_id = j["taskId"].get<std::string>();
    if (j.contains("contextId")) m.context_id = j["contextId"].get<std::string>();
    if (j.contains("referenceTaskIds") && j["referenceTaskIds"].is_array())
        for (const auto& t : j["referenceTaskIds"]) if (t.is_string()) m.reference_task_ids.push_back(t.get<std::string>());
    m.timestamp = j.value("timestamp", "");
    if (j.contains("parts") && j["parts"].is_array())
        for (const auto& part : j["parts"]) m.parts.push_back(parsePart(part));
    return m;
}

std::optional<Task> A2AClient::parseTask(const nlohmann::json& j) {
    if (!j.is_object() || !j.contains("id") || !j["id"].is_string()) return std::nullopt;
    Task t;
    t.id = j["id"].get<std::string>();
    if (j.contains("contextId")) t.context = j["contextId"].get<std::string>();
    if (j.contains("status") && j["status"].is_object()) {
        const auto& st = j["status"];
        auto state = parse_task_state_wire(st.value("state", ""));
        t.state = state.value_or(TaskState::Submitted);
        t.state_message = st.value("message", "");
        t.last_state_change = st.value("timestamp", "");
    }
    if (j.contains("history") && j["history"].is_array())
        for (const auto& mj : j["history"]) t.messages.push_back(parseMessage(mj));
    if (j.contains("artifacts") && j["artifacts"].is_array()) {
        for (const auto& aj : j["artifacts"]) {
            Artifact a;
            if (!aj.is_object()) continue;
            a.artifact_id = aj.value("artifactId", "");
            a.name = aj.value("name", "");
            a.description = aj.value("description", "");
            if (aj.contains("parts") && aj["parts"].is_array())
                for (const auto& part : aj["parts"]) a.parts.push_back(parsePart(part));
            t.artifacts.push_back(std::move(a));
        }
    }
    return t;
}

A2AResult A2AClient::rpcCall(const std::string& endpoint, const AuthSpec& auth,
                             const std::string& method, const std::string& params_json) {
    A2AResult out;
    J req;
    req["jsonrpc"] = "2.0";
    req["id"] = std::to_string(id_counter++);
    req["method"] = method;
    req["params"] = J::parse(params_json.empty() ? "{}" : params_json);

    std::vector<std::pair<std::string, std::string>> headers = {
        {"Content-Type", "application/json"},
        {"Accept", "application/json"},
        {"User-Agent", opts_.user_agent},
    };
    if (auth.active && !auth.header_value.empty()) {
        headers.emplace_back(auth.header_name, auth.header_value);
    }

    HttpResponse r = transport_->request(endpoint, "POST", "", req.dump(), headers);
    if (r.transport_error) {
        out.ok = false;
        out.error_kind = A2AResult::ErrorKind::LocalNetwork;
        out.error = "connection to " + endpoint + " failed: " + r.transport_error_detail;
        return out;
    }
    out.http_status = r.status;

    if (r.status == 401 || r.status == 403) {
        out.ok = false;
        out.error_kind = A2AResult::ErrorKind::AuthRequired;
        out.error = "authentication failed (HTTP " + std::to_string(r.status) + ") from " + endpoint;
        return out;
    }
    if (r.status < 200 || r.status >= 300) {
        out.ok = false;
        out.error_kind = A2AResult::ErrorKind::ProtocolError;
        out.error = "A2A server returned HTTP " + std::to_string(r.status) + " from " + endpoint;
        return out;
    }

    J j;
    try { j = J::parse(r.body); }
    catch (...) {
        out.ok = false;
        out.error_kind = A2AResult::ErrorKind::MalformedResponse;
        out.error = "could not parse A2A JSON-RPC response from " + endpoint;
        return out;
    }

    if (j.contains("error") && j["error"].is_object()) {
        out.ok = false;
        out.error_kind = A2AResult::ErrorKind::ProtocolError;
        const auto& e = j["error"];
        out.error = "A2A error " + std::to_string(e.value("code", 0)) + ": " + e.value("message", "unknown");
        return out;
    }

    if (!j.contains("result")) {
        out.ok = false;
        out.error_kind = A2AResult::ErrorKind::MalformedResponse;
        out.error = "A2A response missing 'result'";
        return out;
    }

    const auto& result = j["result"];
    // A result is a Task or a Message per A2A. Accept the standard direct
    // result as well as wrappers used by older/mixed-version agents.
    const J* task_result = nullptr;
    const J* message_result = nullptr;
    if (result.is_object()) {
        if (result.contains("task") && result["task"].is_object()) {
            task_result = &result["task"];
        } else if (result.contains("message") && result["message"].is_object()) {
            message_result = &result["message"];
        } else if (result.contains("id") && result["id"].is_string() &&
                   result.contains("status") && result["status"].is_object()) {
            task_result = &result;
        } else if (result.contains("messageId") && result["messageId"].is_string() &&
                   result.contains("role") && result["role"].is_string() &&
                   result.contains("parts") && result["parts"].is_array()) {
            message_result = &result;
        }
    }

    if (task_result) {
        auto t = parseTask(*task_result);
        if (!t) {
            out.ok = false;
            out.error_kind = A2AResult::ErrorKind::MalformedResponse;
            out.error = "malformed task in A2A result";
            return out;
        }
        out.ok = true;
        out.task = std::move(*t);
        out.context_id = out.task->context.value();
        // A remote FAILED/REJECTED is a task failure, not a protocol error, but
        // it is still not a success for the caller.
        if (out.task->state == TaskState::Failed || out.task->state == TaskState::Rejected) {
            out.ok = false;
            out.error_kind = A2AResult::ErrorKind::TaskFailure;
            out.error = "remote task ended in " + to_state_string(out.task->state);
        }
    } else if (message_result) {
        if (!message_result->contains("messageId") || !(*message_result)["messageId"].is_string() ||
            !message_result->contains("role") || !(*message_result)["role"].is_string() ||
            !message_result->contains("parts") || !(*message_result)["parts"].is_array()) {
            out.ok = false;
            out.error_kind = A2AResult::ErrorKind::MalformedResponse;
            out.error = "malformed message in A2A result";
            return out;
        }
        out.ok = true;
        Message m = parseMessage(*message_result);
        out.message_text = m.text();
        out.context_id = m.context_id.value();
    } else {
        out.ok = false;
        out.error_kind = A2AResult::ErrorKind::MalformedResponse;
        out.error = "A2A result is neither a Task nor a Message";
    }
    return out;
}

A2AResult A2AClient::sendMessage(const std::string& endpoint, const AuthSpec& auth,
                                 const std::string& message_text,
                                 const TaskId& task_id, const ContextId& context_id) {
    J msg;
    msg["role"] = "ROLE_USER";
    msg["messageId"] = "msg-" + std::to_string(id_counter++);
    J parts = J::array();
    J p;
    p["text"] = message_text;
    parts.push_back(p);
    msg["parts"] = parts;
    if (!context_id.empty()) msg["contextId"] = context_id.value();
    if (!task_id.empty()) msg["taskId"] = task_id.value();
    J params;
    params["message"] = msg;
    return rpcCall(endpoint, auth, "SendMessage", params.dump());
}

A2AResult A2AClient::getTask(const std::string& endpoint, const AuthSpec& auth, const TaskId& task_id) {
    J params;
    params["id"] = task_id.value();
    return rpcCall(endpoint, auth, "GetTask", params.dump());
}

A2AResult A2AClient::cancelTask(const std::string& endpoint, const AuthSpec& auth, const TaskId& task_id) {
    J params;
    params["id"] = task_id.value();
    return rpcCall(endpoint, auth, "CancelTask", params.dump());
}

}  // namespace kitty_a2a
