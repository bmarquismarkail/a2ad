#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <nlohmann/json.hpp>

struct sqlite3;

namespace kitty_a2a {

// Local owner control plane. Never expose this API to worker sandboxes.
// One transaction covers each state transition and its audit event. Network
// dispatch runs outside that transaction; uncertain outcomes are never retried.
class ControlPlane {
public:
    using Json = nlohmann::json;
    using Dispatch = std::function<Json(const Json&)>;
    explicit ControlPlane(const std::string& database_path, bool enforce = false);
    ~ControlPlane();
    ControlPlane(const ControlPlane&) = delete;
    ControlPlane& operator=(const ControlPlane&) = delete;
    Json handle(const Json& request, const Dispatch& dispatch);
    void event(const Json& event);
    Json health(const std::string& agent);
    static std::string digest(const std::string& bytes);
    static std::string newId();

private:
    Json read(const std::string& kind, const std::string& id, sqlite3* db = nullptr);
    void write(const std::string& kind, const std::string& id, const Json& value);
    Json list(const std::string& kind, sqlite3* db = nullptr);
    Json readOnly(const Json& request);
    void append(const Json& value);
    Json control(const Json& request);
    Json begin(const Json& request);
    void finish(const std::string& id, const Json& result);
    sqlite3* db_ = nullptr;
    sqlite3* read_db_ = nullptr;
    bool enforce_;
    std::recursive_mutex mutex_;
};

} // namespace kitty_a2a
