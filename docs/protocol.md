# Protocol Reference

Two protocols are in play: the **local IPC** between the kitten and the daemon,
and the **A2A JSON-RPC** between the daemon and remote agents. Both are
documented here as implemented.

## 1. Local IPC (kitten ⇄ daemon)

**Transport**: Unix domain socket at `$XDG_RUNTIME_DIR/kitty-a2a/a2ad.sock`
(override with `ipc.socket` in config or `A2AD_SOCK`). Perms: parent dir 0700,
socket 0600. No auth (local, single-user).

**Framing**: newline-delimited JSON. One request per line; one response per
line. The client sends exactly one request, reads one response line, closes.

### Requests

Every request is a JSON object with an `"op"` field.

#### `ping`
```json
{ "op": "ping" }
```
→ `{ "ok": true, "pong": true }`

#### `submit`
```json
{ "op": "submit", "agent": "cpp-specialist", "message": "fix the bug",
  "cwd": "/home/brandon/src/repos/Proto-Time" }
```
`cwd` is used for longest-path agent routing (DESIGN.md §13). For a
continuation, pass the existing `task_id` and `context_id`:
```json
{ "op": "submit", "agent": "cpp-specialist", "message": "continue",
  "task_id": "t-abc", "context_id": "c-123" }
```
→ success:
```json
{ "ok": true, "task_id": "t-abc", "context_id": "c-123",
  "state": "WORKING", "message": "task submitted" }
```
→ failure:
```json
{ "ok": false, "error": "unknown agent: foo", "error_kind": "..." }
```

#### `list`
```json
{ "op": "list" }
```
`include_terminal` defaults to `true` (the kitten passes `false` unless
`--all`). → 
```json
{ "ok": true, "tasks": [
    { "task_id": "t-abc", "state": "WORKING", "age_s": 42,
      "title": "fix the bug", "agent": "cpp-specialist", "cwd": "..." }
] }
```

#### `status`
```json
{ "op": "status", "task_id": "t-abc", "refresh": true }
```
`refresh` (default false) first polls the agent for the latest state before
returning. →
```json
{ "ok": true, "task": {
    "task_id": "t-abc", "state": "INPUT_REQUIRED", "agent": "cpp-specialist",
    "endpoint": "http://...", "cwd": "...", "context_id": "c-123",
    "created_at": "2026-08-24T12:00:00Z", "updated_at": "...",
    "title": "fix the bug", "message": "fix the bug",
    "error": null,
    "messages": [ ... ], "artifacts": [ ... ]
} }
```

#### `respond`
```json
{ "op": "respond", "task_id": "t-abc", "message": "use the emulator IR" }
```
→ `{ "ok": true, "task_id": "t-abc", "state": "WORKING" }`

#### `cancel`
```json
{ "op": "cancel", "task_id": "t-abc" }
```
→ `{ "ok": true, "task_id": "t-abc", "state": "CANCELED" }`

#### `agents`
```json
{ "op": "agents" }
```
→
```json
{ "ok": true, "agents": [
    { "name": "cpp-specialist", "endpoint": "http://...",
      "available": true, "description": "..." }
] }
```

#### `discover`
```json
{ "op": "discover", "agent": "cpp-specialist" }
```
Re-fetches the Agent Card. → `{ "ok": true, "agent": { ... } }`

### Events (daemon → all connected clients)

The daemon broadcasts to every open connection:

```json
{ "type": "task.state_changed", "task_id": "t-abc",
  "from": "WORKING", "to": "INPUT_REQUIRED" }
```

The v0 kitten does not consume these (it issues request/response calls); they
are present for a future streaming UI.

## 2. A2A JSON-RPC (daemon ⇄ remote agent)

**Transport**: HTTP POST, `Content-Type: application/json`. One request per
call (no keep-alive multiplexing in v0). 30 s timeout.

**Envelope** (strict JSON-RPC 2.0):
```json
{ "jsonrpc": "2.0", "id": "1", "method": "<Name>", "params": { ... } }
```
Responses:
```json
{ "jsonrpc": "2.0", "id": "1", "result": { ... } }
```
or
```json
{ "jsonrpc": "2.0", "id": "1", "error": { "code": -32000, "message": "..." } }
```

### Methods

| method        | params                                                        | used by    |
|---------------|---------------------------------------------------------------|------------|
| `SendMessage` | `message` (role, parts, messageId, taskId?, contextId?)       | `submit`, `respond` |
| `GetTask`     | `id` (task id)                                                | `status refresh`, `reconcile` |
| `CancelTask`  | `id`                                                          | `cancel`   |
| `SubscribeToTask` | (SSE — not implemented)                                   | —          |

#### SendMessage (new task)
```json
{ "jsonrpc": "2.0", "id": "1", "method": "SendMessage",
  "params": { "message": {
      "role": "ROLE_USER",
      "parts": [ { "text": "fix the bug" } ],
      "messageId": "m-<uuid>",
      "contextId": "c-<uuid>"
  } } }
```

#### SendMessage (continuation)
```json
{ "jsonrpc": "2.0", "id": "2", "method": "SendMessage",
  "params": { "message": {
      "role": "ROLE_USER",
      "parts": [ { "text": "use the emulator IR" } ],
      "messageId": "m-<uuid>",
      "taskId": "t-abc",
      "contextId": "c-123"
  } } }
```

#### GetTask
```json
{ "jsonrpc": "2.0", "id": "3", "method": "GetTask",
  "params": { "id": "t-abc" } }
```

#### CancelTask
```json
{ "jsonrpc": "2.0", "id": "4", "method": "CancelTask",
  "params": { "id": "t-abc" } }
```

### Result shape

The daemon reads `result.task` (preferred) or a top-level `result` that is
itself a task. A task object:
```json
{ "id": "t-abc", "contextId": "c-123",
  "status": { "state": "TASK_STATE_WORKING", "message": "..." },
  "artifacts": [ { "parts": [ { "text": "..." } ], "mime_type": "text/plain" } ],
  "history": [ { "role": "ROLE_AGENT", "parts": [ { "text": "..." } ] } ] }
```

### State mapping (wire → internal)

| wire (`status.state`)        | internal `TaskState`        |
|------------------------------|-----------------------------|
| `TASK_STATE_SUBMITTED`       | `SUBMITTED`                 |
| `TASK_STATE_WORKING`         | `WORKING`                   |
| `TASK_STATE_INPUT_REQUIRED`  | `INPUT_REQUIRED`            |
| `TASK_STATE_AUTH_REQUIRED`   | `AUTH_REQUIRED`             |
| `TASK_STATE_COMPLETED`       | `COMPLETED`                 |
| `TASK_STATE_FAILED`          | `FAILED`                    |
| `TASK_STATE_CANCELED`        | `CANCELED`                  |
| `TASK_STATE_REJECTED`        | `REJECTED`                  |
| `TASK_STATE_UNKNOWN`         | `UNKNOWN`                   |

`parse_state()` also tolerates the v0.3 kebab-case aliases
(`input-required`, `working`, …) for mixed-version agents.

### Agent Card

`GET <endpoint>/.well-known/agent-card.json`. The daemon stores the parsed
card and uses `supported_interfaces[0].url` as the effective endpoint when
present. `security_schemes` (non-empty) marks the agent as `requires_auth`.

## 3. Error kinds

The daemon classifies failures into `A2AResult::ErrorKind`:

| kind            | meaning                                            |
|-----------------|----------------------------------------------------|
| `none`          | success                                            |
| `jsonrpc`       | the agent returned a JSON-RPC error object         |
| `http`          | HTTP 4xx/5xx                                       |
| `parse`         | response was not parseable as a task/envelope      |
| `local_network` | connection failed (no route, refused, DNS)         |
| `timeout`       | libcurl timed out                                  |

These surface in IPC responses as `error_kind` for programmatic handling.
