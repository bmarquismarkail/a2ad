# Kitty-A2A: Implementation Prompt

You are implementing a new project named **kitty-a2a**.

The goal is to create a lightweight, terminal-native frontend for interacting with local and remote autonomous agents over the **Agent2Agent (A2A)** protocol, using **Kitty** as the human interface.

This project is deliberately **not** a terminal multiplexer and must not reproduce the architectural model of tools such as Herdr.

The central design principle is:

> **Kitty is the user interface. A2A is the agent transport. A persistent local daemon manages agent state.**

Do not insert another terminal-emulation, PTY-management, or multiplexing layer between Kitty and the applications running inside it.

---

# 1. Primary Goals

Implement a system with the following architecture:

```text
                     ┌─────────────────────────┐
                     │          Kitty          │
                     │                         │
                     │ shell / editor / tools  │
                     │                         │
                     │    kitty-a2a kitten     │
                     └────────────┬────────────┘
                                  │
                                  │ Local IPC
                                  │ Unix domain socket
                                  ▼
                     ┌─────────────────────────┐
                     │          a2ad           │
                     │                         │
                     │ Agent Registry          │
                     │ Task Registry           │
                     │ A2A Client              │
                     │ Event Subscriptions     │
                     │ Persistence             │
                     │ Credential Handling     │
                     └────────────┬────────────┘
                                  │
                                  │ A2A
                                  │ HTTPS / HTTP+JSON
                                  │ optional JSON-RPC/gRPC later
                                  ▼
           ┌──────────────────────┼──────────────────────┐
           │                      │                      │
           ▼                      ▼                      ▼
     Local Agent            Remote Agent           Remote Agent
     localhost              dev-server             hpc-server
```

The user should be able to interact with local and remote agents in essentially the same manner.

The location or runtime of an agent should be an implementation detail.

---

# 2. Non-Goals

Do **not** build:

* a terminal multiplexer;
* a PTY manager;
* a replacement terminal emulator;
* a tmux clone;
* an SSH frontend;
* a remote shell protocol;
* a general-purpose agent framework;
* an MCP implementation;
* an LLM runtime;
* an agent execution sandbox;
* a replacement for Kitty.

Do not make the Kitty kitten directly own long-lived remote A2A connections.

Do not require Kitty to remain running for remote agent tasks to continue.

---

# 3. Removed

---

# 4. Components

Implement the project with two primary components.

## 4.1 `a2ad`

`a2ad` is a persistent local daemon.

Responsibilities:

* maintain configured A2A agents;
* discover Agent Cards;
* cache agent metadata;
* submit A2A messages;
* create and track A2A Tasks;
* maintain task state;
* maintain A2A contexts/conversations;
* receive streaming task updates;
* reconnect to existing tasks;
* retrieve artifacts;
* cancel tasks;
* expose state to Kitty through local IPC;
* persist enough information to survive Kitty restarts;
* optionally survive daemon restarts;
* handle authentication securely.

The daemon should not care which model or framework exists behind an A2A endpoint.

Examples include:

```text
Codex
Claude Code
Hermes
llama.cpp
custom C++ agents
custom Python agents
HPC agents
research agents
```

These should all appear to the daemon as A2A agents.

---

## 4.2 Kitty kitten

Implement a custom Kitty kitten as the frontend.

Suggested location:

```text
~/.config/kitty/a2a.py
```

or install it through an appropriate project-specific location.

Responsibilities:

* communicate with `a2ad`;
* show available agents;
* show active tasks;
* create tasks;
* inspect task status;
* submit terminal context;
* send selections;
* send previous command output;
* receive user responses for `INPUT_REQUIRED`;
* open task views;
* optionally create Kitty tabs/windows for tasks;
* manipulate Kitty tab titles based on task state.

The kitten should be a **thin client**.

It must not become the primary task database or persistent A2A runtime.

---

# 5. Local IPC

Use a Unix domain socket between the Kitty frontend and `a2ad`.

Example:

```text
$XDG_RUNTIME_DIR/kitty-a2a/a2ad.sock
```

Fall back safely when `XDG_RUNTIME_DIR` is unavailable.

Do not expose the local management API over TCP by default.

Define a small structured protocol.

JSON over a Unix socket is acceptable for the first implementation.

Example request:

```json
{
  "request": "task.create",
  "agent": "cpp-specialist",
  "context": {
    "cwd": "/data/projects/proto-time"
  },
  "message": "Investigate the current VDP performance regression."
}
```

Example response:

```json
{
  "ok": true,
  "task_id": "91e4f0..."
}
```

Favor clear semantic operations over commands that mimic terminal behavior.

---

# 6. Agent Registry

Maintain a local registry of known agents.

Suggested configuration:

```yaml
agents:

  cpp-specialist:
    endpoint: https://dev01.internal/a2a

  build-agent:
    endpoint: https://build01.internal/a2a

  hpc-agent:
    endpoint: https://compute01.internal/a2a

  local-agent:
    endpoint: http://127.0.0.1:9010/a2a
```

The daemon should retrieve the corresponding A2A Agent Card.

Store/cache information including:

```text
name
description
endpoint
protocol version
skills
supported interfaces
streaming support
authentication requirements
input/output content types
```

Do not require the configuration file to duplicate information supplied by the Agent Card.

---

# 7. Agent Discovery

Support standard A2A Agent Card discovery.

Prefer:

```text
/.well-known/agent-card.json
```

where supported.

Design discovery as its own interface so additional discovery methods can later be added.

Potential future mechanisms:

```text
static configuration
DNS-SD / mDNS
service registry
Consul
Kubernetes
local network discovery
custom registries
```

Do not implement all of these now.

The initial implementation only needs static configuration plus standard Agent Card retrieval.

---

# 8. Task Model

A2A tasks are the central abstraction.

Do not model agent work as terminal sessions.

Internally maintain something similar to:

```cpp
struct Task {
    TaskId id;
    AgentId agent;
    ContextId context;

    TaskState state;

    std::string title;
    std::string cwd;

    std::vector<Message> messages;
    std::vector<Artifact> artifacts;
};
```

Represent A2A lifecycle states explicitly.

At minimum support:

```text
SUBMITTED
WORKING
INPUT_REQUIRED
AUTH_REQUIRED
COMPLETED
FAILED
CANCELED
REJECTED
```

Do not collapse these into:

```text
busy
idle
```

A task being inactive is not equivalent to task completion.

---

# 9. Input-Required Workflow

One of the most important workflows is remote-agent interaction without exposing the underlying terminal.

Example:

```text
A2A task
    │
    ▼
WORKING
    │
    ▼
INPUT_REQUIRED
```

The daemon records the state.

Kitty should visibly indicate that user attention is required.

Example tab title:

```text
A2A: VDP regression [INPUT]
```

When the user opens the task, display the question:

```text
Agent: cpp-specialist

I can remove this allocation, but doing so changes
the PixelRenderOutput ABI.

Allow ABI changes?

> Yes
  No
  Respond manually
```

The response must be sent as a new A2A message associated with the same task/context.

---

# 10. Kitty Context Integration

The integration should take advantage of Kitty's knowledge of the current terminal environment.

Provide the daemon with useful context when available.

Potential context includes:

```text
current working directory
current selection
last command output
visible screen contents
scrollback output
Kitty window ID
Kitty tab ID
hostname
shell command
project metadata
```

Do not blindly send all available terminal contents to remote agents.

Context transmission should always be intentional.

---

# 11. Send Selection

Support:

```text
Ctrl+Shift+A
S
```

or the equivalent UI operation.

The workflow:

```text
current Kitty selection
        │
        ▼
kitty-a2a kitten
        │
        ▼
a2ad
        │
        ▼
selected A2A agent
```

Example generated request:

```text
Working directory:
/data/projects/proto-time

Selected terminal content:
<selection>

User request:
Investigate this problem.
```

The user should be able to modify the request before submission.

---

# 12. Send Last Command Output

Support:

```text
Ctrl+Shift+A
O
```

Use Kitty shell integration when available to retrieve the output corresponding to the most recently executed command.

Example:

```bash
$ ctest --test-dir build
...
3 tests failed
```

The user can invoke:

```text
Agent → Diagnose previous command
```

and send:

```text
cwd
last command if available
command output
user instruction
```

to an appropriate A2A agent.

Do not require copying and pasting terminal output manually.

---

# 13. Project-Aware Routing

Allow agent selection defaults based upon the working directory.

Example configuration:

```yaml
projects:

  /data/projects/proto-time:
    default_agent: emulator-agent

  /data/projects/pokered:
    default_agent: retro-coder

  /data/projects/mpss4:
    default_agent: kernel-agent
```

Longest matching path should win.

Example:

```text
cwd:
/data/projects/proto-time/src/video
```

automatically suggests:

```text
emulator-agent
```

The user must still be able to select another agent.

---

# 14. Agent Picker

Provide an agent-selection UI.

Conceptually:

```text
┌──────────────────── Available Agents ────────────────────┐
│                                                          │
│ ● emulator-agent     localhost        llama.cpp          │
│ ● cpp-specialist     dev01            Codex              │
│ ● kernel-engineer    build01          Claude Code        │
│ ● hpc-profiler       compute01        custom             │
│ ○ research-agent     laptop           unavailable        │
│                                                          │
└──────────────────────────────────────────────────────────┘
```

Do not depend on runtime names such as Codex or Claude being available through A2A.

Those labels are optional metadata.

The important fields are:

```text
agent identity
availability
skills
endpoint
task capability
```

---

# 15. Kitty Task Tabs

Support an optional workflow where an A2A task is represented by a Kitty tab or window.

Example:

```text
shell │ code │ A2A: VDP regression [WORKING] │ logs
```

Status should update:

```text
[SUBMITTED]
[WORKING]
[INPUT]
[AUTH]
[DONE]
[FAILED]
[CANCELED]
```

Do not make the tab itself the authoritative task state.

The authoritative state lives in `a2ad`.

Closing a Kitty tab must **not** cancel the remote task.

Likewise, closing Kitty entirely must not cancel the task.

---

# 16. Task View

Create a human-readable task view containing:

```text
Task
Agent
State
Created time
Updated time
Working directory
Conversation/context identifier
Messages
Artifacts
Errors
```

Example:

```text
Task: 91e4f0
Agent: cpp-specialist
State: WORKING

Request:
Investigate VDP rendering performance.

Updates:

14:22 Agent accepted task.
14:22 Running existing benchmark suite.
14:23 renderFramePixels is 48% of sampled CPU time.
14:24 Investigating tile fetch path.

Artifacts:
none yet
```

The task view may stream updates.

---

# 17. Artifacts

Treat A2A Artifacts as first-class objects.

Examples:

```text
patch files
JSON results
benchmark data
logs
reports
source files
images
archives
```

The task view should list artifacts separately from conversational messages.

Example:

```text
Artifacts

1. vdp-optimization.patch
2. benchmark-before.json
3. benchmark-after.json
4. investigation.md
```

Do not infer artifacts by scraping terminal output.

Use the protocol representation.

---

# 18. Streaming

When an A2A agent supports streaming, `a2ad` should subscribe to task updates.

The Kitty client may then receive events from `a2ad`.

Possible local event:

```json
{
  "event": "task.state_changed",
  "task_id": "91e4...",
  "old_state": "WORKING",
  "new_state": "INPUT_REQUIRED"
}
```

Do not make Kitty poll remote A2A servers directly.

---

# 19. Persistence

Persist:

```text
agents
task IDs
agent associated with task
A2A context IDs
task state
timestamps
artifact metadata
project association
```

An initial SQLite implementation is preferred.

Example:

```text
~/.local/state/kitty-a2a/a2ad.db
```

Respect XDG paths where appropriate.

After restarting Kitty, the user should be able to run:

```text
Ctrl+Shift+A
L
```

and see existing tasks.

After restarting `a2ad`, it should attempt to reconcile known non-terminal tasks with their associated remote A2A endpoint.

---

# 20. Security

Do not store plaintext passwords or bearer tokens directly in the main YAML configuration.

Design authentication behind an interface.

Initial acceptable sources include:

```text
environment variables
external credential command
system keyring
client certificate
filesystem credential with strict permissions
```

Future mechanisms may include:

```text
OAuth2
mTLS
SPIFFE/SPIRE
Vault
systemd credentials
```

Do not over-engineer this initially, but avoid an API that forces secrets into `agents.yaml`.

---

# 21. Networking

A2A agents may be:

```text
localhost
LAN
VPN
remote HTTPS hosts
```

Do not assume SSH.

A2A communication should use the endpoint advertised by the Agent Card.

Prefer HTTPS for remote communication.

The implementation should work naturally across:

```text
WireGuard
Tailscale
private VLANs
normal routed networks
```

without caring which network transport made the host reachable.

---

# 22. Protocol Layer

Keep A2A protocol handling isolated from the rest of the project.

Suggested conceptual interface:

```cpp
class A2AClient {
public:
    virtual AgentCard discover(const Endpoint&) = 0;

    virtual TaskHandle sendMessage(
        const AgentId&,
        const Message&
    ) = 0;

    virtual Task getTask(const TaskId&) = 0;

    virtual void cancelTask(const TaskId&) = 0;

    virtual Subscription subscribe(
        const TaskId&,
        TaskEventHandler
    ) = 0;
};
```

Avoid leaking HTTP implementation details throughout the codebase.

The first implementation should support the simplest practical A2A transport required to interoperate with current A2A servers.

Design the abstraction so additional bindings can later be added.

---

# 23. Language Choices

Prefer:

```text
C++20 or newer for a2ad
Python for the Kitty kitten
```

The daemon should use conventional native Linux interfaces where practical.

Reasonable C++ dependencies include:

```text
libcurl or Boost.Beast
SQLite
nlohmann/json
yaml-cpp
Unix domain sockets
```

Do not introduce an unnecessarily large framework.

Keep dependencies suitable for Linux workstation and server environments.

The Kitty kitten must use Kitty's supported Python kitten APIs rather than relying on undocumented internals.

---

# 24. Proposed Repository Layout

Use a structure along these lines:

```text
kitty-a2a/
├── CMakeLists.txt
├── README.md
├── LICENSE
│
├── daemon/
│   ├── include/
│   │   └── kitty_a2a/
│   │       ├── A2AClient.hpp
│   │       ├── AgentRegistry.hpp
│   │       ├── Artifact.hpp
│   │       ├── Config.hpp
│   │       ├── Database.hpp
│   │       ├── IpcServer.hpp
│   │       ├── Task.hpp
│   │       └── TaskManager.hpp
│   │
│   └── src/
│       ├── A2AClient.cpp
│       ├── AgentRegistry.cpp
│       ├── Config.cpp
│       ├── Database.cpp
│       ├── IpcServer.cpp
│       ├── TaskManager.cpp
│       └── main.cpp
│
├── kitten/
│   └── a2a.py
│
├── config/
│   └── agents.example.yaml
│
├── systemd/
│   └── a2ad.service
│
├── tests/
│   ├── daemon/
│   ├── ipc/
│   └── protocol/
│
└── docs/
    ├── architecture.md
    ├── protocol.md
    └── kitty-integration.md
```

Adjust if necessary, but preserve the separation between:

```text
Kitty UI
local daemon
A2A protocol
storage
configuration
```

---

# 25. Service Integration

Provide a systemd user service:

```text
systemctl --user enable --now a2ad.service
```

The daemon should not require root.

Example lifecycle:

```text
login
  │
  ▼
systemd --user
  │
  ▼
a2ad
  │
  ├─ load configuration
  ├─ load persisted tasks
  ├─ refresh Agent Cards
  └─ reconcile running tasks
```

Kitty may start and stop independently.

---

# 26. Kitty Configuration

Provide a minimal integration example.

For example:

```conf
# Preserve function keys for terminal applications.
map f2 no_op

# Enter kitty-a2a command mode.
map --new-mode agent ctrl+shift+a

map --mode agent n kitten a2a.py new
map --mode agent l kitten a2a.py list
map --mode agent a kitten a2a.py agents
map --mode agent s kitten a2a.py selection
map --mode agent o kitten a2a.py output
map --mode agent r kitten a2a.py resume
map --mode agent c kitten a2a.py cancel

map --mode agent esc pop_keyboard_mode
```

Verify correct Kitty syntax against the installed/current Kitty APIs rather than blindly assuming this exact configuration works.

The design intent is authoritative; syntax may be adjusted as required.

---

# 27. Failure Handling

Handle the following explicitly:

```text
a2ad not running
invalid Agent Card
agent unreachable
TLS failure
authentication required
task rejected
task failed
lost streaming connection
Kitty closed
daemon restarted
agent restarted
unknown task
artifact unavailable
malformed server response
```

Never silently convert network errors into task completion.

Distinguish:

```text
local communication error
remote protocol error
remote task failure
authentication failure
```

---

# 28. Observability

Provide structured daemon logging.

Useful events include:

```text
agent.discovered
agent.unreachable

task.created
task.submitted
task.state_changed
task.input_required
task.completed
task.failed
task.canceled

stream.connected
stream.disconnected
stream.reconnected

artifact.received

ipc.client_connected
ipc.request
ipc.error
```

Logs should make distributed behavior diagnosable without requiring packet captures.

---

# 29. Testing

Build tests around semantic behavior rather than UI snapshots.

Required areas:

## Agent Card parsing

Test:

```text
valid card
missing fields
multiple interfaces
skills
auth metadata
streaming capability
unsupported protocol version
```

## Task lifecycle

Test:

```text
SUBMITTED → WORKING → COMPLETED

SUBMITTED → WORKING → INPUT_REQUIRED
                      ↓
                   WORKING
                      ↓
                  COMPLETED

WORKING → FAILED

WORKING → CANCELED
```

## Persistence

Verify:

```text
task survives Kitty restart
task survives daemon restart
task associations remain correct
```

## IPC

Verify:

```text
agent.list
task.create
task.list
task.get
task.respond
task.cancel
```

## Network behavior

Use a mock A2A server.

Do not require access to a real external LLM for automated tests.

---

# 30. First Milestone

Do not attempt the entire vision in the first pass.

Implement this vertical slice:

```text
Kitty
  │
  ▼
custom kitten
  │
  ▼
Unix socket
  │
  ▼
a2ad
  │
  ▼
one configured A2A endpoint
  │
  ▼
submit task
  │
  ▼
display task status/result
```

Minimum milestone functionality:

1. `a2ad` starts as a user service.
2. One A2A endpoint can be configured.
3. Its Agent Card can be retrieved.
4. Kitty can show that agent.
5. Kitty can submit a text request.
6. `a2ad` tracks the resulting task.
7. Kitty can query task status.
8. Kitty can display the result.
9. 
10. Closing Kitty does not destroy the task.

Only after this works should additional features be added.

---

# 31. Second Milestone

Add:

```text
multiple agents
task persistence
task list
INPUT_REQUIRED handling
streaming updates
artifact retrieval
Kitty tab integration
```

---

# 32. Third Milestone

Add:

```text
project-aware routing
send selection
send last command output
agent picker
authentication providers
reconnection logic
richer artifact handling
```

---

# 33. Future Direction

Design APIs so the following can later be implemented without rewriting the core:

```text
automatic agent discovery
agent skill matching
agent-to-agent delegation
MCP-backed local tools
workflow orchestration
task dependencies
parallel agent execution
remote build agents
remote GPU agents
HPC scheduler integration
Herdr-backed A2A workers
Codex-backed workers
Claude-backed workers
llama.cpp-backed workers
```

These are not part of the initial implementation.

---

# 34. Herdr Compatibility Philosophy

Herdr may eventually be used **behind** an A2A agent.

Example:

```text
A2A
 │
 ▼
remote worker gateway
 │
 ▼
Herdr
 │
 ▼
Codex / Claude / Hermes
```

That is acceptable.

However, kitty-a2a itself must never depend on Herdr.

Do not recreate Herdr's remote-terminal architecture.

The important distinction is:

```text
Herdr:
remote terminal/session management

kitty-a2a:
remote semantic agent/task management
```

---

# 35. Architectural Invariants

These requirements must remain true throughout implementation.

### Invariant 1

```text
Kitty remains a normal Kitty terminal.
```

### Invariant 2

```text
*Intentionally Removed*
```

### Invariant 3

```text
A2A tasks outlive Kitty windows.
```

### Invariant 4

```text
Remote agent location is abstracted behind A2A.
```

### Invariant 5

```text
Task state comes from A2A semantics, not PTY inspection.
```

### Invariant 6

```text
The Kitty kitten is a UI client, not the persistent agent runtime.
```

### Invariant 7

```text
a2ad does not depend on a particular LLM, agent framework,
or remote shell implementation.
```

### Invariant 8

```text
Closing a task tab is not equivalent to canceling the task.
```

### Invariant 9

```text
Agent output and task artifacts are structured protocol objects,
not scraped terminal text.
```

---

# 36. Implementation Procedure

Before coding:

1. Inspect the current A2A specification.
2. Inspect the current Kitty kitten APIs.
3. Inspect Kitty remote-control and keyboard-mode APIs.
4. Confirm the current Agent Card structure.
5. Confirm the current A2A task lifecycle.
6. Confirm the simplest standards-compliant transport to implement first.
7. Record any differences between this specification and current upstream APIs.

Do not silently change the architecture when an upstream API differs.

Adapt the implementation mechanism while preserving the design intent.

Then create:

```text
docs/architecture.md
docs/implementation-plan.md
```

before substantial implementation begins.

---

# 37. Development Rules

Work incrementally.

For each milestone:

```text
inspect
design
implement
build
test
verify
commit
```

Do not make unrelated refactors.

Do not replace working architecture simply because another framework would make a prototype shorter.

Favor readable, explicit code.

Do not hide protocol semantics behind generic abstractions that make debugging difficult.

Use RAII and modern C++ practices.

Avoid unnecessary global state.

Use strongly typed task and agent identifiers where reasonable.

Make asynchronous ownership and thread lifetimes explicit.

---

# 38. Definition of Done for the Initial Prototype

The initial prototype is successful when I can:

```text
open Kitty

cd /data/projects/proto-time

press Ctrl+Shift+A

choose an A2A agent

submit:
"Review the current video rendering implementation."

continue using the terminal normally

close Kitty entirely

reopen Kitty later

open the kitty-a2a task list

find the original task

inspect its current state

view its response and artifacts
```

The remote agent may be located on another machine.

At no point should I have to:

```text
SSH into that machine
attach to a tmux session
attach to a remote PTY
inspect a terminal pane
or know which underlying agent runtime it uses
```

That experience is the core product.

---

# 39. First Action

Do **not** immediately implement everything.

Begin by:

1. validating the architecture against the current A2A and Kitty APIs;
2. identifying the minimum libraries/dependencies needed;
3. creating the repository skeleton;
4. writing `docs/architecture.md`;
5. writing a milestone-based implementation plan;
6. identifying risks or specification mismatches;
7. implementing only the smallest end-to-end vertical slice afterward.

When you encounter an architectural ambiguity, prefer the interpretation that maintains:

> **Kitty as UI, a2ad as persistent task manager, A2A as remote-agent boundary.**

