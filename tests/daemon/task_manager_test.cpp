#include "test_framework.hpp"
#include "kitty_a2a/A2AClient.hpp"
#include "kitty_a2a/Config.hpp"
#include "kitty_a2a/Credentials.hpp"
#include "kitty_a2a/Database.hpp"
#include "kitty_a2a/TaskManager.hpp"

#include <filesystem>
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
                         const std::string&, const std::string&,
                         const std::vector<std::pair<std::string, std::string>>&) override {
        ++calls;
        if (method != "POST") return {404, "", false, ""};
        return {200, R"({
          "jsonrpc":"2.0","id":"1","result":{
            "id":"persisted-task","contextId":"remote-context",
            "status":{"state":"TASK_STATE_WORKING","message":"still running"},
            "history":[],"artifacts":[]
          }
        })", false, ""};
    }
};
}  // namespace

ADD_TEST(reconciliation_preserves_agent_routing) {
    auto dbpath = fs::temp_directory_path() /
                  ("a2ad_reconcile_" + std::to_string(getpid()) + ".db");
    fs::remove(dbpath);

    Database db;
    std::string error;
    CHECK(db.open(dbpath.string(), &error));

    Task persisted;
    persisted.id = "persisted-task";
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
    CHECK_EQ(transport->calls, 2);

    db.close();
    fs::remove(dbpath);
}
