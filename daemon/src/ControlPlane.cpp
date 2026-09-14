#include "kitty_a2a/ControlPlane.hpp"

#include <sqlite3.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>

namespace kitty_a2a {
namespace {
using J = nlohmann::json;
int64_t now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
void sql(sqlite3* db, const char* query) {
    if (sqlite3_exec(db, query, nullptr, nullptr, nullptr) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(db));
}
struct Statement {
    sqlite3* db;
    sqlite3_stmt* st = nullptr;
    Statement(sqlite3* d, const char* query) : db(d) {
        if (sqlite3_prepare_v2(db, query, -1, &st, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db));
    }
    ~Statement() { sqlite3_finalize(st); }
    void bind(int index, const std::string& text) {
        if (sqlite3_bind_text(st, index, text.data(), static_cast<int>(text.size()), SQLITE_TRANSIENT) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db));
    }
    bool row() {
        int r = sqlite3_step(st);
        if (r != SQLITE_ROW && r != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db));
        return r == SQLITE_ROW;
    }
    std::string text(int col) {
        const auto* p = sqlite3_column_text(st, col);
        return p ? reinterpret_cast<const char*>(p) : "";
    }
};
struct Transaction {
    sqlite3* db;
    bool committed = false;
    explicit Transaction(sqlite3* d) : db(d) { sql(db, "BEGIN IMMEDIATE"); }
    ~Transaction() { if (!committed) sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr); }
    void commit() { sql(db, "COMMIT"); committed = true; }
};
std::string required(const J& j, const char* key) {
    auto value = j.value(key, "");
    if (value.empty() || value.size() > 256) throw std::invalid_argument(std::string(key) + " must contain 1..256 bytes");
    return value;
}
J failure(const std::string& kind, const std::string& error) {
    return {{"ok", false}, {"error_kind", kind}, {"error", error}};
}
const std::set<std::string> effects = {"READ_REMOTE", "WRITE_REMOTE", "EXECUTE_REMOTE", "PUBLISH",
    "CREATE_IDENTITY", "STORE_EXTERNAL", "SEND_MESSAGE", "DELEGATE"};
std::set<std::string> effectSet(const J& j) {
    if (!j.is_array()) throw std::invalid_argument("effects must be an array");
    std::set<std::string> out;
    for (const auto& e : j) {
        auto name = e.get<std::string>();
        if (!effects.contains(name)) throw std::invalid_argument("unknown effect: " + name);
        out.insert(name);
    }
    return out;
}
bool mutation(const std::string& op) {
    return op == "submit" || op == "respond" || op == "cancel" || op == "stream_submit" ||
        op == "push.create" || op == "push.delete" || op == "artifact.materialize";
}
J canonical(J request) {
    request.erase("request_id");
    return request;
}
}

std::string ControlPlane::digest(const std::string& bytes) {
    unsigned char hash[EVP_MAX_MD_SIZE]; unsigned int size = 0;
    if (EVP_Digest(bytes.data(), bytes.size(), hash, &size, EVP_sha256(), nullptr) != 1)
        throw std::runtime_error("SHA-256 failed");
    std::ostringstream out;
    for (unsigned int i = 0; i < size; ++i) out << std::hex << std::setfill('0') << std::setw(2) << int(hash[i]);
    return out.str();
}
std::string ControlPlane::newId() {
    unsigned char bytes[16];
    if (RAND_bytes(bytes, sizeof(bytes)) != 1) throw std::runtime_error("secure random generation failed");
    std::ostringstream out;
    for (auto byte : bytes) out << std::hex << std::setfill('0') << std::setw(2) << int(byte);
    return out.str();
}
ControlPlane::ControlPlane(const std::string& path, bool enforce) : enforce_(enforce) {
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        std::string error = sqlite3_errmsg(db_); sqlite3_close(db_); db_ = nullptr;
        throw std::runtime_error(error);
    }
    try {
        sqlite3_busy_timeout(db_, 5000);
        sql(db_, "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;"
            "CREATE TABLE IF NOT EXISTS control_records(kind TEXT NOT NULL,id TEXT NOT NULL,value TEXT NOT NULL,PRIMARY KEY(kind,id));"
            "CREATE TABLE IF NOT EXISTS control_events(sequence INTEGER PRIMARY KEY AUTOINCREMENT,time INTEGER NOT NULL,value TEXT NOT NULL,previous_hash TEXT NOT NULL,hash TEXT NOT NULL);"
            "CREATE TRIGGER IF NOT EXISTS control_events_no_update BEFORE UPDATE ON control_events BEGIN SELECT RAISE(ABORT,'audit is append-only'); END;"
            "CREATE TRIGGER IF NOT EXISTS control_events_no_delete BEFORE DELETE ON control_events BEGIN SELECT RAISE(ABORT,'audit is append-only'); END;");
        if (sqlite3_open_v2(path.c_str(), &read_db_, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
            sqlite3_close(read_db_); read_db_ = nullptr;
            throw std::runtime_error("cannot open read-only control-plane connection");
        }
        sqlite3_busy_timeout(read_db_, 5000);
        Transaction tx(db_);
        for (auto execution : list("execution", db_)) {
            if (execution.value("state", "") == "DISPATCHING") {
                execution["state"] = "UNCERTAIN";
                execution["reason"] = "daemon restarted during dispatch; reconcile remote state before any new attempt";
                write("execution", execution.at("id"), execution);
                append({{"type", "execution.uncertain"}, {"execution_id", execution.at("id")}});
            }
        }
        tx.commit();
    } catch (...) { sqlite3_close(read_db_); read_db_ = nullptr; sqlite3_close(db_); db_ = nullptr; throw; }
}
ControlPlane::~ControlPlane() { sqlite3_close(read_db_); sqlite3_close(db_); }
ControlPlane::Json ControlPlane::read(const std::string& kind, const std::string& id, sqlite3* database) {
    sqlite3* db = database ? database : db_;
    Statement st(db, "SELECT value FROM control_records WHERE kind=? AND id=?");
    st.bind(1, kind); st.bind(2, id);
    return st.row() ? J::parse(st.text(0)) : J();
}
void ControlPlane::write(const std::string& kind, const std::string& id, const J& value) {
    Statement st(db_, "INSERT INTO control_records VALUES(?,?,?) ON CONFLICT(kind,id) DO UPDATE SET value=excluded.value");
    st.bind(1, kind); st.bind(2, id); st.bind(3, value.dump()); st.row();
}
ControlPlane::Json ControlPlane::list(const std::string& kind, sqlite3* database) {
    sqlite3* db = database ? database : db_;
    Statement st(db, "SELECT value FROM control_records WHERE kind=? ORDER BY id"); st.bind(1, kind);
    J values = J::array(); while (st.row()) values.push_back(J::parse(st.text(0))); return values;
}
void ControlPlane::append(const J& value) {
    Statement last(db_, "SELECT hash FROM control_events ORDER BY sequence DESC LIMIT 1");
    std::string previous = last.row() ? last.text(0) : "";
    auto timestamp = now(); std::string body = value.dump();
    auto hash = digest(previous + "\n" + std::to_string(timestamp) + "\n" + body);
    Statement st(db_, "INSERT INTO control_events(time,value,previous_hash,hash) VALUES(?,?,?,?)");
    sqlite3_bind_int64(st.st, 1, timestamp); st.bind(2, body); st.bind(3, previous); st.bind(4, hash); st.row();
}
void ControlPlane::event(const J& value) {
    std::lock_guard lock(mutex_); Transaction tx(db_); append(value); tx.commit();
}
ControlPlane::Json ControlPlane::health(const std::string& agent) {
    std::lock_guard lock(mutex_); auto h = read("health", agent);
    return h.is_null() ? J{{"agent", agent}, {"failures", 0}, {"quarantined", false}} : h;
}

ControlPlane::Json ControlPlane::control(const J& req) {
    const auto op = req.value("op", "");
    if (op == "policy.info") return {{"ok", true}, {"enforce", enforce_}, {"effects", effects},
        {"authority", "local socket owner; worker isolation must exclude this socket and database"}};
    if (op == "execution.list" || op == "session.list" || op == "approval.list" || op == "provenance.list" || op == "continuation.list") {
        auto kind = op.substr(0, op.find('.'));
        auto items = list(kind);
        if (kind == "execution") {
            J filtered = J::array();
            for (const auto& item : items) {
                if (req.contains("session_id") && item.value("session_id", "") != req.at("session_id").get<std::string>()) continue;
                if (req.contains("task_id") && item.value("task_id", "") != req.at("task_id").get<std::string>()) continue;
                filtered.push_back(item);
            }
            items = std::move(filtered);
        }
        return {{"ok", true}, {"items", items}};
    }
    if (op == "execution.get" || op == "session.get") {
        auto item = read(op.substr(0, op.find('.')), required(req, "id"));
        if (item.is_null()) return failure("not_found", "record not found");
        return {{"ok", true}, {"record", item}};
    }
    if (op == "continuation.create") {
        auto task = required(req, "task_id");
        auto kind = required(req, "kind");
        if (kind != "INPUT_REQUIRED" && kind != "APPROVAL" && kind != "RETRY" && kind != "HANDOFF")
            return failure("invalid_request", "kind must be INPUT_REQUIRED, APPROVAL, RETRY, or HANDOFF");
        auto id = newId();
        J continuation = {{"id", id}, {"task_id", task}, {"kind", kind}, {"state", "PENDING"},
            {"reason", req.value("reason", "")}, {"created_at", now()}};
        write("continuation", id, continuation);
        append({{"type", "continuation.required"}, {"continuation_id", id}, {"task_id", task}, {"kind", kind}});
        return {{"ok", true}, {"continuation", continuation}};
    }
    if (op == "session.create") {
        const auto id = req.contains("id") ? required(req, "id") : newId();
        if (!read("session", id).is_null()) return failure("conflict", "session already exists");
        J session = {{"id", id}, {"created_at", now()}, {"label", req.value("label", "")}};
        write("session", id, session); append({{"type", "session.created"}, {"session_id", id}});
        return {{"ok", true}, {"session", session}};
    }
    if (op == "lease.acquire" || op == "lease.renew" || op == "lease.release") {
        auto resource = required(req, "resource");
        auto owner = required(req, "owner");
        auto lease = read("lease", resource);
        int ttl = req.value("ttl_seconds", 60);
        if (ttl < 1 || ttl > 3600) return failure("invalid_request", "ttl_seconds must be 1..3600");
        if (op == "lease.acquire") {
            if (!lease.is_null() && lease.value("expires_at", int64_t{0}) > now())
                return failure("lease_busy", "resource already has a live lease");
            auto fence = lease.is_null() ? int64_t{1} : lease.at("fence").get<int64_t>() + 1;
            lease = {{"resource", resource}, {"owner", owner}, {"token", newId()}, {"fence", fence}, {"expires_at", now() + ttl}};
        } else {
            if (lease.is_null() || lease.value("expires_at", int64_t{0}) <= now() || lease.at("owner") != owner ||
                lease.at("token") != required(req, "token"))
                return failure("lease_lost", "lease expired or owner/token mismatch");
            lease["expires_at"] = op == "lease.release" ? now() : now() + ttl;
        }
        write("lease", resource, lease);
        append({{"type", op}, {"resource", resource}, {"owner", owner}, {"fence", lease["fence"]}});
        return {{"ok", true}, {"lease", lease}};
    }
    if (op == "approval.revoke") {
        auto id = required(req, "id"); auto approval = read("approval", id);
        if (approval.is_null()) return failure("not_found", "approval not found");
        approval["state"] = "REVOKED";
        write("approval", id, approval); append({{"type", op}, {"request_id", id}});
        return {{"ok", true}, {"approval", approval}};
    }
    if (op == "approval.decide") {
        const auto id = required(req, "id"); auto approval = read("approval", id);
        if (approval.is_null()) return failure("not_found", "approval not found");
        if (approval["state"] != "PENDING") return failure("conflict", "approval already decided");
        if (!req.contains("allow") || !req["allow"].is_boolean()) return failure("invalid_request", "allow must be boolean");
        approval["state"] = req["allow"].get<bool>() ? "APPROVED" : "DENIED";
        int ttl = req.value("ttl_seconds", 300);
        if (ttl < 1 || ttl > 3600) return failure("invalid_request", "ttl_seconds must be 1..3600");
        approval["expires_at"] = now() + ttl;
        approval["reason"] = req.value("reason", "");
        write("approval", id, approval);
        append({{"type", "approval.decided"}, {"request_id", id}, {"state", approval["state"]}, {"expires_at", approval["expires_at"]}});
        return {{"ok", true}, {"approval", approval}};
    }
    if (op == "agent.health" || op == "agent.quarantine" || op == "agent.release") {
        auto agent = required(req, "agent"); auto h = health(agent);
        if (op != "agent.health") {
            h["quarantined"] = op == "agent.quarantine";
            h["reason"] = req.value("reason", "owner decision");
            if (op == "agent.release") h["failures"] = 0;
            write("health", agent, h); append({{"type", op}, {"agent", agent}, {"reason", h["reason"]}});
        }
        return {{"ok", true}, {"health", h}};
    }
    if (op == "provenance.record") {
        auto agent = required(req, "agent");
        auto sha = required(req, "sha256");
        if (sha.size() != 64 || sha.find_first_not_of("0123456789abcdef") != std::string::npos)
            return failure("invalid_request", "sha256 must be 64 lowercase hexadecimal characters");
        J record = {{"agent", agent}, {"sha256", sha}, {"source", required(req, "source")},
            {"version", req.value("version", "")}, {"sbom_uri", req.value("sbom_uri", "")}, {"recorded_at", now()},
            {"trust", "owner-supplied inventory; not remote attestation"}};
        write("provenance", agent, record); append({{"type", op}, {"agent", agent}, {"sha256", sha}});
        return {{"ok", true}, {"record", record}};
    }
    if (op == "channel.open") {
        auto id = required(req, "id"); auto c = read("channel", id);
        if (c.is_null()) {
            auto after = req.value("after", int64_t{0});
            if (after < 0) return failure("invalid_request", "after must be nonnegative");
            c = {{"id", id}, {"cursor", after}, {"delivered", after}};
            write("channel", id, c);
        }
        return {{"ok", true}, {"channel", c}};
    }
    if (op == "channel.read" || op == "events.read") {
        auto after = req.value("after", int64_t{0}); J channel;
        if (op == "channel.read") {
            channel = read("channel", required(req, "id"));
            if (channel.is_null()) return failure("not_found", "open channel first");
            after = channel.at("cursor");
        }
        const int limit = req.value("limit", 100);
        if (after < 0 || limit < 1 || limit > 500) return failure("invalid_request", "after >= 0 and limit 1..500 required");
        Statement st(db_, "SELECT sequence,time,value,previous_hash,hash FROM control_events WHERE sequence>? ORDER BY sequence LIMIT ?");
        sqlite3_bind_int64(st.st, 1, after); sqlite3_bind_int(st.st, 2, limit);
        J rows = J::array(); auto next = after;
        while (st.row()) {
            next = sqlite3_column_int64(st.st, 0);
            rows.push_back({{"sequence", next}, {"time", sqlite3_column_int64(st.st, 1)},
                {"event", J::parse(st.text(2))}, {"previous_hash", st.text(3)}, {"hash", st.text(4)}});
        }
        if (!channel.is_null()) { channel["delivered"] = std::max(channel.at("delivered").get<int64_t>(), next); write("channel", channel.at("id"), channel); }
        return {{"ok", true}, {"events", rows}, {"next_cursor", next}, {"may_have_more", rows.size() == static_cast<size_t>(limit)}};
    }
    if (op == "channel.ack") {
        auto id = required(req, "id"); auto c = read("channel", id);
        if (c.is_null()) return failure("not_found", "channel not found");
        auto cursor = req.at("cursor").get<int64_t>();
        if (cursor < c.at("cursor").get<int64_t>() || cursor > c.at("delivered").get<int64_t>())
            return failure("invalid_cursor", "ack must be monotonic and cannot exceed delivered cursor");
        c["cursor"] = cursor; write("channel", id, c); return {{"ok", true}, {"channel", c}};
    }
    if (op == "audit.verify") {
        Statement st(db_, "SELECT time,value,previous_hash,hash FROM control_events ORDER BY sequence");
        std::string previous; int64_t count = 0;
        while (st.row()) {
            auto expected = digest(previous + "\n" + std::to_string(sqlite3_column_int64(st.st, 0)) + "\n" + st.text(1));
            if (previous != st.text(2) || expected != st.text(3)) return failure("integrity_failure", "audit chain mismatch");
            previous = st.text(3); ++count;
        }
        return {{"ok", true}, {"count", count}, {"head_sha256", previous}};
    }
    return J();
}

ControlPlane::Json ControlPlane::readOnly(const J& req) {
    const auto op = req.value("op", "");
    const bool list_op = op == "execution.list" || op == "session.list" || op == "approval.list" ||
        op == "provenance.list" || op == "continuation.list";
    if (op == "policy.info") return { {"ok", true}, {"enforce", enforce_}, {"effects", effects},
        {"authority", "local socket owner; worker isolation must exclude this socket and database"} };
    if (list_op) {
        const auto kind = op.substr(0, op.find('.'));
        const bool paged = req.contains("limit") || req.contains("after");
        if (!paged) {
            auto items = list(kind, read_db_);
            if (kind == "execution") {
                J filtered = J::array();
                for (const auto& item : items) {
                    if (req.contains("session_id") && item.value("session_id", "") != req.at("session_id").get<std::string>()) continue;
                    if (req.contains("task_id") && item.value("task_id", "") != req.at("task_id").get<std::string>()) continue;
                    filtered.push_back(item);
                }
                items = std::move(filtered);
            }
            return {{"ok", true}, {"items", items}};
        }
        if ((req.contains("after") && !req.at("after").is_string()) ||
            (req.contains("limit") && (!req.at("limit").is_number_integer() || req.at("limit").get<int>() < 1 || req.at("limit").get<int>() > 500)))
            return failure("invalid_request", "after must be a string and limit must be 1..500");
        const std::string after = req.value("after", "");
        const int limit = req.value("limit", 100);
        Statement st(read_db_, "SELECT id,value FROM control_records WHERE kind=? AND id>? ORDER BY id LIMIT ?");
        st.bind(1, kind); st.bind(2, after); sqlite3_bind_int(st.st, 3, limit + 1);
        J items = J::array(); std::string next; bool more = false;
        while (st.row()) {
            auto id = st.text(0);
            if (items.size() == static_cast<size_t>(limit)) { more = true; break; }
            auto item = J::parse(st.text(1));
            if (kind == "execution") {
                if (req.contains("session_id") && item.value("session_id", "") != req.at("session_id").get<std::string>()) continue;
                if (req.contains("task_id") && item.value("task_id", "") != req.at("task_id").get<std::string>()) continue;
            }
            items.push_back(std::move(item));
            next = id;
        }
        J result = {{"ok", true}, {"items", items}};
        if (more) result["next_cursor"] = next;
        return result;
    }
    if (op == "execution.get" || op == "session.get") {
        auto item = read(op.substr(0, op.find('.')), required(req, "id"), read_db_);
        if (item.is_null()) return failure("not_found", "record not found");
        return {{"ok", true}, {"record", item}};
    }
    if (op == "audit.verify") {
        Statement st(read_db_, "SELECT time,value,previous_hash,hash FROM control_events ORDER BY sequence");
        std::string previous; int64_t count = 0;
        while (st.row()) {
            auto expected = digest(previous + "\n" + std::to_string(sqlite3_column_int64(st.st, 0)) + "\n" + st.text(1));
            if (previous != st.text(2) || expected != st.text(3)) return failure("integrity_failure", "audit chain mismatch");
            previous = st.text(3); ++count;
        }
        return {{"ok", true}, {"count", count}, {"head_sha256", previous}};
    }
    return J();
}

ControlPlane::Json ControlPlane::begin(const J& req) {
    const auto id = required(req, "request_id"); const auto hash = digest(canonical(req).dump());
    auto prior = read("execution", id);
    if (!prior.is_null()) {
        if (prior.at("request_sha256") != hash) return failure("idempotency_conflict", "request_id was used with different parameters");
        if (prior.contains("result")) return prior["result"];
        auto result = failure("outcome_uncertain", "operation is in flight or its outcome is unknown; inspect execution and reconcile, do not resubmit");
        result["execution_id"] = id; return result;
    }
    if (req.contains("lease")) {
        const auto& supplied = req.at("lease");
        auto lease = read("lease", required(supplied, "resource"));
        if (lease.is_null() || lease.value("expires_at", int64_t{0}) <= now() ||
            lease.at("owner") != required(supplied, "owner") || lease.at("token") != required(supplied, "token") ||
            lease.at("fence") != supplied.at("fence"))
            return failure("lease_lost", "dispatch requires a live matching lease and fencing number");
    }
    auto continuation_id = req.value("continuation_id", "");
    if (!continuation_id.empty()) {
        auto continuation = read("continuation", continuation_id);
        if (continuation.is_null() || continuation.value("state", "") != "PENDING" ||
            continuation.value("task_id", "") != req.value("task_id", ""))
            return failure("invalid_continuation", "continuation must be pending and bound to this task");
    }
    auto retry_of = req.value("retry_of", "");
    if (!retry_of.empty()) {
        auto previous = read("execution", retry_of);
        if (previous.is_null() || previous.value("state", "") != "FAILED")
            return failure("invalid_retry", "only a definitively failed dispatch may be retried; uncertain dispatches require remote reconciliation");
    }
    auto agent = req.value("agent", "");
    if (!agent.empty() && health(agent).value("quarantined", false)) return failure("quarantined", "agent is quarantined");
    auto requested = effectSet(req.value("effects", J::array()));
    const auto op = req.value("op", "");
    if (op == "submit" || op == "respond" || op == "stream_submit") requested.insert("EXECUTE_REMOTE");
    if (op == "cancel" || op == "push.create" || op == "push.delete") requested.insert("WRITE_REMOTE");
    if (op == "artifact.materialize") requested.insert("STORE_EXTERNAL");
    auto parent_id = req.value("parent_execution_id", "");
    J chain = J::array();
    if (!parent_id.empty()) {
        auto parent = read("execution", parent_id);
        if (parent.is_null()) return failure("invalid_delegation", "parent execution not found");
        auto allowed = effectSet(parent.at("effects"));
        if (!allowed.contains("DELEGATE") || !std::includes(allowed.begin(), allowed.end(), requested.begin(), requested.end()))
            return failure("permission_denied", "child effects exceed parent grant or DELEGATE is absent");
        chain = parent.at("identity_chain");
        for (const auto& ancestor : chain) {
            auto grant = read("approval", ancestor.at("execution_id"));
            if (grant.is_null() || grant.value("state", "") != "APPROVED" || grant.value("expires_at", int64_t{0}) <= now())
                return failure("permission_denied", "ancestor grant expired or was revoked");
        }
        if (chain.size() >= 16) return failure("invalid_delegation", "maximum delegation depth reached");
    }
    chain.push_back({{"agent", agent}, {"execution_id", id}});
    auto session_id = req.value("session_id", "");
    if (!session_id.empty() && read("session", session_id).is_null()) return failure("not_found", "session not found");
    if (enforce_ || req.contains("effects") || !parent_id.empty()) {
        auto approval = read("approval", id);
        if (approval.is_null()) {
            approval = {{"id", id}, {"state", "PENDING"}, {"request_sha256", hash}, {"agent", agent},
                {"operation", op}, {"effects", requested}, {"created_at", now()}, {"task_id", req.value("task_id", "")}};
            write("approval", id, approval); append({{"type", "approval.required"}, {"request_id", id}, {"effects", requested}, {"agent", agent}});
        }
        if (approval.at("request_sha256") != hash) return failure("idempotency_conflict", "approval is bound to different parameters");
        if (approval["state"] != "APPROVED") {
            auto result = failure(approval["state"] == "DENIED" ? "permission_denied" : "approval_required", "owner approval required for exact request");
            result["approval"] = approval; return result;
        }
        if (approval.at("expires_at").get<int64_t>() <= now()) return failure("grant_expired", "approval expired; create a new request_id for review");
    }
    J execution = {{"id", id}, {"state", "DISPATCHING"}, {"request_sha256", hash}, {"agent", agent},
        {"operation", op}, {"session_id", session_id}, {"effects", requested}, {"identity_chain", chain},
        {"parent_execution_id", parent_id}, {"continuation_id", continuation_id}, {"retry_of", retry_of}, {"started_at", now()}, {"dispatch_deadline_at", now() + 60},
        {"task_id", req.value("task_id", "")}};
    write("execution", id, execution);
    append({{"type", "execution.started"}, {"execution_id", id}, {"agent", agent}, {"session_id", session_id},
        {"request_sha256", hash}, {"effects", requested}, {"identity_chain", chain}});
    return J();
}
void ControlPlane::finish(const std::string& id, const J& result) {
    std::lock_guard lock(mutex_); Transaction tx(db_);
    auto execution = read("execution", id);
    const auto error = result.value("error_kind", "");
    bool uncertain = error == "local_network" || error == "malformed_response" || error == "dispatch_exception";
    execution["state"] = uncertain ? "UNCERTAIN" : result.value("ok", false) ? "DELIVERED" : "FAILED";
    if (!uncertain) execution["result"] = result;
    execution["finished_at"] = now();
    if (result.contains("task_id")) execution["task_id"] = result["task_id"];
    if (result.contains("context_id")) execution["context_id"] = result["context_id"];
    write("execution", id, execution);
    auto continuation_id = execution.value("continuation_id", "");
    if (!continuation_id.empty() && result.value("ok", false)) {
        auto continuation = read("continuation", continuation_id);
        continuation["state"] = "DELIVERED"; continuation["execution_id"] = id;
        write("continuation", continuation_id, continuation);
        append({{"type", "continuation.delivered"}, {"continuation_id", continuation_id}, {"execution_id", id}});
    }
    auto agent = execution.value("agent", "");
    if (!agent.empty()) {
        auto h = health(agent);
        if (uncertain) h["failures"] = h.value("failures", 0) + 1;
        else if (result.value("ok", false)) h["failures"] = 0;
        if (h.value("failures", 0) >= 3) { h["quarantined"] = true; h["reason"] = "three consecutive uncertain dispatches"; }
        write("health", agent, h);
    }
    append({{"type", "execution.finished"}, {"execution_id", id}, {"state", execution["state"]}, {"task_id", execution["task_id"]}});
    tx.commit();
}
ControlPlane::Json ControlPlane::handle(const J& req, const Dispatch& dispatch) {
    try {
        if (!req.is_object()) return failure("invalid_request", "request must be an object");
        const auto op = req.value("op", "");
        if (op == "policy.info" || op == "execution.list" || op == "session.list" || op == "approval.list" ||
            op == "provenance.list" || op == "continuation.list" || op == "execution.get" || op == "session.get" ||
            op == "audit.verify") {
            auto result = readOnly(req);
            if (!result.is_null()) return result;
        }
        {
            std::lock_guard lock(mutex_); Transaction tx(db_);
            auto result = control(req);
            if (!result.is_null()) { tx.commit(); return result; }
            tx.commit();
        }
        if (!mutation(req.value("op", ""))) return dispatch(req);
        J request = req;
        if (!request.contains("request_id")) {
            if (enforce_ || request.contains("effects")) return failure("invalid_request", "request_id required for governed mutations");
            request["request_id"] = newId();
        }
        {
            std::lock_guard lock(mutex_); Transaction tx(db_);
            auto result = begin(request); tx.commit(); if (!result.is_null()) return result;
        }
        J result;
        try { result = dispatch(request); }
        catch (const std::exception&) { result = failure("dispatch_exception", "dispatch raised an exception; reconcile before retrying"); }
        result["execution_id"] = request.at("request_id");
        finish(request.at("request_id"), result);
        return result;
    } catch (const std::exception& e) { return failure("control_error", e.what()); }
}
} // namespace kitty_a2a
