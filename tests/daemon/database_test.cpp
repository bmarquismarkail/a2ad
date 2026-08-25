#include "test_framework.hpp"
#include "kitty_a2a/Database.hpp"
#include "kitty_a2a/Task.hpp"
#include <filesystem>
#include <fstream>

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
        t.agent = "cpp-specialist";
        t.context = "ctx-1";
        t.state = TaskState::Working;
        t.title = "fix the bug";
        t.cwd = "/data/projects/proto-time";
        t.created_at = "2026-08-24T00:00:00Z";
        t.updated_at = "2026-08-24T00:00:01Z";
        t.last_state_change = "2026-08-24T00:00:01Z";

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
