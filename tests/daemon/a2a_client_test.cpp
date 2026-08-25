#include "test_framework.hpp"
#include "kitty_a2a/A2AClient.hpp"
#include "kitty_a2a/Credentials.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

using namespace kitty_a2a;

// A scriptable mock transport. Each call returns the next queued response, or
// the default if the queue is empty. Records the last request for assertions.
class MockTransport : public HttpTransport {
public:
    std::vector<HttpResponse> queue;
    HttpResponse fallback{200, "{}", false, ""};
    std::string last_url;
    std::string last_method;
    std::string last_body;
    std::vector<std::pair<std::string, std::string>> last_headers;
    int calls = 0;

    HttpResponse request(const std::string& endpoint, const std::string& method,
                         const std::string& path, const std::string& body,
                         const std::vector<std::pair<std::string, std::string>>& headers) override {
        ++calls;
        last_url = endpoint + (path.empty() ? "" : "/" + path);
        last_method = method;
        last_body = body;
        last_headers = headers;
        if (!queue.empty()) { auto r = queue.front(); queue.erase(queue.begin()); return r; }
        return fallback;
    }
};

ADD_TEST(parse_task_from_wire) {
    auto mt = std::make_shared<MockTransport>();
    auto creds = make_credential_provider();
    auto creds_shared = std::shared_ptr<CredentialProvider>(std::move(creds));
    A2AClient client(mt, creds_shared);

    HttpResponse ok{200, R"({
      "jsonrpc":"2.0","id":"1","result":{
        "task":{
          "id":"task-1","contextId":"ctx-1",
          "status":{"state":"TASK_STATE_WORKING","message":"running","timestamp":"2026-08-24T00:00:00Z"},
          "history":[
            {"messageId":"m1","role":"ROLE_USER","parts":[{"text":"hi"}]},
            {"messageId":"m2","role":"ROLE_AGENT","parts":[{"text":"on it"}]}
          ],
          "artifacts":[
            {"artifactId":"a1","name":"out.patch","description":"the patch",
             "parts":[{"text":"diff --git","filename":"out.patch","mediaType":"text/x-diff"}]}
          ]
        }
      }
    })", false, ""};
    mt->queue.push_back(ok);

    AuthSpec auth;
    auto r = client.getTask("http://x", auth, TaskId("task-1"));
    CHECK(r.ok);
    CHECK(r.task.has_value());
    if (r.task) {
        CHECK_EQ(r.task->state, TaskState::Working);
        CHECK_EQ(r.task->state_message, std::string("running"));
        CHECK_EQ(r.task->context.value(), std::string("ctx-1"));
        CHECK_EQ(r.task->messages.size(), (size_t)2);
        CHECK_EQ(r.task->messages[0].role, std::string("user"));
        CHECK_EQ(r.task->messages[1].role, std::string("agent"));
        CHECK_EQ(r.task->artifacts.size(), (size_t)1);
        CHECK(r.task->artifacts[0].name == "out.patch");
        CHECK(r.task->artifacts[0].content_summary().find("diff --git") != std::string::npos);
    }
    // Verify the JSON-RPC method name used.
    auto body = nlohmann::json::parse(mt->last_body);
    CHECK_EQ(body["method"].get<std::string>(), std::string("GetTask"));
    CHECK_EQ(body["jsonrpc"].get<std::string>(), std::string("2.0"));
    CHECK_EQ(body["params"]["id"].get<std::string>(), std::string("task-1"));
}

ADD_TEST(auth_failure_classification) {
    auto mt = std::make_shared<MockTransport>();
    auto creds = make_credential_provider();
    auto creds_shared = std::shared_ptr<CredentialProvider>(std::move(creds));
    A2AClient client(mt, creds_shared);
    mt->queue.push_back(HttpResponse{403, "forbidden", false, ""});

    AuthSpec auth; auth.active = true; auth.header_name = "Authorization"; auth.header_value = "Bearer x";
    auto r = client.getTask("http://x", auth, TaskId("t"));
    CHECK(!r.ok);
    CHECK(r.error_kind == A2AResult::ErrorKind::AuthRequired);
}

ADD_TEST(transport_error_classification) {
    auto mt = std::make_shared<MockTransport>();
    auto creds = make_credential_provider();
    auto creds_shared = std::shared_ptr<CredentialProvider>(std::move(creds));
    A2AClient client(mt, creds_shared);
    mt->queue.push_back(HttpResponse{0, "", true, "Connection refused"});

    AuthSpec auth;
    auto r = client.getTask("http://x", auth, TaskId("t"));
    CHECK(!r.ok);
    CHECK(r.error_kind == A2AResult::ErrorKind::LocalNetwork);
}

ADD_TEST(jsonrpc_error_classification) {
    auto mt = std::make_shared<MockTransport>();
    auto creds = make_credential_provider();
    auto creds_shared = std::shared_ptr<CredentialProvider>(std::move(creds));
    A2AClient client(mt, creds_shared);
    mt->queue.push_back(HttpResponse{200, R"({"jsonrpc":"2.0","id":"1","error":{"code":-32001,"message":"Task not found"}})", false, ""});

    AuthSpec auth;
    auto r = client.getTask("http://x", auth, TaskId("missing"));
    CHECK(!r.ok);
    CHECK(r.error_kind == A2AResult::ErrorKind::ProtocolError);
    CHECK(r.error.find("Task not found") != std::string::npos);
}

ADD_TEST(discover_agent_card) {
    auto mt = std::make_shared<MockTransport>();
    auto creds = make_credential_provider();
    auto creds_shared = std::shared_ptr<CredentialProvider>(std::move(creds));
    A2AClient client(mt, creds_shared);
    mt->fallback = HttpResponse{200, R"({
      "name":"cpp-specialist","description":"A C++ dev agent","version":"1.0.0",
      "capabilities":{"streaming":true},
      "supportedInterfaces":[{"url":"https://dev01.internal/a2a","protocolBinding":"jsonrpc","protocolVersion":"1.0.0"}],
      "securitySchemes":{"apiKey":{"type":"apiKey","name":"X-Api-Key"}}
    })", false, ""};

    auto card = client.discover("https://dev01.internal/a2a");
    CHECK(card.valid);
    CHECK_EQ(card.name, std::string("cpp-specialist"));
    CHECK(card.capabilities.streaming);
    CHECK_EQ(card.interfaces.size(), (size_t)1);
    if (card.interfaces.size() == 1) {
        CHECK_EQ(card.interfaces[0].protocol_binding, std::string("jsonrpc"));
        CHECK_EQ(card.interfaces[0].protocol_version, std::string("1.0.0"));
    }
    CHECK(card.auth_schemes.size() >= 1);
    CHECK(card.requires_auth());
}
