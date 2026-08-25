#include "kitty_a2a/Database.hpp"

#include <sqlite3.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <ctime>
#include <stdexcept>


namespace kitty_a2a {

namespace {
std::string now_iso() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}
}  // namespace

struct Database::DbImpl {
    sqlite3* db = nullptr;
};

Database::Database() = default;

Database::~Database() { close(); }

bool Database::open(const std::string& path, std::string* error) {
    close();
    impl_ = std::make_unique<DbImpl>();
    int rc = sqlite3_open(path.c_str(), &impl_->db);
    if (rc != SQLITE_OK) {
        if (error) *error = "cannot open database: " + std::string(sqlite3_errmsg(impl_->db));
        return false;
    }
    // Serialized mode is default (single connection). Enable WAL for better
    // read/write overlap while remaining single-connection in practice.
    sqlite3_exec(impl_->db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(impl_->db, "PRAGMA foreign_keys=ON;", nullptr, nullptr, nullptr);

    const char* schema = R"SQL(
    CREATE TABLE IF NOT EXISTS schema_version (version INTEGER NOT NULL);
    CREATE TABLE IF NOT EXISTS agents (
        id          TEXT PRIMARY KEY,
        endpoint    TEXT NOT NULL,
        host_label  TEXT NOT NULL DEFAULT '',
        card_json   TEXT NOT NULL DEFAULT '{}',
        available   INTEGER NOT NULL DEFAULT 0,
        last_error  TEXT NOT NULL DEFAULT ''
    );
    CREATE TABLE IF NOT EXISTS tasks (
        id          TEXT PRIMARY KEY,
        agent       TEXT NOT NULL,
        context_id  TEXT NOT NULL DEFAULT '',
        state       TEXT NOT NULL,
        state_message TEXT NOT NULL DEFAULT '',
        title       TEXT NOT NULL DEFAULT '',
        cwd         TEXT NOT NULL DEFAULT '',
        created_at  TEXT NOT NULL,
        updated_at  TEXT NOT NULL,
        last_state_change TEXT NOT NULL,
        messages    TEXT NOT NULL DEFAULT '[]',
        artifacts   TEXT NOT NULL DEFAULT '[]',
        error       TEXT NOT NULL DEFAULT ''
    );
    CREATE INDEX IF NOT EXISTS idx_tasks_state ON tasks(state);
    CREATE INDEX IF NOT EXISTS idx_tasks_agent ON tasks(agent);
    )SQL";
    char* err = nullptr;
    rc = sqlite3_exec(impl_->db, schema, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::string msg = err ? err : "unknown";
        sqlite3_free(err);
        if (error) *error = "schema setup failed: " + msg;
        close();
        return false;
    }

    // Bump schema version (idempotent for this milestone).
    sqlite3_exec(impl_->db,
        "DELETE FROM schema_version; INSERT INTO schema_version(version) VALUES(1);",
        nullptr, nullptr, nullptr);

    open_ = true;
    return true;
}

void Database::close() {
    if (impl_ && impl_->db) {
        sqlite3_close(impl_->db);
        impl_->db = nullptr;
    }
    open_ = false;
}

void Database::saveAgent(const std::string& id, const std::string& endpoint,
                         const std::string& host_label, const std::string& card_json,
                         bool available, const std::string& last_error) {
    std::lock_guard lk(mutex_);
    if (!impl_->db) return;
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "INSERT INTO agents(id, endpoint, host_label, card_json, available, last_error) "
        "VALUES(?,?,?,?,?,?) "
        "ON CONFLICT(id) DO UPDATE SET endpoint=excluded.endpoint, "
        "host_label=excluded.host_label, card_json=excluded.card_json, "
        "available=excluded.available, last_error=excluded.last_error;";
    if (sqlite3_prepare_v2(impl_->db, sql, -1, &st, nullptr) != SQLITE_OK) return;
    sqlite3_bind_text(st, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, endpoint.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, host_label.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, card_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 5, available ? 1 : 0);
    sqlite3_bind_text(st, 6, last_error.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

std::vector<Agent> Database::loadAgents() {
    std::lock_guard lk(mutex_);
    std::vector<Agent> out;
    if (!impl_->db) return out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(impl_->db,
            "SELECT id, endpoint, host_label, card_json, available, last_error FROM agents",
            -1, &st, nullptr) != SQLITE_OK) return out;
    while (sqlite3_step(st) == SQLITE_ROW) {
        Agent a;
        a.id = (const char*)sqlite3_column_text(st, 0);
        a.endpoint = (const char*)sqlite3_column_text(st, 1);
        a.host_label = (const char*)sqlite3_column_text(st, 2);
        const char* card = (const char*)sqlite3_column_text(st, 3);
        try {
            auto j = nlohmann::json::parse(card ? card : "{}");
            // We persist the card as the parsed AgentCard's JSON; reconstruct via
            // the same fields used to build it in TaskManager.
            a.card = AgentCard();
            a.card->valid = j.value("valid", false);
            a.card->name = j.value("name", "");
            a.card->description = j.value("description", "");
            a.card->version = j.value("version", "");
            if (j.contains("capabilities") && j["capabilities"].is_object()) {
                const auto& c = j["capabilities"];
                a.card->capabilities.streaming = c.value("streaming", false);
                a.card->capabilities.push_notifications = c.value("pushNotifications", false);
                a.card->capabilities.extended_agent_card = c.value("extendedAgentCard", false);
            }
            if (j.contains("interfaces") && j["interfaces"].is_array()) {
                for (const auto& it : j["interfaces"]) {
                    AgentInterface ai;
                    ai.url = it.value("url", "");
                    ai.protocol_binding = it.value("protocolBinding", "jsonrpc");
                    ai.protocol_version = it.value("protocolVersion", "");
                    a.card->interfaces.push_back(std::move(ai));
                }
            }
            if (j.contains("auth_schemes") && j["auth_schemes"].is_array())
                for (const auto& s : j["auth_schemes"]) if (s.is_string()) a.card->auth_schemes.push_back(s.get<std::string>());
        } catch (...) { a.card = std::nullopt; }
        a.available = sqlite3_column_int(st, 4) != 0;
        a.last_error = (const char*)sqlite3_column_text(st, 5);
        out.push_back(std::move(a));
    }
    sqlite3_finalize(st);
    return out;
}

static void bindTask(sqlite3_stmt* st, const Task& t) {
    sqlite3_bind_text(st, 1, t.id.value().c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, t.agent.value().c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, t.context.value().c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, to_state_string(t.state).c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, t.state_message.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 6, t.title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 7, t.cwd.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 8, t.created_at.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 9, t.updated_at.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 10, t.last_state_change.c_str(), -1, SQLITE_TRANSIENT);
    // messages / artifacts serialized
    nlohmann::json msgs = nlohmann::json::array();
    for (const auto& m : t.messages) {
        nlohmann::json mj;
        mj["messageId"] = m.message_id;
        mj["role"] = m.role;
        mj["contextId"] = m.context_id.value();
        mj["taskId"] = m.task_id.value();
        mj["timestamp"] = m.timestamp;
        nlohmann::json parts = nlohmann::json::array();
        for (const auto& p : m.parts) {
            nlohmann::json pj;
            if (!p.text.empty()) pj["text"] = p.text;
            if (!p.raw_b64.empty()) pj["raw"] = p.raw_b64;
            if (!p.url.empty()) pj["url"] = p.url;
            if (!p.data.empty()) { try { pj["data"] = nlohmann::json::parse(p.data); } catch (...) { pj["data"] = p.data; } }
            if (!p.media_type.empty()) pj["mediaType"] = p.media_type;
            if (!p.filename.empty()) pj["filename"] = p.filename;
            parts.push_back(pj);
        }
        mj["parts"] = parts;
        msgs.push_back(mj);
    }
    std::string msgs_s = msgs.dump();
    sqlite3_bind_text(st, 11, msgs_s.c_str(), -1, SQLITE_TRANSIENT);

    nlohmann::json arts = nlohmann::json::array();
    for (const auto& a : t.artifacts) {
        nlohmann::json aj;
        aj["artifactId"] = a.artifact_id;
        aj["name"] = a.name;
        aj["description"] = a.description;
        nlohmann::json parts = nlohmann::json::array();
        for (const auto& p : a.parts) {
            nlohmann::json pj;
            if (!p.text.empty()) pj["text"] = p.text;
            if (!p.raw_b64.empty()) pj["raw"] = p.raw_b64;
            if (!p.url.empty()) pj["url"] = p.url;
            if (!p.data.empty()) { try { pj["data"] = nlohmann::json::parse(p.data); } catch (...) { pj["data"] = p.data; } }
            if (!p.media_type.empty()) pj["mediaType"] = p.media_type;
            if (!p.filename.empty()) pj["filename"] = p.filename;
            parts.push_back(pj);
        }
        aj["parts"] = parts;
        arts.push_back(aj);
    }
    std::string arts_s = arts.dump();
    sqlite3_bind_text(st, 12, arts_s.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 13, t.error.c_str(), -1, SQLITE_TRANSIENT);
}

static Task taskFromRow(sqlite3_stmt* st) {
    Task t;
    t.id = (const char*)sqlite3_column_text(st, 0);
    t.agent = (const char*)sqlite3_column_text(st, 1);
    t.context = (const char*)sqlite3_column_text(st, 2);
    t.state = parse_task_state((const char*)sqlite3_column_text(st, 3)).value_or(TaskState::Submitted);
    t.state_message = (const char*)sqlite3_column_text(st, 3 + 1);
    t.title = (const char*)sqlite3_column_text(st, 5);
    t.cwd = (const char*)sqlite3_column_text(st, 6);
    t.created_at = (const char*)sqlite3_column_text(st, 7);
    t.updated_at = (const char*)sqlite3_column_text(st, 8);
    t.last_state_change = (const char*)sqlite3_column_text(st, 9);
    t.error = (const char*)sqlite3_column_text(st, 12);

    auto readParts = [](const nlohmann::json& arr) {
        std::vector<Part> out;
        if (arr.is_array()) for (const auto& pj : arr) {
            Part p;
            if (pj.contains("text")) p.text = pj["text"].get<std::string>();
            if (pj.contains("raw")) p.raw_b64 = pj["raw"].get<std::string>();
            if (pj.contains("url")) p.url = pj["url"].get<std::string>();
            if (pj.contains("data")) p.data = pj["data"].is_string() ? pj["data"].get<std::string>() : pj["data"].dump();
            p.media_type = pj.value("mediaType", "");
            p.filename = pj.value("filename", "");
            out.push_back(p);
        }
        return out;
    };

    const char* msgs = (const char*)sqlite3_column_text(st, 10);
    try {
        auto mj = nlohmann::json::parse(msgs ? msgs : "[]");
        if (mj.is_array()) for (const auto& mm : mj) {
            Message m;
            m.message_id = mm.value("messageId", "");
            m.role = mm.value("role", "user");
            m.context_id = mm.value("contextId", "");
            m.task_id = mm.value("taskId", "");
            m.timestamp = mm.value("timestamp", "");
            if (mm.contains("parts")) m.parts = readParts(mm["parts"]);
            t.messages.push_back(std::move(m));
        }
    } catch (...) {}
    const char* arts = (const char*)sqlite3_column_text(st, 11);
    try {
        auto aj = nlohmann::json::parse(arts ? arts : "[]");
        if (aj.is_array()) for (const auto& aa : aj) {
            Artifact a;
            a.artifact_id = aa.value("artifactId", "");
            a.name = aa.value("name", "");
            a.description = aa.value("description", "");
            if (aa.contains("parts")) a.parts = readParts(aa["parts"]);
            t.artifacts.push_back(std::move(a));
        }
    } catch (...) {}
    return t;
}

void Database::insertTask(const Task& task) {
    std::lock_guard lk(mutex_);
    if (!impl_->db) return;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(impl_->db,
        "INSERT OR REPLACE INTO tasks(id,agent,context_id,state,state_message,title,cwd,created_at,updated_at,last_state_change,messages,artifacts,error) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)",
        -1, &st, nullptr) != SQLITE_OK) return;
    bindTask(st, task);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

void Database::updateTask(const Task& task) {
    insertTask(task);  // INSERT OR REPLACE covers both
}

std::optional<Task> Database::getTask(const std::string& id) {
    std::lock_guard lk(mutex_);
    if (!impl_->db) return std::nullopt;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(impl_->db,
        "SELECT id,agent,context_id,state,state_message,title,cwd,created_at,updated_at,last_state_change,messages,artifacts,error FROM tasks WHERE id=?",
        -1, &st, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_text(st, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
        Task t = taskFromRow(st);
        sqlite3_finalize(st);
        return t;
    }
    sqlite3_finalize(st);
    return std::nullopt;
}

std::vector<Task> Database::listTasks(bool include_terminal) {
    std::lock_guard lk(mutex_);
    std::vector<Task> out;
    if (!impl_->db) return out;
    std::string sql = "SELECT id,agent,context_id,state,state_message,title,cwd,created_at,updated_at,last_state_change,messages,artifacts,error FROM tasks";
    if (!include_terminal) sql += " WHERE state NOT IN ('COMPLETED','FAILED','CANCELED','REJECTED')";
    sql += " ORDER BY updated_at DESC";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(impl_->db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return out;
    while (sqlite3_step(st) == SQLITE_ROW) out.push_back(taskFromRow(st));
    sqlite3_finalize(st);
    return out;
}

std::vector<Task> Database::listNonTerminalTasks() { return listTasks(false); }

void Database::dropTask(const std::string& id) {
    std::lock_guard lk(mutex_);
    if (!impl_->db) return;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(impl_->db, "DELETE FROM tasks WHERE id=?", -1, &st, nullptr) != SQLITE_OK) return;
    sqlite3_bind_text(st, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

}  // namespace kitty_a2a
