#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace kitty_a2a {

// A2A 1.0 lifecycle states.
//
// Upstream A2A 1.0 serializes these as ProtoJSON enum names in
// SCREAMING_SNAKE_CASE, e.g. "TASK_STATE_WORKING". The design document
// (DESIGN.md §8) uses the bare names ("WORKING"). We keep the design's short
// names as the daemon's internal enum and translate to/from the wire form in
// the A2AClient. This keeps the internal model stable even if the upstream
// naming changes again.
//
// Note: per A2A 1.0, INPUT_REQUIRED and AUTH_REQUIRED are *interrupted*
// (non-terminal) states; a task returns to WORKING after the client responds.
// COMPLETED / FAILED / CANCELED / REJECTED are terminal.
enum class TaskState {
    Submitted,
    Working,
    InputRequired,
    AuthRequired,
    Completed,
    Failed,
    Canceled,
    Rejected,
};

// Human-readable, stable, wire-independent name for a TaskState.
std::string_view to_string(TaskState state);
std::string to_state_string(TaskState state);   // allocates
std::optional<TaskState> parse_task_state(std::string_view s);

// True for COMPLETED / FAILED / CANCELED / REJECTED.
bool is_terminal(TaskState state);
// True for INPUT_REQUIRED / AUTH_REQUIRED.
bool is_interrupted(TaskState state);

// The SCREAMING_SNAKE_CASE enum name used on the A2A 1.0 wire
// (e.g. "TASK_STATE_WORKING"). Returns "" if unknown.
std::string task_state_wire_name(TaskState state);
// Parse the A2A 1.0 wire name ("TASK_STATE_WORKING", and tolerate legacy
// kebab-case "working" / "input-required" from pre-1.0 servers).
std::optional<TaskState> parse_task_state_wire(std::string_view s);

// Strongly-typed identifiers. These are thin wrappers over std::string so the
// daemon can pass Task/Agent/Context identifiers around without mixing them up
// at call sites, per DESIGN.md §37 ("strongly typed task and agent
// identifiers"). They remain trivially serializable to/from the database and
// IPC layer.
template <typename Tag>
class TypedId {
public:
    TypedId() = default;
    // From a string literal / string / string_view. Explicit so only intentional
    // conversions work. A single constructor avoids the classic ambiguity where a
    // `const char*` literal could convert to both `std::string` and
    // `std::string_view`. Call sites write TaskId("...") directly.
    explicit TypedId(std::string_view v) : value_(v) {}

    bool empty() const { return value_.empty(); }
    const std::string& value() const { return value_; }
    explicit operator bool() const { return !value_.empty(); }

    bool operator==(const TypedId& other) const { return value_ == other.value_; }
    bool operator!=(const TypedId& other) const { return !(*this == other); }
    bool operator<(const TypedId& other) const { return value_ < other.value_; }

    // Convenience: assign from a string (used by persistence layer).
    TypedId& operator=(const std::string& v) { value_ = v; return *this; }
    TypedId& operator=(const char* v) { value_ = v ? v : ""; return *this; }
    TypedId& operator=(std::string_view v) { value_ = std::string(v); return *this; }

private:
    std::string value_;
};

// Tag types (empty structs used only to distinguish the id kinds).
struct AgentIdTag {};
struct TaskIdTag {};
struct ContextIdTag {};

using AgentId = TypedId<AgentIdTag>;
using TaskId = TypedId<TaskIdTag>;
using ContextId = TypedId<ContextIdTag>;

}  // namespace kitty_a2a
