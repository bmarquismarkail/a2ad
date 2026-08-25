#pragma once

#include <string>
#include <vector>

#include "kitty_a2a/Artifact.hpp"
#include "kitty_a2a/types.hpp"

namespace kitty_a2a {

// An A2A `Message`: one turn of the user<->agent conversation.
struct Message {
    std::string message_id;
    std::string role;              // "user" | "agent" (ROLE_USER / ROLE_AGENT on the wire)
    std::vector<Part> parts;
    TaskId task_id;
    ContextId context_id;
    std::vector<std::string> reference_task_ids;
    std::string timestamp;         // ISO-8601 UTC, if the server supplied one

    // The concatenated text of this message's text/data parts — what a human
    // wants to see in a task view.
    std::string text() const;
};

// The daemon's task record. This is the internal model described in DESIGN.md
// §8. It is *not* a terminal-session model (Invariant 5): state is derived from
// A2A semantics only.
//
// Ownership/threading note: instances are only mutated on the daemon's single
// worker thread and read across IPC requests under TaskManager's mutex. No
// instance is shared across threads by pointer without that lock.
struct Task {
    TaskId id;
    AgentId agent;
    ContextId context;

    TaskState state = TaskState::Submitted;
    std::string state_message;     // human text accompanying the state

    std::string title;             // short human label (from the request or an artifact)
    std::string cwd;               // working directory the request was associated with

    std::string created_at;        // ISO-8601 UTC
    std::string updated_at;        // ISO-8601 UTC
    std::string last_state_change; // ISO-8601 UTC

    std::vector<Message> messages; // conversation history
    std::vector<Artifact> artifacts;

    // Most recent error detail, when state == Failed or the last update failed.
    std::string error;
};

}  // namespace kitty_a2a
