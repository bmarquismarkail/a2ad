#include "test_framework.hpp"
#include "kitty_a2a/types.hpp"
#include <string>

using namespace kitty_a2a;

ADD_TEST(state_roundtrip) {
    struct Case { TaskState s; const char* internal; const char* wire; };
    Case cases[] = {
        {TaskState::Submitted,     "SUBMITTED",      "TASK_STATE_SUBMITTED"},
        {TaskState::Working,       "WORKING",        "TASK_STATE_WORKING"},
        {TaskState::InputRequired, "INPUT_REQUIRED", "TASK_STATE_INPUT_REQUIRED"},
        {TaskState::AuthRequired,  "AUTH_REQUIRED",  "TASK_STATE_AUTH_REQUIRED"},
        {TaskState::Completed,     "COMPLETED",      "TASK_STATE_COMPLETED"},
        {TaskState::Failed,        "FAILED",         "TASK_STATE_FAILED"},
        {TaskState::Canceled,      "CANCELED",       "TASK_STATE_CANCELED"},
        {TaskState::Rejected,      "REJECTED",       "TASK_STATE_REJECTED"},
    };
    for (auto& c : cases) {
        CHECK_EQ(to_state_string(c.s), c.internal);
        CHECK_EQ(task_state_wire_name(c.s), c.wire);
        CHECK(parse_task_state_wire(c.wire).has_value());
        if (parse_task_state_wire(c.wire)) CHECK_EQ(*parse_task_state_wire(c.wire), c.s);
    }
}

ADD_TEST(interrupted_and_terminal) {
    CHECK(is_interrupted(TaskState::InputRequired));
    CHECK(is_interrupted(TaskState::AuthRequired));
    CHECK(!is_interrupted(TaskState::Working));
    CHECK(!is_interrupted(TaskState::Submitted));

    CHECK(is_terminal(TaskState::Completed));
    CHECK(is_terminal(TaskState::Failed));
    CHECK(is_terminal(TaskState::Canceled));
    CHECK(is_terminal(TaskState::Rejected));
    CHECK(!is_terminal(TaskState::Working));
    CHECK(!is_terminal(TaskState::InputRequired));
}

ADD_TEST(legacy_wire_parsing) {
    CHECK(parse_task_state_wire("input-required").has_value());
    if (parse_task_state_wire("input-required"))
        CHECK_EQ(*parse_task_state_wire("input-required"), TaskState::InputRequired);
    CHECK(parse_task_state_wire("auth-required").has_value());
    CHECK(parse_task_state_wire("working").has_value());
    CHECK(!parse_task_state_wire("not-a-state").has_value());
}
