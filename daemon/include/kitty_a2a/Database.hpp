#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "kitty_a2a/AgentCard.hpp"
#include "kitty_a2a/Task.hpp"

namespace kitty_a2a {

// SQLite persistence (DESIGN.md §19). The daemon persists just enough to
// survive Kitty restarts and its own restarts: agents + cards, and the task
// table (ids, agent, context, state, timestamps, messages, artifacts, project
// association).
//
// Concurrency: a single sqlite handle guarded by one std::mutex. All daemon
// mutations happen on the worker thread; IPC handlers take the same lock. This
// keeps the SQLite connection single-threaded (SQLITE_OPEN_NOSMALLFTS off,
// default serialized mode) and avoids cross-thread fd sharing entirely.
class Database {
public:
    struct DbImpl;  // opaque; defined in Database.cpp

    Database();
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    bool open(const std::string& path, std::string* error = nullptr);
    void close();
    bool isOpen() const { return open_; }

    // -- Agents -------------------------------------------------------------
    // Upsert agent identity + card JSON. `available`/`last_error` track health.
    void saveAgent(const std::string& id, const std::string& endpoint,
                   const std::string& host_label, const std::string& card_json,
                   bool available, const std::string& last_error);
    std::vector<Agent> loadAgents();

    // -- Tasks --------------------------------------------------------------
    // Insert a fresh task record (state SUBMITTED). Returns the row.
    void insertTask(const Task& task);

    // Update an existing task's mutable fields. Unknown id is a no-op.
    void updateTask(const Task& task);

    std::optional<Task> getTask(const std::string& id);
    std::vector<Task> listTasks(bool include_terminal = true);

    // Reconcile helper: all non-terminal tasks (for daemon-start re-sync).
    std::vector<Task> listNonTerminalTasks();

    void dropTask(const std::string& id);

private:
    // Serialize a Task to a JSON blob for the messages/artifacts columns.
    std::unique_ptr<DbImpl> impl_;
    std::mutex mutex_;
    bool open_ = false;
};

}  // namespace kitty_a2a
