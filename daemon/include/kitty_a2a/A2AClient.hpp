#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

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
    std::string message_text;          // when the agent replied with a Message (not a Task)
    std::string context_id;

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

    bool failed() const { return !ok; }
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

    // Send a message (creates a task or continues an existing one).
    // `task_id`/`context_id` may be empty for a fresh conversation.
    A2AResult sendMessage(const std::string& endpoint, const AuthSpec& auth,
                          const std::string& message_text,
                          const TaskId& task_id, const ContextId& context_id);

    // Poll current task state (GetTask).
    A2AResult getTask(const std::string& endpoint, const AuthSpec& auth, const TaskId& task_id);

    // Request cancellation (CancelTask).
    A2AResult cancelTask(const std::string& endpoint, const AuthSpec& auth, const TaskId& task_id);

    // Access the credential provider (used by the TaskManager to resolve auth
    // for an agent from its non-secret config reference).
    std::shared_ptr<CredentialProvider> credentials() const { return creds_; }

private:
    // Perform a JSON-RPC 2.0 call: POSTs to the endpoint, decodes result/error.
    A2AResult rpcCall(const std::string& endpoint, const AuthSpec& auth,
                      const std::string& method, const std::string& params_json);

    // Parse a Task object (from result.task) into our internal Task.
    static std::optional<Task> parseTask(const nlohmann::json& j);
    // Parse a Part / Message / Artifact from JSON (A2A 1.0 camelCase wire).
    static Part parsePart(const nlohmann::json& j);
    static Message parseMessage(const nlohmann::json& j);

    std::shared_ptr<HttpTransport> transport_;
    std::shared_ptr<CredentialProvider> creds_;
    Options opts_;
};

}  // namespace kitty_a2a
