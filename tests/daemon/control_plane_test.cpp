#include "test_framework.hpp"
#include "kitty_a2a/ControlPlane.hpp"
#include <filesystem>
#include <thread>
#include <atomic>

using kitty_a2a::ControlPlane;
using J = nlohmann::json;
namespace {
struct Store {
    std::string path = (std::filesystem::temp_directory_path() / ("a2ad-control-" + ControlPlane::newId())).string();
    ~Store() { for (auto suffix : {"", "-wal", "-shm"}) std::filesystem::remove(path + suffix); }
};
auto unused = [](const J&) -> J { return {{"ok", false}, {"error", "unexpected dispatch"}}; };
}
ADD_TEST(control_approval_exact_request_and_replay) {
    Store store; ControlPlane plane(store.path, true); int sends = 0;
    auto send = [&](const J&) -> J { ++sends; return {{"ok", true}, {"task_id", "remote-task"}}; };
    J request = {{"op", "submit"}, {"agent", "worker"}, {"message", "private prompt"}, {"request_id", "req-1"}};
    auto pending = plane.handle(request, send);
    CHECK_EQ(pending["error_kind"], "approval_required"); CHECK_EQ(sends, 0);
    CHECK(plane.handle({{"op", "approval.decide"}, {"id", "req-1"}, {"allow", true}}, unused)["ok"] == true);
    auto altered = request; altered["message"] = "different";
    CHECK_EQ(plane.handle(altered, send)["error_kind"], "idempotency_conflict");
    CHECK(plane.handle(request, send)["ok"] == true);
    CHECK(plane.handle(request, send)["ok"] == true); CHECK_EQ(sends, 1);
    CHECK(plane.handle({{"op", "audit.verify"}}, unused)["ok"] == true);
    auto events = plane.handle({{"op", "events.read"}}, unused);
    CHECK(events.dump().find("private prompt") == std::string::npos);
}
ADD_TEST(control_restart_preserves_sessions_and_uncertain_dispatch) {
    Store store; J request = {{"op", "submit"}, {"agent", "worker"}, {"request_id", "uncertain"}};
    int sends = 0;
    {
        ControlPlane plane(store.path);
        CHECK(plane.handle({{"op", "session.create"}, {"id", "session-1"}}, unused)["ok"] == true);
        request["session_id"] = "session-1";
        plane.handle(request, [&](const J&) -> J { ++sends; return {{"ok", false}, {"error_kind", "local_network"}}; });
    }
    {
        ControlPlane plane(store.path);
        CHECK_EQ(plane.handle(request, unused)["error_kind"], "outcome_uncertain");
        CHECK_EQ(plane.handle({{"op", "session.get"}, {"id", "session-1"}}, unused)["record"]["id"], "session-1");
        CHECK_EQ(plane.handle({{"op", "execution.get"}, {"id", "uncertain"}}, unused)["record"]["state"], "UNCERTAIN");
        CHECK_EQ(sends, 1);
    }
}
ADD_TEST(control_channel_replay_ack_and_restart) {
    Store store;
    {
        ControlPlane plane(store.path);
        plane.event({{"type", "test.one"}}); plane.event({{"type", "test.two"}});
        CHECK(plane.handle({{"op", "channel.open"}, {"id", "frontend"}}, unused)["ok"] == true);
        auto page = plane.handle({{"op", "channel.read"}, {"id", "frontend"}, {"limit", 1}}, unused);
        CHECK_EQ(page["events"].size(), size_t{1});
        CHECK_EQ(plane.handle({{"op", "channel.read"}, {"id", "frontend"}, {"limit", 1}}, unused)["events"], page["events"]);
        CHECK_EQ(plane.handle({{"op", "channel.ack"}, {"id", "frontend"}, {"cursor", 2}}, unused)["error_kind"], "invalid_cursor");
        CHECK(plane.handle({{"op", "channel.ack"}, {"id", "frontend"}, {"cursor", 1}}, unused)["ok"] == true);
    }
    ControlPlane plane(store.path);
    auto page = plane.handle({{"op", "channel.read"}, {"id", "frontend"}}, unused);
    CHECK_EQ(page["events"].size(), size_t{1}); CHECK_EQ(page["events"][0]["sequence"], 2);
    CHECK_EQ(plane.handle({{"op", "events.read"}, {"limit", 501}}, unused)["error_kind"], "invalid_request");
}
ADD_TEST(control_delegation_cannot_escalate) {
    Store store; ControlPlane plane(store.path);
    auto send = [](const J&) -> J { return {{"ok", true}}; };
    J parent = {{"op", "submit"}, {"agent", "parent"}, {"request_id", "parent"}, {"effects", {"EXECUTE_REMOTE", "DELEGATE"}}};
    CHECK_EQ(plane.handle(parent, send)["error_kind"], "approval_required");
    plane.handle({{"op", "approval.decide"}, {"id", "parent"}, {"allow", true}}, unused);
    CHECK(plane.handle(parent, send)["ok"] == true);
    J child = {{"op", "submit"}, {"agent", "child"}, {"request_id", "child"}, {"parent_execution_id", "parent"}, {"effects", {"PUBLISH"}}};
    CHECK_EQ(plane.handle(child, send)["error_kind"], "permission_denied");
    child["effects"] = J::array({"EXECUTE_REMOTE"});
    CHECK_EQ(plane.handle(child, send)["error_kind"], "approval_required");
    plane.handle({{"op", "approval.decide"}, {"id", "child"}, {"allow", true}}, unused);
    CHECK(plane.handle(child, send)["ok"] == true);
    CHECK_EQ(plane.handle({{"op", "execution.get"}, {"id", "child"}}, unused)["record"]["identity_chain"].size(), size_t{2});
}
ADD_TEST(control_quarantine_and_denial) {
    Store store; ControlPlane plane(store.path);
    for (int i = 0; i < 3; ++i) {
        plane.handle({{"op", "submit"}, {"agent", "flaky"}, {"request_id", std::to_string(i)}},
            [](const J&) -> J { return {{"ok", false}, {"error_kind", "local_network"}}; });
    }
    CHECK_EQ(plane.handle({{"op", "submit"}, {"agent", "flaky"}, {"request_id", "blocked"}}, unused)["error_kind"], "quarantined");
    CHECK(plane.handle({{"op", "agent.release"}, {"agent", "flaky"}}, unused)["ok"] == true);
    CHECK(plane.health("flaky")["quarantined"] == false);
    J request = {{"op", "submit"}, {"agent", "flaky"}, {"request_id", "denied"}, {"effects", {"PUBLISH"}}};
    plane.handle(request, unused);
    plane.handle({{"op", "approval.decide"}, {"id", "denied"}, {"allow", false}}, unused);
    CHECK_EQ(plane.handle(request, unused)["error_kind"], "permission_denied");
}
ADD_TEST(control_digest_known_vector) {
    CHECK_EQ(ControlPlane::digest("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

ADD_TEST(control_lease_fencing_and_revocation) {
    Store store; ControlPlane plane(store.path);
    auto lease = plane.handle({{"op", "lease.acquire"}, {"resource", "build-tree"}, {"owner", "one"}}, unused)["lease"];
    CHECK_EQ(lease["fence"], 1);
    CHECK_EQ(plane.handle({{"op", "lease.acquire"}, {"resource", "build-tree"}, {"owner", "two"}}, unused)["error_kind"], "lease_busy");
    CHECK_EQ(plane.handle({{"op", "lease.renew"}, {"resource", "build-tree"}, {"owner", "one"}, {"token", "forged"}}, unused)["error_kind"], "lease_lost");
    CHECK(plane.handle({{"op", "lease.release"}, {"resource", "build-tree"}, {"owner", "one"}, {"token", lease["token"]}}, unused)["ok"] == true);
    auto replacement = plane.handle({{"op", "lease.acquire"}, {"resource", "build-tree"}, {"owner", "two"}}, unused)["lease"];
    CHECK_EQ(replacement["fence"], 2);
    CHECK_EQ(plane.handle({{"op", "lease.renew"}, {"resource", "build-tree"}, {"owner", "one"}, {"token", lease["token"]}}, unused)["error_kind"], "lease_lost");
}
ADD_TEST(control_concurrent_duplicate_never_dispatches_twice) {
    Store store; ControlPlane plane(store.path);
    J request = {{"op", "submit"}, {"agent", "worker"}, {"request_id", "concurrent"}};
    std::atomic<bool> entered = false, release = false;
    std::thread first([&] {
        plane.handle(request, [&](const J&) -> J {
            entered = true; while (!release.load()) std::this_thread::yield();
            return {{"ok", true}};
        });
    });
    while (!entered.load()) std::this_thread::yield();
    CHECK_EQ(plane.handle(request, unused)["error_kind"], "outcome_uncertain");
    release = true; first.join();
    CHECK(plane.handle(request, unused)["ok"] == true);
}

ADD_TEST(control_handoff_bound_to_task_and_resolved_by_delivery) {
    Store store; ControlPlane plane(store.path);
    auto continuation = plane.handle({{"op", "continuation.create"}, {"task_id", "task-1"}, {"kind", "HANDOFF"}}, unused)["continuation"];
    J request = {{"op", "respond"}, {"task_id", "wrong"}, {"request_id", "answer"}, {"continuation_id", continuation["id"]}};
    CHECK_EQ(plane.handle(request, unused)["error_kind"], "invalid_continuation");
    request["task_id"] = "task-1";
    CHECK(plane.handle(request, [](const J&) -> J { return {{"ok", true}}; })["ok"] == true);
    CHECK_EQ(plane.handle({{"op", "continuation.list"}}, unused)["items"][0]["state"], "DELIVERED");
    request["request_id"] = "answer-again";
    CHECK_EQ(plane.handle(request, unused)["error_kind"], "invalid_continuation");
}
ADD_TEST(control_revoked_ancestor_blocks_delegation) {
    Store store; ControlPlane plane(store.path);
    J parent = {{"op", "submit"}, {"agent", "one"}, {"request_id", "root"}, {"effects", {"EXECUTE_REMOTE", "DELEGATE"}}};
    plane.handle(parent, unused);
    plane.handle({{"op", "approval.decide"}, {"id", "root"}, {"allow", true}}, unused);
    CHECK(plane.handle(parent, [](const J&) -> J { return {{"ok", true}}; })["ok"] == true);
    plane.handle({{"op", "approval.revoke"}, {"id", "root"}}, unused);
    auto denied = plane.handle({{"op", "submit"}, {"agent", "two"}, {"request_id", "child"},
        {"parent_execution_id", "root"}, {"effects", {"EXECUTE_REMOTE"}}}, unused);
    CHECK_EQ(denied["error_kind"], "permission_denied");
}
