# Architecture

This document describes the as-built architecture of `a2ad` and the `a2a.py`
kitten, and records where the implementation intentionally diverges from the
assumptions baked into `DESIGN.md`. `DESIGN.md` is the authoritative spec; this
file is the engineering record of what actually ships.

## Component map

```
Kitty terminal
   |
   |  kitten a2a.py  (Python, in-process NDJSON client over Unix socket)
   v
a2ad  (C++20 daemon, persistent, owns all state)
   |
   +-- Database       SQLite at $XDG_STATE_HOME/kitty-a2a/a2ad.db
   +-- TaskManager    the state owner; routes, creates, refreshes, cancels
   +-- A2AClient      A2A v1 JSON-RPC, HTTP+JSON, and native gRPC
   +-- IpcServer      Unix domain socket, NDJSON, one accept thread
   +-- Config         agents.yaml (YAML), XDG-aware paths
   +-- CredentialProvider  env / file / none — secrets never in config
```

The kitten is deliberately dumb: it formats output and talks NDJSON. The daemon
owns every state transition so a task's lifecycle survives the kitten exiting
(DESIGN.md §16).

## Process model

One daemon per user. It:

1. loads `agents.yaml` (or `--config`),
2. opens (or creates) the SQLite DB,
3. binds the Unix socket at `$XDG_RUNTIME_DIR/kitty-a2a/a2ad.sock`
   (0600, parent 0700),
4. does a best-effort `discoverAllAgents()` (GET the Agent Card),
5. subscribes to streaming-capable agents and optionally runs the configured
   polling reconcile fallback for other non-terminal tasks,
6. blocks on SIGINT/SIGTERM; on exit it `sqlite3_close()`s and removes the
   socket file.

Streaming subscriptions are daemon-owned. The kitten never connects directly
to a remote A2A stream (DESIGN.md §17).

## Threading

- One accept thread (IpcServer).
- One short-lived thread per IPC connection (kitten calls are short-lived).
- TaskManager serializes mutating operations with a `std::mutex`.
- The reconcile loop runs on the main thread between sleep iterations.
- A2AClient uses fresh libcurl handles for JSON-RPC/REST and gRPC channels for
  Protobuf RPCs. TaskManager owns cancellable subscription threads and joins
  them during shutdown.

## Persistence

SQLite schema (`tasks` + `agents` tables). Tasks serialize their
`messages`/`artifacts` as JSON blobs in the `messages`/`artifacts` columns.
The `state` column stores the internal enum string (`SUBMITTED`, `WORKING`,
`INPUT_REQUIRED`, `COMPLETED`, `FAILED`, `CANCELED`, …). On startup the daemon
re-opens the DB and, if `--reconcile` is set, re-polls every non-terminal task
to recover post-crash state (DESIGN.md §18).

## A2A versioning — what changed from v0.3 to v1.0

DESIGN.md was written against the A2A **v0.3** REST-style API. The current
upstream A2A is **v1.0**, which is normative in **Protobuf** and offers three
equivalent bindings: JSON-RPC 2.0 over HTTP/SSE, gRPC, and HTTP/REST. The
implementation supports all three and honors Agent Card interface preference.

### Enum spelling (the big one)

| v0.3 (DESIGN.md)            | v1.0 (ProtoJSON)                  |
|-----------------------------|-----------------------------------|
| `input-required`            | `TASK_STATE_INPUT_REQUIRED`       |
| `working`                   | `TASK_STATE_WORKING`              |
| `completed`                 | `TASK_STATE_COMPLETED`            |
| `failed`                    | `TASK_STATE_FAILED`               |
| `canceled` / `cancelled`    | `TASK_STATE_CANCELED`             |
| `submitted`                 | `TASK_STATE_SUBMITTED`            |
| `auth-required`             | `TASK_STATE_AUTH_REQUIRED`        |
| `rejected`                  | `TASK_STATE_REJECTED`             |

The daemon's internal enum uses **`SCREAMING_SNAKE_CASE`** to match v1.0.
`parse_state()` / `to_state_string()` are the single place where wire values
are mapped to internal ones (and vice-versa). If an older agent still sends
`input-required`, `parse_state()` normalizes the common v0.3 aliases so the
daemon is tolerant of mixed-version fleets.

### Method names

| v0.3 (DESIGN.md)            | v1.0 (implemented)                |
|-----------------------------|-----------------------------------|
| `agent/getTasks`            | `GetTask` (singular) / `ListTasks`|
| `agent/sendMessage`         | `SendMessage`                     |
| `agent/cancelTask`          | `CancelTask`                      |
| `agent/subscribeToTask`     | `SubscribeToTask` (SSE, unimpl.)  |

The A2A v1.0 JSON-RPC methods use CamelCase. The daemon sends `GetTask`,
`SendMessage`, `CancelTask`. (`ListTasks` is parsed but not yet wired to an
IPC op in v0.)

### Request/response envelope

v1.0 is strict JSON-RPC 2.0:

```json
{ "jsonrpc": "2.0", "id": "<n>", "method": "SendMessage",
  "params": { "message": { "role": "ROLE_USER", "parts": [{"text":"..."}],
                           "messageId":"...", "taskId":"...",
                           "contextId":"..." } } }
```

Responses are `{ "jsonrpc":"2.0", "id":"<n>", "result": { ... } }` or
`{ "jsonrpc":"2.0", "id":"<n>", "error": {"code":<n>,"message":"..."} }`.

The daemon's `A2AClient::parseResponse` handles both the `result` and the
`error` envelope, and maps `result.task` / `result.status` /
`result.artifacts` onto the internal `Task` struct.

### Agent Card

Path unchanged: `/.well-known/agent-card.json`. v1.0 added
`supported_interfaces` (a list of `{url, protocol_binding, protocol_version}`)
and `security_schemes` (OpenAPI-style). The daemon reads
`supported_interfaces[0].url` as the effective endpoint when present, and
treats a non-empty `security_schemes` map as `requires_auth() == true`.

## IPC protocol (v0)

Newline-delimited JSON over a Unix socket. Each request is one JSON object on
one line; each response is one JSON object on one line.

| op         | params                                    | response                              |
|------------|-------------------------------------------|---------------------------------------|
| `ping`     | —                                         | `{ok, pong}`                          |
| `submit`   | `agent`, `message`, `cwd?`, `task_id?`, `context_id?` | `{ok, task_id, context_id, state, message}` |
| `list`     | `include_terminal?`                       | `{ok, tasks:[{task_id,state,age_s,title,...}]}` |
| `status`   | `task_id`, `refresh?`                     | `{ok, task:{...}}`                    |
| `respond`  | `task_id`, `message`                      | `{ok, task_id, state}`                |
| `cancel`   | `task_id`                                 | `{ok, task_id, state}`                |
| `agents`   | —                                         | `{ok, agents:[{name,endpoint,available,...}]}` |
| `discover` | `agent`                                   | `{ok, agent:{...}}`                   |

Events (daemon → client, broadcast to all connected):

```json
{ "type": "task.state_changed", "task_id": "...", "from": "WORKING", "to": "INPUT_REQUIRED" }
```

v0 has no auth on the socket (DESIGN.md §5: local socket, per-user, 0600).

## Credential model

`CredentialProvider` is an interface with three implementations:

- `EnvCredentialProvider` — reads a named env var at request time.
- `FileCredentialProvider` — reads a file path (0600 recommended).
- `NoneCredentialProvider` — no auth.

`make_credential_provider()` builds the right one from an `AuthSpec`
(`{type, name}`). The `Authorization` header is set only when the provider
returns a non-empty value. YAML config holds **references only** — never the
secret (DESIGN.md §20).

## Transport boundary

JSON-RPC, HTTP+JSON/REST, and native gRPC are implemented, including SSE and
gRPC server-streaming subscriptions. Agent Card ordering drives binding
selection. Remote `ListTasks`, artifact materialization, and project-aware
routing are also implemented. Multi-user/remote IPC remains intentionally out
of scope per the local single-user daemon design.
