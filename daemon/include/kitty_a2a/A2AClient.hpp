#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <stop_token>
#include <map>
#include <mutex>

#include <nlohmann/json.hpp>

#include "kitty_a2a/AgentCard.hpp"
#include "kitty_a2a/Credentials.hpp"
#include "kitty_a2a/Task.hpp"
#include "kitty_a2a/types.hpp"

namespace kitty_a2a {

// Result of an A2A call. Exactly one of `task` / `message` / `error` is set on
// success (per A2A, a SendMessage returns a Task or a Message); on failure
// `error` is set and carries a classified error (DESIGN.md §27: we must
// distinguish local, protocol, task, and auth failures — never silently map a
// network error to task completion).
struct A2AResult {
    bool ok = false;
    std::optional<Task> task;
    std::vector<Task> tasks;            // ListTasks result
    std::string message_text;          // when the agent replied with a Message (not a Task)
    std::string context_id;
    std::optional<Message> message;
    std::string next_page_token;
    int page_size = 0;
    int total_size = 0;
    nlohmann::json value;

    // Error classification (empty on success).
    enum class ErrorKind {
        None = 0,
        LocalNetwork,      // couldn't reach endpoint / TLS / connection reset
        AuthRequired,      // 401/403 or card demanded auth we couldn't satisfy
        ProtocolError,     // A2A JSON-RPC error (TaskNotFound, VersionNotSupported, ...)
        TaskFailure,       // remote task itself failed (state == FAILED / REJECTED)
        MalformedResponse, // couldn't parse the server response
    };
    ErrorKind error_kind = ErrorKind::None;
    std::string error;                 // human-readable detail
    int http_status = 0;               // transport status, if any
    int protocol_code = 0;
    nlohmann::json error_details;

    bool failed() const { return !ok; }
};

struct ListTasksFilter {
    std::string context_id;
    std::optional<TaskState> status;
    int page_size = 100;
    std::string page_token;
    std::optional<int> history_length;
    std::string status_timestamp_after;
    std::optional<bool> include_artifacts;
};

struct PushNotificationConfig {
    std::string id;
    std::string task_id;
    std::string url;
    std::string token;
    std::string auth_scheme;
    std::string auth_credentials;
};

// A minimal HTTP transport seam so the protocol logic is unit-testable against
// a mock and so gRPC/REST bindings can be added later (DESIGN.md §22, §33).
// A concrete impl performs the actual request and returns status + body.
struct HttpResponse {
    int status = 0;
    std::string body;
    bool transport_error = false;   // connection failed / TLS error, etc.
    std::string transport_error_detail;
};
class HttpTransport {
public:
    virtual ~HttpTransport() = default;
    // Perform one request. `endpoint` is the A2A service URL; `method` POST/GET;
    // `path` is appended to endpoint (may be empty for POST to endpoint itself).
    // `headers` are extra headers (auth, accept). Returns the raw response.
    virtual HttpResponse request(const std::string& endpoint, const std::string& method,
                                 const std::string& path, const std::string& body,
                                 const std::vector<std::pair<std::string, std::string>>& headers) = 0;

    // Stream response chunks. The default preserves compatibility with simple
    // transports/mocks by delivering a buffered response once.
    virtual HttpResponse stream(const std::string& endpoint, const std::string& method,
                                const std::string& path, const std::string& body,
                                const std::vector<std::pair<std::string, std::string>>& headers,
                                const std::function<bool(std::string_view)>& on_chunk) {
        auto response = request(endpoint, method, path, body, headers);
        if (!response.body.empty()) on_chunk(response.body);
        return response;
    }
};

// Isolated A2A 1.0 client over the JSON-RPC 2.0 binding (DESIGN.md §22).
// This is the ONLY place in the daemon that knows about A2A wire details.
class A2AClient {
public:
    struct Options {
        int timeout_ms = 30000;     // connect+read timeout
        std::string user_agent = "a2ad/0.1";
    };

    A2AClient(std::shared_ptr<HttpTransport> transport, std::shared_ptr<CredentialProvider> creds)
        : transport_(std::move(transport)), creds_(std::move(creds)) {}
    A2AClient(std::shared_ptr<HttpTransport> transport, std::shared_ptr<CredentialProvider> creds,
              Options options)
        : transport_(std::move(transport)), creds_(std::move(creds)), opts_(options) {}

    // Discover the Agent Card. Tries the well-known URI relative to the
    // endpoint's origin, then the endpoint itself as a fallback (DESIGN.md §7).
    AgentCard discover(const std::string& endpoint);

    // Register the selected advertised interface. Calls to its URL then carry
    // the advertised binding, v1 minor version and tenant on every operation.
    void registerInterface(const AgentInterface& interface);

    // Send a message (creates a task or continues an existing one).
    // `task_id`/`context_id` may be empty for a fresh conversation.
    A2AResult sendMessage(const std::string& endpoint, const AuthSpec& auth,
                          const std::string& message_text,
                          const TaskId& task_id, const ContextId& context_id);
    A2AResult sendStreamingMessage(const std::string& endpoint, const AuthSpec& auth,
                          const std::string& message_text, const TaskId& task_id,
                          const ContextId& context_id,
                          const std::function<void(const nlohmann::json&)>& on_event,
                          std::stop_token stop = {});

    // Poll current task state (GetTask).
    A2AResult getTask(const std::string& endpoint, const AuthSpec& auth, const TaskId& task_id);

    // Request cancellation (CancelTask).
    A2AResult cancelTask(const std::string& endpoint, const AuthSpec& auth, const TaskId& task_id);

    // List tasks known by the remote agent (as opposed to the daemon's local
    // persisted registry). Optional context/state filters follow A2A v1.0.
    A2AResult listTasks(const std::string& endpoint, const AuthSpec& auth,
                        const std::string& context_id = {}, int page_size = 100);
    A2AResult listTasks(const std::string& endpoint, const AuthSpec& auth,
                        const ListTasksFilter& filter);

    A2AResult createPushConfig(const std::string& endpoint, const AuthSpec& auth,
                               const PushNotificationConfig& config);
    A2AResult getPushConfig(const std::string& endpoint, const AuthSpec& auth,
                            const std::string& task_id, const std::string& id);
    A2AResult listPushConfigs(const std::string& endpoint, const AuthSpec& auth,
                             const std::string& task_id, int page_size = 50,
                             const std::string& page_token = {});
    A2AResult deletePushConfig(const std::string& endpoint, const AuthSpec& auth,
                               const std::string& task_id, const std::string& id);
    A2AResult getExtendedAgentCard(const std::string& endpoint, const AuthSpec& auth);

    // Fetch an artifact URL with the same credential policy as A2A calls.
    HttpResponse download(const std::string& url, const AuthSpec& auth);

    // Blocking SSE subscription. Returns when the stream closes or stop is
    // requested; each valid Task update is delivered immediately.
    A2AResult subscribeToTask(const std::string& endpoint, const AuthSpec& auth,
                              const TaskId& task_id, const std::function<void(const Task&)>& on_task,
                              std::stop_token stop = {});

    // Access the credential provider (used by the TaskManager to resolve auth
    // for an agent from its non-secret config reference).
    std::shared_ptr<CredentialProvider> credentials() const { return creds_; }

private:
    // Perform a JSON-RPC 2.0 call: POSTs to the endpoint, decodes result/error.
    A2AResult rpcCall(const std::string& endpoint, const AuthSpec& auth,
                      const std::string& method, const std::string& params_json);
    A2AResult streamCall(const std::string& endpoint, const AuthSpec& auth,
                         const std::string& method, nlohmann::json params,
                         const std::function<void(const nlohmann::json&)>& on_event,
                         std::stop_token stop);

    // Parse a direct or compatibility-wrapped Task result into our internal Task.
    static std::optional<Task> parseTask(const nlohmann::json& j);
    // Parse a Part / Message / Artifact from JSON (A2A 1.0 camelCase wire).
    static Part parsePart(const nlohmann::json& j);
    static Message parseMessage(const nlohmann::json& j);

    std::shared_ptr<HttpTransport> transport_;
    std::shared_ptr<CredentialProvider> creds_;
    Options opts_;
    std::mutex bindings_mutex_;
    struct BindingInfo { std::string binding = "JSONRPC"; std::string version = "1.0"; std::string tenant; };
    std::map<std::string, BindingInfo> bindings_; // interface URL -> selected wire information
};

}  // namespace kitty_a2a
