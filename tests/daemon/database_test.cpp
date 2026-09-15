#include "test_framework.hpp"
#include "kitty_a2a/Database.hpp"
#include "kitty_a2a/Task.hpp"
#include <filesystem>
#include <fstream>
#include <sqlite3.h>

using namespace kitty_a2a;
namespace fs = std::filesystem;

ADD_TEST(task_roundtrip) {
    auto dbpath = fs::temp_directory_path() / "a2ad_test.db";
    fs::remove(dbpath);
    Database db;
    std::string err;
    CHECK(db.open(dbpath.string(), &err));
    if (err.empty()) {
        Task t;
        t.id = "task-abc";
        t.remote_task_id = TaskId("remote-abc");
        t.agent = "cpp-specialist";
        t.context = "ctx-1";
        t.state = TaskState::Working;
        t.title = "fix the bug";
        t.cwd = "/data/projects/proto-time";
        t.created_at = "2026-08-24T00:00:00Z";
        t.updated_at = "2026-08-24T00:00:01Z";
        t.last_state_change = "2026-08-24T00:00:01Z";
        t.metadata = {{"trace", "yes"}};

        Message m; m.role = "agent"; m.message_id = "m1";
        Part p; p.text = "on it";
        m.parts.push_back(p);
        t.messages.push_back(m);

        Artifact a; a.artifact_id = "a1"; a.name = "fix.patch";
        Part fp; fp.text = "diff --git"; a.parts.push_back(fp);
        t.artifacts.push_back(a);

        db.insertTask(t);

        auto loaded = db.getTask("task-abc");
        CHECK(loaded.has_value());
        if (loaded) {
            CHECK_EQ(loaded->state, TaskState::Working);
            CHECK(loaded->remote_task_id.has_value());
            if (loaded->remote_task_id) CHECK_EQ(loaded->remote_task_id->value(), std::string("remote-abc"));
            CHECK_EQ(loaded->metadata.value("trace", ""), std::string("yes"));
            CHECK_EQ(loaded->title, std::string("fix the bug"));
            CHECK_EQ(loaded->cwd, std::string("/data/projects/proto-time"));
            CHECK_EQ(loaded->messages.size(), (size_t)1);
            CHECK_EQ(loaded->messages[0].role, std::string("agent"));
            CHECK_EQ(loaded->artifacts.size(), (size_t)1);
            CHECK_EQ(loaded->artifacts[0].name, std::string("fix.patch"));
        }
    }
    db.close();
    fs::remove(dbpath);
}

ADD_TEST(migrates_v1_database_once) {
    auto dbpath = fs::temp_directory_path() / "a2ad_test_v1_migration.db";
    fs::remove(dbpath);
    sqlite3* raw = nullptr;
    CHECK_EQ(sqlite3_open(dbpath.c_str(), &raw), SQLITE_OK);
    const char* schema =
        "CREATE TABLE schema_version(version INTEGER NOT NULL); INSERT INTO schema_version VALUES(1);"
        "CREATE TABLE agents(id TEXT PRIMARY KEY,endpoint TEXT NOT NULL,host_label TEXT NOT NULL DEFAULT '',card_json TEXT NOT NULL DEFAULT '{}',available INTEGER NOT NULL DEFAULT 0,last_error TEXT NOT NULL DEFAULT '');"
        "CREATE TABLE tasks(id TEXT PRIMARY KEY,agent TEXT NOT NULL,context_id TEXT NOT NULL DEFAULT '',state TEXT NOT NULL,state_message TEXT NOT NULL DEFAULT '',title TEXT NOT NULL DEFAULT '',cwd TEXT NOT NULL DEFAULT '',created_at TEXT NOT NULL,updated_at TEXT NOT NULL,last_state_change TEXT NOT NULL,messages TEXT NOT NULL DEFAULT '[]',artifacts TEXT NOT NULL DEFAULT '[]',error TEXT NOT NULL DEFAULT '');"
        "INSERT INTO tasks VALUES('old','agent','ctx','WORKING','','','','T0','T0','T0','[]','[]','');";
    CHECK_EQ(sqlite3_exec(raw, schema, nullptr, nullptr, nullptr), SQLITE_OK);
    sqlite3_close(raw);
    Database db; std::string error;
    CHECK(db.open(dbpath.string(), &error));
    auto migrated = db.getTask("old");
    CHECK(migrated.has_value());
    if (migrated) {
        CHECK(migrated->remote_task_id.has_value());
        if (migrated->remote_task_id) CHECK_EQ(migrated->remote_task_id->value(), std::string("old"));
    }
    db.close();
    CHECK(db.open(dbpath.string(), &error));
    db.close(); fs::remove(dbpath);
}

ADD_TEST(update_task) {
    auto dbpath = fs::temp_directory_path() / "a2ad_test2.db";
    fs::remove(dbpath);
    Database db;
    std::string err;
    CHECK(db.open(dbpath.string(), &err));
    if (err.empty()) {
        Task t;
        t.id = "t1"; t.agent = "g"; t.context = "c";
        t.state = TaskState::Submitted;
        t.title = "x"; t.created_at = "T0"; t.updated_at = "T0";
        db.insertTask(t);

        t.state = TaskState::Completed;
        t.state_message = "done";
        t.updated_at = "T1";
        db.updateTask(t);

        auto loaded = db.getTask("t1");
        CHECK(loaded.has_value());
        if (loaded) {
            CHECK_EQ(loaded->state, TaskState::Completed);
            CHECK_EQ(loaded->state_message, std::string("done"));
        }
    }
    db.close();
    fs::remove(dbpath);
}

ADD_TEST(list_and_terminal_filter) {
    auto dbpath = fs::temp_directory_path() / "a2ad_test3.db";
    fs::remove(dbpath);
    Database db;
    std::string err;
    CHECK(db.open(dbpath.string(), &err));
    if (err.empty()) {
        auto mk = [&](const char* id, TaskState s) {
            Task t; t.id = id; t.agent = "g"; t.context = "c";
            t.state = s; t.title = id; t.created_at = "T0"; t.updated_at = "T0";
            db.insertTask(t);
        };
        mk("working", TaskState::Working);
        mk("done", TaskState::Completed);
        mk("failed", TaskState::Failed);
        mk("inputreq", TaskState::InputRequired);

        auto all = db.listTasks(true);
        CHECK_EQ(all.size(), (size_t)4);

        auto nonterminal = db.listTasks(false);
        // Working + InputRequired are non-terminal; Completed + Failed are terminal.
        CHECK_EQ(nonterminal.size(), (size_t)2);

        auto nont = db.listNonTerminalTasks();
        CHECK_EQ(nont.size(), (size_t)2);
    }
    db.close();
    fs::remove(dbpath);
}

ADD_TEST(persistence_across_reopen) {
    auto dbpath = fs::temp_directory_path() / "a2ad_test4.db";
    fs::remove(dbpath);
    {
        Database db;
        std::string err;
        CHECK(db.open(dbpath.string(), &err));
        Task t; t.id = "persist"; t.agent = "g"; t.context = "c";
        t.state = TaskState::Working; t.title = "p"; t.created_at = "T0"; t.updated_at = "T0";
        db.insertTask(t);
        db.close();
    }
    {
        Database db2;
        std::string err2;
        CHECK(db2.open(dbpath.string(), &err2));
        auto loaded = db2.getTask("persist");
        CHECK(loaded.has_value());
        if (loaded) CHECK_EQ(loaded->state, TaskState::Working);
        db2.close();
    }
    fs::remove(dbpath);
}

ADD_TEST(message_only_interaction_remains_local_after_reopen) {
    auto path = fs::temp_directory_path() / "a2ad_message_only_restart.db";
    fs::remove(path);
    Database db; std::string error;
    CHECK(db.open(path.string(), &error));
    Task task; task.id = "local-only"; task.agent = "agent";
    task.state = TaskState::Completed; task.created_at = "T0"; task.updated_at = "T0";
    db.insertTask(task); db.close();
    CHECK(db.open(path.string(), &error));
    auto loaded = db.getTask("local-only");
    CHECK(loaded.has_value());
    if (loaded) CHECK(!loaded->remote_task_id.has_value());
    db.close(); fs::remove(path);
}
