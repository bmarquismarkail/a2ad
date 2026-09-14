#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include <thread>

#include "kitty_a2a/A2AClient.hpp"
#include "kitty_a2a/AgentCard.hpp"
#include "kitty_a2a/Config.hpp"
#include "kitty_a2a/Database.hpp"
#include "kitty_a2a/Task.hpp"
#include "kitty_a2a/types.hpp"

namespace kitty_a2a {

// Context associated with a task request (DESIGN.md §5, §10). Kept minimal:
// the working directory is the primary piece of terminal context we carry into
// an A2A message. More context fields (selection, last output) are attached to
// the message text by the kitten, not modeled here.
struct RequestContext {
    std::string cwd;
};

struct CreateTaskRequest {
    AgentId agent;
    std::string message;
    RequestContext context;
    // Optional explicit continuation: an existing task_id / context_id.
    TaskId continue_task_id;
    ContextId continue_context_id;
};

struct CreateTaskResponse {
    bool ok = false;
    TaskId task_id;
    ContextId context_id;
    TaskState state = TaskState::Submitted;
    std::string message;      // a short human-readable note (success or reason)
    std::string error_kind;   // "" on success, else the A2AResult::ErrorKind name
};

// A summary row for task list views (no full message/artifact payload).
struct TaskSummary {
    TaskId id;
    AgentId agent;
    TaskState state;
    std::string title;
    std::string cwd;
    std::string created_at;
    std::string updated_at;
    std::string state_message;
    bool has_artifacts = false;
};

// Owns the in-memory + persisted task state (DESIGN.md §4.1). This is the
// "persistent local daemon manages agent state" component. The Kitty kitten is
// a thin client of this (Invariant 6).
//
// Threading model: a single worker thread drives A2A submissions and (in
// milestone 2) streaming/polling. Public methods enqueue work and return
// promptly where sensible; synchronous query methods (list/get) take the state
// mutex directly. All Task mutations occur under state_mutex_.
class TaskManager {
public:
    TaskManager(Database& db, std::shared_ptr<A2AClient> a2a, const Config& config);
    ~TaskManager();

    // Load persisted non-terminal tasks and reconcile them with their remote
    // endpoints (best-effort GetTask; on failure keep the last known state and
    // mark a reconciliation note, never a fake completion).
    void reconcileOnStartup();
    // Subscribe to all persisted non-terminal tasks whose agents advertise
    // streaming. Streams reconnect with bounded backoff until shutdown.
    void startSubscriptions();

    // Create + submit a task. This performs the remote SendMessage (blocking the
    // caller briefly) and records the resulting task.
    CreateTaskResponse createTask(const CreateTaskRequest& req);

    // Respond to an INPUT_REQUIRED / AUTH_REQUIRED task by sending a new A2A
    // message in the same task/context.
    CreateTaskResponse respondToTask(const std::string& task_id, const std::string& response_text);

    // Cancel a task (remote CancelTask + local state update).
    A2AResult cancelTask(const std::string& task_id);

    // Synchronous status refresh via GetTask (used by the `status` IPC op).
    A2AResult refreshTask(const std::string& task_id);

    // Queries (synchronous, under the state mutex).
    std::vector<TaskSummary> listTasks(bool include_terminal = true);
    A2AResult listRemoteTasks(const std::string& agent, const std::string& context_id = {},
                              int page_size = 100);
    A2AResult listRemoteTasks(const std::string& agent, const ListTasksFilter& filter);
    A2AResult streamMessage(const std::string& agent, const std::string& message,
                            const TaskId& task_id, const ContextId& context_id,
                            const std::function<void(const nlohmann::json&)>& on_event,
                            std::stop_token stop = {});
    A2AResult subscribeTask(const std::string& local_task_id,
                            const std::function<void(const Task&)>& on_task,
                            std::stop_token stop = {});
    A2AResult createPushConfig(const std::string& agent, const PushNotificationConfig& config);
    A2AResult getPushConfig(const std::string& agent, const std::string& task_id, const std::string& id);
    A2AResult listPushConfigs(const std::string& agent, const std::string& task_id,
                              int page_size, const std::string& page_token);
    A2AResult deletePushConfig(const std::string& agent, const std::string& task_id, const std::string& id);
    A2AResult getExtendedAgentCard(const std::string& agent);
    std::optional<Task> getTask(const std::string& id);
    std::vector<Agent> listAgents();
    std::vector<std::string> listAgentIds() const;

    // Materialize one protocol artifact part into a caller-selected directory.
    // The returned path is canonicalized beneath output_dir.
    bool materializeArtifact(const std::string& task_id, const std::string& artifact_id,
                             size_t part_index, const std::string& output_dir,
                             std::string* output_path, std::string* error);

    // Refresh an agent's Agent Card + availability. Returns the agent after.
    Agent discoverAgent(const std::string& id);
    void discoverAllAgents();

    // Event sink for task.state_changed pushes to connected IPC clients
    // (DESIGN.md §18). Set by main before starting the IPC server.
    using EventSink = std::function<void(const nlohmann::json&)>;
    void setEventSink(EventSink sink) { event_sink_ = std::move(sink); }

private:
    void startSubscription(const Task& task);
    void emitStateChanged(const std::string& task_id, TaskState old_state, TaskState new_state);
    // Resolve the agent's endpoint + auth spec for a request.
    bool resolveEndpoint(const AgentId& id, std::string* endpoint, AuthSpec* auth) const;

    Database& db_;
    std::shared_ptr<A2AClient> a2a_;
    Config config_;

    std::mutex state_mutex_;
    std::map<std::string, Agent> agents_;   // by agent id

    // Cached auth specs per agent (re-resolved on demand; values never logged).
    EventSink event_sink_;
    std::vector<std::jthread> subscriptions_;
};

}  // namespace kitty_a2a
