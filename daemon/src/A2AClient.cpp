#include "kitty_a2a/A2AClient.hpp"
#include "GrpcTransport.hpp"

#include <atomic>
#include <cctype>
#include <string>
#include <sstream>
#include <algorithm>
#include <random>
#include <iomanip>

namespace json = nlohmann;

namespace kitty_a2a {

namespace {
std::atomic<unsigned> id_counter{1};

std::string uuid_v4() {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<unsigned long long> dist;
    unsigned long long a = dist(gen), b = dist(gen);
    unsigned char bytes[16];
    for (int i = 0; i < 8; ++i) bytes[i] = static_cast<unsigned char>(a >> (i * 8));
    for (int i = 0; i < 8; ++i) bytes[8 + i] = static_cast<unsigned char>(b >> (i * 8));
    bytes[6] = (bytes[6] & 0x0f) | 0x40;
    bytes[8] = (bytes[8] & 0x3f) | 0x80;
    std::ostringstream out;
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out << '-';
        out << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(bytes[i]);
    }
    return out.str();
}

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

std::string role_internal(std::string_view wire) {
    if (wire == "ROLE_AGENT" || wire == "agent") return "agent";
    if (wire == "ROLE_USER" || wire == "user") return "user";
    return "unknown";
}

std::string url_encode(std::string_view value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out.push_back(static_cast<char>(c));
        else { out.push_back('%'); out.push_back(hex[c >> 4]); out.push_back(hex[c & 15]); }
    }
    return out;
}

void add_query(std::string& path, const std::string& key, const nlohmann::json& value) {
    if (value.is_null()) return;
    std::string text = value.is_string() ? value.get<std::string>() : value.dump();
    if (text.empty()) return;
    path += path.find('?') == std::string::npos ? '?' : '&';
    path += url_encode(key) + "=" + url_encode(text);
}
}  // namespace

namespace {
// Convenience alias for the JSON value type used throughout the client.
using J = nlohmann::json;
}  // namespace

void A2AClient::registerInterface(const AgentInterface& interface) {
    if (interface.url.empty()) return;
    std::lock_guard lock(bindings_mutex_);
    bindings_[interface.url] = {interface.protocol_binding.empty() ? "JSONRPC" : interface.protocol_binding,
                                interface.protocol_version.empty() ? "1.0" : interface.protocol_version,
                                interface.tenant};
}

AgentCard A2AClient::discover(const std::string& endpoint) {
    AgentCard card;
    auto [origin, rest] = split_origin(endpoint);
    // Primary: origin + /.well-known/agent-card.json  (A2A 1.0 well-known URI).
    std::vector<std::string> candidates = {origin + "/.well-known/agent-card.json"};
    // A reverse proxy may mount an agent below a path prefix. Probe the same
    // well-known path beneath that configured base before falling back to the
    // endpoint itself.
    if (!rest.empty() && rest != "/") {
        std::string scoped = endpoint;
        while (!scoped.empty() && scoped.back() == '/') scoped.pop_back();
        candidates.push_back(scoped + "/.well-known/agent-card.json");
        candidates.push_back(endpoint);
    } else {
        candidates.push_back(origin);
    }

    for (const auto& url : candidates) {
        CachedCard cached;
        { std::lock_guard lock(discovery_mutex_); auto it = card_cache_.find(url); if (it != card_cache_.end()) cached = it->second; }
        std::vector<std::pair<std::string, std::string>> headers = {{"Accept", "application/json"}, {"User-Agent", opts_.user_agent}};
        if (!cached.etag.empty()) headers.emplace_back("If-None-Match", cached.etag);
        HttpResponse r = transport_->request(url, "GET", "", {}, headers);
        if (r.status == 304 && !cached.body.empty()) { r.status = 200; r.body = cached.body; r.headers["etag"] = cached.etag; }
        if (r.transport_error) continue;
        if (r.status < 200 || r.status >= 300) continue;
        J j;
        try { j = J::parse(r.body); } catch (...) { continue; }
        if (j.is_object() && j.contains("name") && j["name"].is_string() && !j["name"].get<std::string>().empty()) {
            card = AgentCard();
            card.raw = j;
            auto s = [&j](const char* k) { return j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : std::string(); };
            card.description = s("description");
            card.version = s("version");
            card.name = s("name");

            if (j.contains("capabilities") && j["capabilities"].is_object()) {
                const auto& c = j["capabilities"];
                card.capabilities.streaming = c.value("streaming", false);
                card.capabilities.push_notifications = c.value("pushNotifications", false);
                card.capabilities.extended_agent_card = c.value("extendedAgentCard", false);
                if (c.contains("extensions") && c["extensions"].is_array()) {
                    for (const auto& ej : c["extensions"]) {
                        if (!ej.is_object()) continue;
                        AgentExtension extension;
                        extension.uri = ej.value("uri", ""); extension.description = ej.value("description", "");
                        extension.required = ej.value("required", false);
                        if (ej.contains("params") && ej["params"].is_object()) extension.params = ej["params"];
                        card.extensions.push_back(std::move(extension));
                    }
                }
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
                    ai.tenant = it.value("tenant", "");
                    card.interfaces.push_back(std::move(ai));
                }
            }
            for (const auto& interface : card.interfaces) registerInterface(interface);
            if (j.contains("defaultInputModes") && j["defaultInputModes"].is_array())
                for (const auto& m : j["defaultInputModes"]) if (m.is_string()) card.input_modes.push_back(m.get<std::string>());
            if (j.contains("defaultOutputModes") && j["defaultOutputModes"].is_array())
                for (const auto& m : j["defaultOutputModes"]) if (m.is_string()) card.output_modes.push_back(m.get<std::string>());
            if (j.contains("securitySchemes") && j["securitySchemes"].is_object()) {
                for (auto it = j["securitySchemes"].begin(); it != j["securitySchemes"].end(); ++it) {
                    card.auth_schemes.push_back(it.key());
                    SecurityScheme scheme; scheme.name = it.key();
                    if (it.value().is_object()) {
                        const auto& sj = it.value();
                        scheme.type = sj.value("type", "");
                        scheme.scheme = sj.value("scheme", "");
                        scheme.location = sj.value("in", sj.value("location", ""));
                        scheme.parameter = sj.value("name", "");
                        if (sj.contains("apiKeySecurityScheme")) {
                            const auto& v = sj["apiKeySecurityScheme"];
                            scheme.type = "apiKey"; scheme.location = v.value("location", ""); scheme.parameter = v.value("name", "");
                        } else if (sj.contains("httpAuthSecurityScheme")) {
                            const auto& v = sj["httpAuthSecurityScheme"];
                            scheme.type = "http"; scheme.scheme = v.value("scheme", "");
                        } else if (sj.contains("oauth2SecurityScheme")) scheme.type = "oauth2";
                        else if (sj.contains("openIdConnectSecurityScheme")) scheme.type = "openIdConnect";
                        else if (sj.contains("mtlsSecurityScheme")) scheme.type = "mutualTLS";
                    }
                    card.security_schemes[it.key()] = std::move(scheme);
                }
            }
            const char* requirements_key = j.contains("securityRequirements") ? "securityRequirements" :
                                           (j.contains("security") ? "security" : nullptr);
            if (requirements_key && j[requirements_key].is_array()) {
                for (const auto& rj : j[requirements_key]) {
                    if (!rj.is_object()) continue;
                    SecurityRequirement requirement;
                    const auto* source = &rj;
                    if (rj.contains("schemes") && rj["schemes"].is_object()) source = &rj["schemes"];
                    for (auto it = source->begin(); it != source->end(); ++it) {
                        std::vector<std::string> scopes;
                        const auto* values = &it.value();
                        if (it.value().is_object() && it.value().contains("list")) values = &it.value()["list"];
                        if (values->is_array()) for (const auto& v : *values) if (v.is_string()) scopes.push_back(v.get<std::string>());
                        requirement.schemes[it.key()] = std::move(scopes);
                    }
                    card.security_requirements.push_back(std::move(requirement));
                }
            }
            if (card.description.empty()) card.warnings.push_back("nonconforming Agent Card: required description is missing");
            if (card.version.empty()) card.warnings.push_back("nonconforming Agent Card: required version is missing");
            if (card.interfaces.empty()) card.warnings.push_back("nonconforming Agent Card: no supportedInterfaces were advertised; using configured endpoint");
            if (!j.contains("capabilities")) card.warnings.push_back("nonconforming Agent Card: required capabilities are missing");
            if (card.input_modes.empty()) card.warnings.push_back("nonconforming Agent Card: required defaultInputModes are missing");
            if (card.output_modes.empty()) card.warnings.push_back("nonconforming Agent Card: required defaultOutputModes are missing");
            if (card.skills.empty()) card.warnings.push_back("nonconforming Agent Card: required skills are missing");
            bool has_v1 = card.interfaces.empty();
            for (const auto& i : card.interfaces) {
                if (i.protocol_version.empty()) {
                    has_v1 = true;
                    card.warnings.push_back("nonconforming Agent Card: interface protocolVersion is missing; assuming 1.0");
                } else if (i.protocol_version == "1" || i.protocol_version.rfind("1.", 0) == 0) has_v1 = true;
            }
            if (!has_v1) card.warnings.push_back("Agent Card does not advertise an A2A v1.x interface");
            if (j.contains("signatures") && j["signatures"].is_array() && !j["signatures"].empty())
                card.warnings.push_back("Agent Card signatures were not verified because no trusted verification key is configured");
            for (const auto& extension : card.extensions) {
                if (extension.required) {
                    has_v1 = false;
                    card.parse_error = "Agent Card requires unsupported extension: " + extension.uri;
                    break;
                }
            }
            card.valid = has_v1;
            if (!card.valid && card.parse_error.empty()) card.parse_error = "Agent Card has no supported A2A v1.x interface";
            if (card.valid) {
                std::lock_guard lock(discovery_mutex_);
                card_cache_[url] = {r.body, r.headers.contains("etag") ? r.headers.at("etag") : ""};
            }
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
    if (j.contains("data") && !j["data"].is_null()) p.data = j["data"].dump();
    p.media_type = j.value("mediaType", "");
    p.filename = j.value("filename", "");
    if (j.contains("metadata") && j["metadata"].is_object()) p.metadata = j["metadata"];
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
    if (j.contains("metadata") && j["metadata"].is_object()) m.metadata = j["metadata"];
    if (j.contains("extensions") && j["extensions"].is_array())
        for (const auto& extension : j["extensions"]) if (extension.is_string()) m.extensions.push_back(extension.get<std::string>());
    if (j.contains("parts") && j["parts"].is_array())
        for (const auto& part : j["parts"]) m.parts.push_back(parsePart(part));
    return m;
}

std::optional<Task> A2AClient::parseTask(const nlohmann::json& j) {
    if (!j.is_object() || !j.contains("id") || !j["id"].is_string()) return std::nullopt;
    Task t;
    t.id = j["id"].get<std::string>();
    t.remote_task_id = t.id;
    if (j.contains("contextId")) t.context = j["contextId"].get<std::string>();
    if (j.contains("status") && j["status"].is_object()) {
        const auto& st = j["status"];
        auto state = parse_task_state_wire(st.value("state", ""));
        t.state = state.value_or(TaskState::Unknown);
        // A2A 1.0 defines status.message as a structured Message.  Retain
        // support for older agents that sent a plain string here.
        if (st.contains("message") && st["message"].is_object()) {
            Message message = parseMessage(st["message"]);
            t.state_message = message.text();
            t.messages.push_back(std::move(message));
        } else if (st.contains("message") && st["message"].is_string()) {
            t.state_message = st["message"].get<std::string>();
        }
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
            if (aj.contains("metadata") && aj["metadata"].is_object()) a.metadata = aj["metadata"];
            if (aj.contains("extensions") && aj["extensions"].is_array())
                for (const auto& e : aj["extensions"]) if (e.is_string()) a.extensions.push_back(e.get<std::string>());
            t.artifacts.push_back(std::move(a));
        }
    }
    if (j.contains("metadata") && j["metadata"].is_object()) t.metadata = j["metadata"];
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

    BindingInfo info;
    {
        std::lock_guard lock(bindings_mutex_);
        if (auto it = bindings_.find(endpoint); it != bindings_.end()) info = it->second;
    }
    if (!info.tenant.empty() && !req["params"].contains("tenant")) req["params"]["tenant"] = info.tenant;
    std::string binding = info.binding;
    std::transform(binding.begin(), binding.end(), binding.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    const bool rest = binding == "HTTP+JSON" || binding == "REST" || binding == "HTTP_JSON";
    const bool grpc = binding == "GRPC";
    std::vector<std::pair<std::string, std::string>> headers = {
        {"Content-Type", "application/json"},
        {"Accept", "application/json"},
        {"User-Agent", opts_.user_agent},
        {"A2A-Version", info.version},
    };
    if (auth.active && !auth.header_value.empty()) {
        headers.emplace_back(auth.header_name, auth.header_value);
    }
    headers.insert(headers.end(), auth.extra_headers.begin(), auth.extra_headers.end());
    if (!auth.client_cert_file.empty()) headers.emplace_back("X-A2AD-Client-Cert-File", auth.client_cert_file);
    if (!auth.client_key_file.empty()) headers.emplace_back("X-A2AD-Client-Key-File", auth.client_key_file);

    HttpResponse r;
    if (grpc) {
        r = grpcCall(endpoint, auth, method, req["params"], info.version);
        if (!r.transport_error && r.status >= 200 && r.status < 300) {
            try { r.body = nlohmann::json({{"jsonrpc", "2.0"}, {"result", r.body.empty() ? nlohmann::json::object() : nlohmann::json::parse(r.body)}}).dump(); }
            catch (...) { /* normal malformed-response handling below */ }
        }
    } else if (rest) {
        headers[0].second = "application/a2a+json";
        headers[1].second = "application/a2a+json";
        std::string verb = "POST", path, body = req["params"].dump();
        const auto& params = req["params"];
        std::string prefix = info.tenant.empty() ? "" : url_encode(info.tenant) + "/";
        if (method == "SendMessage") path = prefix + "message:send";
        else if (method == "GetTask") {
            verb = "GET"; path = prefix + "tasks/" + url_encode(params.value("id", "")); body.clear();
            if (params.contains("historyLength")) add_query(path, "historyLength", params["historyLength"]);
        }
        else if (method == "ListTasks") {
            verb = "GET"; path = prefix + "tasks"; body.clear();
            for (const char* key : {"contextId", "status", "pageSize", "pageToken", "historyLength", "statusTimestampAfter", "includeArtifacts"})
                if (params.contains(key)) add_query(path, key, params[key]);
        } else if (method == "CancelTask") path = prefix + "tasks/" + url_encode(params.value("id", "")) + ":cancel";
        else if (method == "CreateTaskPushNotificationConfig")
            path = prefix + "tasks/" + url_encode(params.value("taskId", "")) + "/pushNotificationConfigs";
        else if (method == "GetTaskPushNotificationConfig") {
            verb = "GET"; body.clear(); path = prefix + "tasks/" + url_encode(params.value("taskId", "")) +
                "/pushNotificationConfigs/" + url_encode(params.value("id", ""));
        } else if (method == "ListTaskPushNotificationConfigs") {
            verb = "GET"; body.clear(); path = prefix + "tasks/" + url_encode(params.value("taskId", "")) + "/pushNotificationConfigs";
            if (params.contains("pageSize")) add_query(path, "pageSize", params["pageSize"]);
            if (params.contains("pageToken")) add_query(path, "pageToken", params["pageToken"]);
        } else if (method == "DeleteTaskPushNotificationConfig") {
            verb = "DELETE"; body.clear(); path = prefix + "tasks/" + url_encode(params.value("taskId", "")) +
                "/pushNotificationConfigs/" + url_encode(params.value("id", ""));
        } else if (method == "GetExtendedAgentCard") { verb = "GET"; body.clear(); path = prefix + "extendedAgentCard"; }
        else {
            A2AResult unsupported; unsupported.error_kind = A2AResult::ErrorKind::ProtocolError;
            unsupported.error = "operation is not available through HTTP+JSON: " + method;
            return unsupported;
        }
        if (!auth.query_name.empty()) add_query(path, auth.query_name, auth.query_value);
        for (const auto& [name, value] : auth.extra_query) add_query(path, name, value);
        r = transport_->request(endpoint, verb, path, body, headers);
        if (!r.transport_error && r.status >= 200 && r.status < 300) {
            try { r.body = nlohmann::json({{"jsonrpc", "2.0"}, {"result", r.body.empty() ? nlohmann::json::object() : nlohmann::json::parse(r.body)}}).dump(); }
            catch (...) { /* normal malformed-response handling below */ }
        }
    } else {
        std::string target = endpoint;
        if (!auth.query_name.empty()) add_query(target, auth.query_name, auth.query_value);
        for (const auto& [name, value] : auth.extra_query) add_query(target, name, value);
        r = transport_->request(target, "POST", "", req.dump(), headers);
    }
    if (r.response_too_large) {
        out.ok = false; out.error_kind = A2AResult::ErrorKind::MalformedResponse;
        out.error = "A2A response exceeds the 64 MiB limit";
        return out;
    }
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

    if (!rest && !grpc && (j.value("jsonrpc", "") != "2.0" || !j.contains("id") || j["id"] != req["id"])) {
        out.error_kind = A2AResult::ErrorKind::MalformedResponse;
        out.error = "A2A JSON-RPC response has an invalid version or mismatched id";
        return out;
    }
    if (j.contains("error") && j["error"].is_object()) {
        out.ok = false;
        out.error_kind = A2AResult::ErrorKind::ProtocolError;
        const auto& e = j["error"];
        out.protocol_code = e.value("code", 0);
        if (e.contains("data")) out.error_details = e["data"];
        out.error = "A2A error " + std::to_string(out.protocol_code) + ": " + e.value("message", "unknown");
        return out;
    }

    if (!j.contains("result")) {
        out.ok = false;
        out.error_kind = A2AResult::ErrorKind::MalformedResponse;
        out.error = "A2A response missing 'result'";
        return out;
    }

    const auto& result = j["result"];
    out.value = result;
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

    const J* task_list = nullptr;
    if (result.is_array()) task_list = &result;
    else if (result.is_object() && result.contains("tasks") && result["tasks"].is_array())
        task_list = &result["tasks"];

    if (task_list) {
        for (const auto& item : *task_list) {
            if (auto task = parseTask(item)) out.tasks.push_back(std::move(*task));
        }
        out.ok = true;
        if (result.is_object()) {
            out.next_page_token = result.value("nextPageToken", "");
            out.page_size = result.value("pageSize", 0);
            out.total_size = result.value("totalSize", 0);
        }
    } else if (task_result) {
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
        // FAILED/REJECTED are valid protocol results and must be persisted.
        if (out.task->state == TaskState::Failed || out.task->state == TaskState::Rejected) {
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
        out.message = std::move(m);
    } else if (method == "DeleteTaskPushNotificationConfig" ||
               method == "CreateTaskPushNotificationConfig" ||
               method == "GetTaskPushNotificationConfig" ||
               method == "ListTaskPushNotificationConfigs" || method == "GetExtendedAgentCard") {
        out.ok = true;
    } else {
        out.ok = false;
        out.error_kind = A2AResult::ErrorKind::MalformedResponse;
        out.error = "A2A result is neither a Task nor a Message";
    }
    return out;
}

A2AResult A2AClient::sendMessage(const std::string& endpoint, const AuthSpec& auth,
                                 const std::string& message_text,
                                 const TaskId& task_id, const ContextId& context_id,
                                 const std::string& message_id) {
    J msg;
    msg["role"] = "ROLE_USER";
    msg["messageId"] = message_id.empty() ? uuid_v4() : message_id;
    J parts = J::array();
    J p;
    p["text"] = message_text;
    parts.push_back(p);
    msg["parts"] = parts;
    if (!context_id.empty()) msg["contextId"] = context_id.value();
    if (!task_id.empty()) msg["taskId"] = task_id.value();
    J params;
    params["message"] = msg;
    params["configuration"] = {{"returnImmediately", true}};
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

A2AResult A2AClient::listTasks(const std::string& endpoint, const AuthSpec& auth,
                               const std::string& context_id, int page_size) {
    ListTasksFilter filter; filter.context_id = context_id; filter.page_size = page_size;
    return listTasks(endpoint, auth, filter);
}

A2AResult A2AClient::listTasks(const std::string& endpoint, const AuthSpec& auth,
                               const ListTasksFilter& filter) {
    J params;
    if (!filter.context_id.empty()) params["contextId"] = filter.context_id;
    if (filter.status) params["status"] = task_state_wire_name(*filter.status);
    if (filter.page_size > 0) params["pageSize"] = std::clamp(filter.page_size, 1, 100);
    if (!filter.page_token.empty()) params["pageToken"] = filter.page_token;
    if (filter.history_length) params["historyLength"] = *filter.history_length;
    if (!filter.status_timestamp_after.empty()) params["statusTimestampAfter"] = filter.status_timestamp_after;
    if (filter.include_artifacts) params["includeArtifacts"] = *filter.include_artifacts;
    return rpcCall(endpoint, auth, "ListTasks", params.dump());
}

A2AResult A2AClient::createPushConfig(const std::string& endpoint, const AuthSpec& auth,
                                      const PushNotificationConfig& config) {
    J params = {{"id", config.id}, {"taskId", config.task_id}, {"url", config.url}, {"token", config.token}};
    if (!config.auth_scheme.empty()) params["authentication"] = {{"scheme", config.auth_scheme}, {"credentials", config.auth_credentials}};
    return rpcCall(endpoint, auth, "CreateTaskPushNotificationConfig", params.dump());
}

A2AResult A2AClient::getPushConfig(const std::string& endpoint, const AuthSpec& auth,
                                   const std::string& task_id, const std::string& id) {
    return rpcCall(endpoint, auth, "GetTaskPushNotificationConfig", J{{"taskId", task_id}, {"id", id}}.dump());
}

A2AResult A2AClient::listPushConfigs(const std::string& endpoint, const AuthSpec& auth,
                                    const std::string& task_id, int page_size, const std::string& page_token) {
    J params = {{"taskId", task_id}, {"pageSize", page_size}};
    if (!page_token.empty()) params["pageToken"] = page_token;
    return rpcCall(endpoint, auth, "ListTaskPushNotificationConfigs", params.dump());
}

A2AResult A2AClient::deletePushConfig(const std::string& endpoint, const AuthSpec& auth,
                                      const std::string& task_id, const std::string& id) {
    return rpcCall(endpoint, auth, "DeleteTaskPushNotificationConfig", J{{"taskId", task_id}, {"id", id}}.dump());
}

A2AResult A2AClient::getExtendedAgentCard(const std::string& endpoint, const AuthSpec& auth) {
    return rpcCall(endpoint, auth, "GetExtendedAgentCard", "{}");
}

HttpResponse A2AClient::download(const std::string& url, const AuthSpec& auth) {
    std::vector<std::pair<std::string, std::string>> headers = {{"Accept", "*/*"},
                                                                {"User-Agent", opts_.user_agent}};
    if (auth.active && !auth.header_value.empty()) headers.emplace_back(auth.header_name, auth.header_value);
    headers.insert(headers.end(), auth.extra_headers.begin(), auth.extra_headers.end());
    std::string target = url;
    if (!auth.query_name.empty()) add_query(target, auth.query_name, auth.query_value);
    for (const auto& [name, value] : auth.extra_query) add_query(target, name, value);
    if (!auth.client_cert_file.empty()) headers.emplace_back("X-A2AD-Client-Cert-File", auth.client_cert_file);
    if (!auth.client_key_file.empty()) headers.emplace_back("X-A2AD-Client-Key-File", auth.client_key_file);
    return transport_->request(target, "GET", "", "", headers);
}

A2AResult A2AClient::streamCall(const std::string& endpoint, const AuthSpec& auth,
                                const std::string& method, J params,
                                const std::function<void(const J&)>& on_event,
                                std::stop_token stop) {
    BindingInfo info;
    { std::lock_guard lock(bindings_mutex_); if (auto it = bindings_.find(endpoint); it != bindings_.end()) info = it->second; }
    if (!info.tenant.empty()) params["tenant"] = info.tenant;
    J request = {{"jsonrpc", "2.0"}, {"id", std::to_string(id_counter++)}, {"method", method}, {"params", params}};
    std::string binding = info.binding;
    std::transform(binding.begin(), binding.end(), binding.begin(), [](unsigned char c) { return std::toupper(c); });
    const bool rest = binding == "HTTP+JSON" || binding == "REST" || binding == "HTTP_JSON";
    const bool grpc = binding == "GRPC";
    std::vector<std::pair<std::string, std::string>> headers = {
        {"Content-Type", "application/json"}, {"Accept", "text/event-stream"},
        {"Cache-Control", "no-cache"}, {"User-Agent", opts_.user_agent}, {"A2A-Version", info.version}};
    if (auth.active && !auth.header_value.empty()) headers.emplace_back(auth.header_name, auth.header_value);
    headers.insert(headers.end(), auth.extra_headers.begin(), auth.extra_headers.end());
    if (!auth.client_cert_file.empty()) headers.emplace_back("X-A2AD-Client-Cert-File", auth.client_cert_file);
    if (!auth.client_key_file.empty()) headers.emplace_back("X-A2AD-Client-Key-File", auth.client_key_file);

    std::string pending;
    auto consume = [&](std::string_view chunk) {
        if (stop.stop_requested()) return false;
        pending.append(chunk);
        pending.erase(std::remove(pending.begin(), pending.end(), '\r'), pending.end());
        size_t boundary;
        while ((boundary = pending.find("\n\n")) != std::string::npos) {
            std::string event = pending.substr(0, boundary);
            pending.erase(0, boundary + 2);
            std::string data;
            std::istringstream lines(event);
            for (std::string line; std::getline(lines, line);) {
                if (line.rfind("data:", 0) == 0) {
                    if (!data.empty()) data += '\n';
                    size_t start = line.size() > 5 && line[5] == ' ' ? 6 : 5;
                    data += line.substr(start);
                }
            }
            if (data.empty() || data == "[DONE]") continue;
            try {
                auto json = nlohmann::json::parse(data);
                const auto* value = &json;
                if (json.contains("result")) value = &json["result"];
                on_event(*value);
            } catch (...) { /* malformed events do not terminate a healthy stream */ }
        }
        return !stop.stop_requested();
    };
    if (grpc) {
        auto response = grpcStream(endpoint, auth, method, params, [&](const J& event) { on_event(event); return !stop.stop_requested(); }, stop, info.version);
        A2AResult result; result.http_status = response.status;
        if (stop.stop_requested()) { result.ok = true; return result; }
        if (response.transport_error) {
            result.error_kind = A2AResult::ErrorKind::LocalNetwork;
            result.error = "gRPC stream failed: " + response.transport_error_detail;
        } else if (response.status < 200 || response.status >= 300) {
            result.error_kind = A2AResult::ErrorKind::ProtocolError;
            result.error = method + " gRPC failed: " + response.body;
        } else result.ok = true;
        return result;
    }
    if (rest) {
        headers[0].second = "application/a2a+json";
    }
    const std::string prefix = info.tenant.empty() ? "" : url_encode(info.tenant) + "/";
    const bool subscribe = method == "SubscribeToTask";
    std::string rest_path = subscribe ? prefix + "tasks/" + url_encode(params.value("id", "")) + ":subscribe"
                                      : prefix + "message:stream";
    std::string target = endpoint;
    if (!auth.query_name.empty()) {
        if (rest) add_query(rest_path, auth.query_name, auth.query_value);
        else add_query(target, auth.query_name, auth.query_value);
    }
    for (const auto& [name, value] : auth.extra_query) {
        if (rest) add_query(rest_path, name, value); else add_query(target, name, value);
    }
    auto response = transport_->stream(target, rest && subscribe ? "GET" : "POST",
                                       rest ? rest_path : "",
                                       rest ? (subscribe ? "" : params.dump()) : request.dump(), headers, consume);
    A2AResult result; result.http_status = response.status;
    if (stop.stop_requested()) { result.ok = true; return result; }
    if (response.transport_error) {
        result.error_kind = A2AResult::ErrorKind::LocalNetwork;
        result.error = "stream failed: " + response.transport_error_detail;
    } else if (response.status < 200 || response.status >= 300) {
        result.error_kind = A2AResult::ErrorKind::ProtocolError;
        result.error = method + " returned HTTP " + std::to_string(response.status);
    } else result.ok = true;
    return result;
}

A2AResult A2AClient::subscribeToTask(const std::string& endpoint, const AuthSpec& auth,
                                     const TaskId& task_id,
                                     const std::function<void(const Task&)>& on_task,
                                     std::stop_token stop) {
    std::stop_source stream_stop;
    std::stop_callback external_stop(stop, [&stream_stop] { stream_stop.request_stop(); });
    return streamCall(endpoint, auth, "SubscribeToTask", {{"id", task_id.value()}},
        [&](const J& event) {
            const J* value = &event;
            if (event.contains("task")) value = &event["task"];
            if (auto task = parseTask(*value)) {
                on_task(*task);
                if (is_terminal(task->state)) stream_stop.request_stop();
            }
            else if (event.contains("statusUpdate") && event["statusUpdate"].is_object()) {
                const auto& update = event["statusUpdate"];
                J task_json = {{"id", update.value("taskId", task_id.value())},
                               {"contextId", update.value("contextId", "")},
                               {"status", update.value("status", J::object())}};
                if (auto task = parseTask(task_json)) {
                    task->partial_update = true;
                    if (update.contains("metadata")) task->metadata = update["metadata"];
                    on_task(*task);
                    if (is_terminal(task->state)) stream_stop.request_stop();
                }
            } else if (event.contains("artifactUpdate") && event["artifactUpdate"].is_object()) {
                const auto& update = event["artifactUpdate"];
                J task_json = {{"id", update.value("taskId", task_id.value())},
                               {"contextId", update.value("contextId", "")},
                               {"artifacts", J::array({update.value("artifact", J::object())})}};
                if (auto task = parseTask(task_json)) {
                    task->partial_update = true; task->has_state_update = false;
                    task->artifact_append = update.value("append", false);
                    if (update.contains("metadata")) task->metadata = update["metadata"];
                    on_task(*task);
                }
            } else if (event.contains("message") && event["message"].is_object()) {
                Message message = parseMessage(event["message"]);
                if (!message.task_id.empty()) {
                    Task task; task.id = message.task_id; task.remote_task_id = message.task_id;
                    task.context = message.context_id; task.partial_update = true;
                    task.has_state_update = false; task.messages.push_back(std::move(message));
                    on_task(task);
                }
            }
        }, stream_stop.get_token());
}

A2AResult A2AClient::sendStreamingMessage(const std::string& endpoint, const AuthSpec& auth,
                                           const std::string& message_text, const TaskId& task_id,
                                           const ContextId& context_id,
                                           const std::function<void(const J&)>& on_event,
                                           std::stop_token stop) {
    J message = {{"role", "ROLE_USER"}, {"messageId", uuid_v4()}, {"parts", J::array({{{"text", message_text}}})}};
    if (!task_id.empty()) message["taskId"] = task_id.value();
    if (!context_id.empty()) message["contextId"] = context_id.value();
    return streamCall(endpoint, auth, "SendStreamingMessage", {{"message", message}}, on_event, stop);
}

}  // namespace kitty_a2a
