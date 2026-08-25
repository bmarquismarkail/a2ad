#include "kitty_a2a/types.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

namespace kitty_a2a {

namespace {
std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}
}  // namespace

std::string_view to_string(TaskState state) {
    switch (state) {
        case TaskState::Submitted:     return "SUBMITTED";
        case TaskState::Working:       return "WORKING";
        case TaskState::InputRequired: return "INPUT_REQUIRED";
        case TaskState::AuthRequired:  return "AUTH_REQUIRED";
        case TaskState::Completed:     return "COMPLETED";
        case TaskState::Failed:        return "FAILED";
        case TaskState::Canceled:      return "CANCELED";
        case TaskState::Rejected:      return "REJECTED";
    }
    return "UNKNOWN";
}

std::string to_state_string(TaskState state) { return std::string(to_string(state)); }

bool is_terminal(TaskState state) {
    return state == TaskState::Completed || state == TaskState::Failed ||
           state == TaskState::Canceled || state == TaskState::Rejected;
}

bool is_interrupted(TaskState state) {
    return state == TaskState::InputRequired || state == TaskState::AuthRequired;
}

// A2A 1.0 wire names are ProtoJSON enum names: "TASK_STATE_" + SCREAMING_SNAKE.
std::string task_state_wire_name(TaskState state) {
    return std::string("TASK_STATE_") + std::string(to_string(state));
}

std::optional<TaskState> parse_task_state(std::string_view s) {
    // Internal (bare) names, case-insensitive.
    std::string v = lower(std::string(s));
    if (v == "submitted") return TaskState::Submitted;
    if (v == "working") return TaskState::Working;
    if (v == "input_required" || v == "input-required" || v == "inputrequired")
        return TaskState::InputRequired;
    if (v == "auth_required" || v == "auth-required" || v == "authrequired")
        return TaskState::AuthRequired;
    if (v == "completed") return TaskState::Completed;
    if (v == "failed") return TaskState::Failed;
    if (v == "canceled" || v == "cancelled") return TaskState::Canceled;
    if (v == "rejected") return TaskState::Rejected;
    return std::nullopt;
}

std::optional<TaskState> parse_task_state_wire(std::string_view s) {
    // A2A 1.0: "TASK_STATE_WORKING" etc. Tolerate:
    //   * the bare "WORKING"
    //   * legacy v0.x kebab-case "input-required" / "auth-required"
    //   * the bare kebab form without the TASK_STATE_ prefix.
    std::string v = std::string(s);
    // Strip a leading "TASK_STATE_" prefix if present.
    const std::string prefix = "TASK_STATE_";
    if (v.rfind(prefix, 0) == 0) v.erase(0, prefix.size());
    return parse_task_state(v);
}

}  // namespace kitty_a2a
