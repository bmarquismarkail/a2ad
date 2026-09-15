#include "test_framework.hpp"
#include "kitty_a2a/A2AClient.hpp"
#include "kitty_a2a/Config.hpp"
#include "kitty_a2a/Credentials.hpp"
#include "kitty_a2a/Database.hpp"
#include "kitty_a2a/TaskManager.hpp"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unistd.h>

using namespace kitty_a2a;
namespace fs = std::filesystem;

namespace {
class ReconcileTransport : public HttpTransport {
public:
    int calls = 0;

    HttpResponse request(const std::string&, const std::string& method,
                         const std::string&, const std::string& body,
                         const std::vector<std::pair<std::string, std::string>>&) override {
        ++calls;
        if (method != "POST") return {404, "", false, ""};
        auto request = nlohmann::json::parse(body);
        nlohmann::json response = nlohmann::json::parse(R"({
          "jsonrpc":"2.0","result":{
            "id":"persisted-task","contextId":"remote-context",
            "status":{"state":"TASK_STATE_WORKING","message":"still running"},
            "history":[],"artifacts":[]
          }
        })");
        response["id"] = request["id"];
        return {200, response.dump(), false, ""};
    }
};

class PrefixDiscoveryTransport : public HttpTransport {
public:
    std::vector<std::string> urls;

    HttpResponse request(const std::string& endpoint, const std::string& method,
                         const std::string&, const std::string&,
                         const std::vector<std::pair<std::string, std::string>>&) override {
        urls.push_back(endpoint);
        if (method != "GET" ||
            endpoint != "https://home.example/a2a/.well-known/agent-card.json") {
            return {404, "", false, ""};
        }
        return {200, R"({
          "name":"home",
          "url":"https://home.example/a2a/",
          "supportedInterfaces":[{
            "url":"https://home.example/a2a/",
            "protocolBinding":"JSONRPC",
            "protocolVersion":"1.0"
          }]
        })", false, ""};
    }
};
}  // namespace

ADD_TEST(configured_agent_overrides_persisted_endpoint) {
    auto dbpath = fs::temp_directory_path() /
                  ("a2ad_agent_endpoint_" + std::to_string(getpid()) + ".db");
    fs::remove(dbpath);

    Database db;
    std::string error;
    CHECK(db.open(dbpath.string(), &error));
    db.saveAgent("hermes-home", "http://localhost:9900", "stale-home", R"({
      "valid":true,
      "name":"stale-home",
      "interfaces":[{
        "url":"http://localhost:9900",
        "protocolBinding":"JSONRPC",
        "protocolVersion":"1.0"
      }]
    })", true, "");

    Config config;
    AgentConfig configured;
    configured.endpoint = "https://home.example/a2a/";
    configured.host_label = "home";
    config.agents["hermes-home"] = configured;

    auto transport = std::make_shared<PrefixDiscoveryTransport>();
    auto credentials = make_credential_provider();
    auto shared_credentials = std::shared_ptr<CredentialProvider>(std::move(credentials));
    auto client = std::make_shared<A2AClient>(transport, shared_credentials);
    TaskManager manager(db, client, config);

    auto agents = manager.listAgents();
    CHECK_EQ(agents.size(), size_t(1));
    CHECK_EQ(agents[0].endpoint, std::string("https://home.example/a2a/"));
    CHECK_EQ(agents[0].host_label, std::string("home"));
    CHECK(!agents[0].available);
    CHECK(!agents[0].card.has_value());

    Agent discovered = manager.discoverAgent("hermes-home");
    CHECK(discovered.available);
    CHECK(discovered.card.has_value());
    CHECK_EQ(discovered.effective_endpoint(), std::string("https://home.example/a2a/"));
    CHECK_EQ(transport->urls.size(), size_t(2));
    CHECK_EQ(transport->urls[0],
             std::string("https://home.example/.well-known/agent-card.json"));
    CHECK_EQ(transport->urls[1],
             std::string("https://home.example/a2a/.well-known/agent-card.json"));

    auto persisted = db.loadAgents();
    CHECK_EQ(persisted.size(), size_t(1));
    CHECK_EQ(persisted[0].endpoint, std::string("https://home.example/a2a/"));

    db.close();
    fs::remove(dbpath);
}

ADD_TEST(public_agent_rejects_loopback_advertised_interface) {
    Agent agent;
    agent.endpoint = "https://marquisthesage.example/a2a/";
    AgentCard card;
    card.interfaces.push_back({"http://127.0.0.1:9910/", "JSONRPC", "1.0", "tenant-a"});
    agent.card = card;

    const auto interface = agent.effective_interface();
    CHECK(interface.has_value());
    CHECK_EQ(interface->url, std::string("https://marquisthesage.example/a2a/"));
    CHECK_EQ(interface->protocol_binding, std::string("JSONRPC"));
    CHECK_EQ(interface->protocol_version, std::string("1.0"));
    CHECK_EQ(interface->tenant, std::string("tenant-a"));
    CHECK_EQ(agent.effective_endpoint(), std::string("https://marquisthesage.example/a2a/"));
}

ADD_TEST(local_agent_accepts_loopback_advertised_interface) {
    Agent agent;
    agent.endpoint = "http://localhost:9900/";
    AgentCard card;
    card.interfaces.push_back({"http://127.0.0.1:9910/", "JSONRPC", "1.0", ""});
    agent.card = card;

    CHECK_EQ(agent.effective_endpoint(), std::string("http://127.0.0.1:9910/"));
}

ADD_TEST(reconciliation_preserves_agent_routing) {
    auto dbpath = fs::temp_directory_path() /
                  ("a2ad_reconcile_" + std::to_string(getpid()) + ".db");
    fs::remove(dbpath);

    Database db;
    std::string error;
    CHECK(db.open(dbpath.string(), &error));

    Task persisted;
    persisted.id = "persisted-task";
    persisted.remote_task_id = TaskId("persisted-task");
    persisted.agent = "configured-agent";
    persisted.context = "old-context";
    persisted.state = TaskState::Working;
    persisted.title = "local title";
    persisted.cwd = "/local/cwd";
    persisted.created_at = "T0";
    persisted.updated_at = "T0";
    db.insertTask(persisted);

    Config config;
    AgentConfig agent;
    agent.endpoint = "http://agent.example/a2a";
    config.agents["configured-agent"] = agent;

    auto transport = std::make_shared<ReconcileTransport>();
    auto credentials = make_credential_provider();
    auto shared_credentials = std::shared_ptr<CredentialProvider>(std::move(credentials));
    auto client = std::make_shared<A2AClient>(transport, shared_credentials);
    TaskManager manager(db, client, config);

    manager.reconcileOnStartup();
    auto reconciled = db.getTask("persisted-task");
    CHECK(reconciled.has_value());
    if (reconciled) {
        CHECK_EQ(reconciled->agent.value(), std::string("configured-agent"));
        CHECK_EQ(reconciled->title, std::string("local title"));
        CHECK_EQ(reconciled->cwd, std::string("/local/cwd"));
        CHECK_EQ(reconciled->context.value(), std::string("remote-context"));
    }

    auto refreshed = manager.refreshTask("persisted-task");
    CHECK(refreshed.ok);
    // Three lazy discovery probes, then two task reads. Discovery is cached.
    CHECK_EQ(transport->calls, 5);

    db.close();
    fs::remove(dbpath);
}

ADD_TEST(artifact_digest_and_symlink_protection) {
    auto dir = fs::temp_directory_path() / ("a2ad_artifact_guard_" + std::to_string(getpid()));
    fs::remove_all(dir); fs::create_directories(dir);
    Database db; std::string error, output;
    CHECK(db.open((dir / "state.db").string(), &error));
    Config config;
    auto transport = std::make_shared<ReconcileTransport>();
    auto client = std::make_shared<A2AClient>(transport, std::shared_ptr<CredentialProvider>(make_credential_provider()));
    TaskManager manager(db, client, config);
    Task task; task.id = "artifact-task"; task.agent = "unused"; task.created_at = "T0"; task.updated_at = "T0";
    Artifact artifact; artifact.artifact_id = "artifact";
    Part part; part.text = "abc"; part.filename = "result.txt"; artifact.parts.push_back(part);
    task.artifacts.push_back(artifact); db.insertTask(task);
    CHECK(!manager.materializeArtifact("artifact-task", "artifact", 0, dir.string(), &output, &error, "wrong-digest"));
    CHECK(!fs::exists(dir / "result.txt"));
    std::ofstream(dir / "target.txt") << "untouched";
    fs::create_symlink(dir / "target.txt", dir / "result.txt");
    CHECK(!manager.materializeArtifact("artifact-task", "artifact", 0, dir.string(), &output, &error));
    fs::remove(dir / "result.txt");
    CHECK(manager.materializeArtifact("artifact-task", "artifact", 0, dir.string(), &output, &error,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    std::ifstream in(dir / "target.txt"); std::string original; in >> original; CHECK_EQ(original, "untouched");
    CHECK(!manager.materializeArtifact("artifact-task", "artifact", 0, dir.string(), &output, &error));
    db.close(); fs::remove_all(dir);
}
ADD_TEST(remote_task_ids_are_namespaced_by_local_identity) {
    auto path = fs::temp_directory_path() / ("a2ad_task_identity_" + std::to_string(getpid()) + ".db");
    fs::remove(path); Database db; std::string error; CHECK(db.open(path.string(), &error));
    Config config; AgentConfig a; a.endpoint = "https://agent.example/a2a";
    config.agents["first"] = a; config.agents["second"] = a;
    auto transport = std::make_shared<ReconcileTransport>();
    auto client = std::make_shared<A2AClient>(transport, std::shared_ptr<CredentialProvider>(make_credential_provider()));
    TaskManager manager(db, client, config);
    CreateTaskRequest request; request.agent = "first"; request.message = "test";
    auto first = manager.createTask(request); request.agent = "second";
    auto second = manager.createTask(request);
    CHECK(first.ok && second.ok); CHECK(first.task_id != second.task_id);
    CHECK_EQ(db.listTasks().size(), size_t{2});
    auto stored = db.getTask(first.task_id.value());
    CHECK(stored && stored->remote_task_id && stored->remote_task_id->value() == "persisted-task");
    db.close(); fs::remove(path);
}

ADD_TEST(discovery_revalidates_etag_without_losing_card) {
    class Conditional : public HttpTransport {
    public:
        bool conditional = false;
        int calls = 0;
        HttpResponse request(const std::string&, const std::string&, const std::string&, const std::string&,
                             const std::vector<std::pair<std::string, std::string>>& headers) override {
            ++calls;
            if (calls == 1) return {200,
                R"({"name":"cached","supportedInterfaces":[{"url":"https://agent.example","protocolBinding":"JSONRPC","protocolVersion":"1.0"}]})",
                false, "", {{"etag", "\"v1\""}}};
            for (const auto& header : headers) if (header.first == "If-None-Match" && header.second == "\"v1\"") conditional = true;
            return {304, "", false, ""};
        }
    };
    auto transport = std::make_shared<Conditional>();
    A2AClient client(transport, std::shared_ptr<CredentialProvider>(make_credential_provider()));
    CHECK(client.discover("https://agent.example").valid);
    auto card = client.discover("https://agent.example");
    CHECK(card.valid); CHECK_EQ(card.name, "cached"); CHECK(transport->conditional);
}
