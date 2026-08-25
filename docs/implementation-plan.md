# Implementation Plan

This is the as-built execution record. `DESIGN.md` §39 mandates a specific
order: validate upstream APIs first, then skeleton + docs, then the smallest
end-to-end vertical slice, then build/test/verify. That is exactly the order
this project followed.

## Milestone 0 — Validate upstream APIs (done)

Before writing a line of C++, the following were verified against the live
upstream rather than assumed from `DESIGN.md`:

- **A2A version**: v1.0 (not v0.3). Normative schema is Protobuf; bindings are
  JSON-RPC over HTTP/SSE, gRPC, and HTTP/REST.
- **Enum spelling**: v1.0 uses `SCREAMING_SNAKE_CASE` (e.g.
  `TASK_STATE_INPUT_REQUIRED`), not the v0.3 `input-required` kebab-case the
  design doc shows.
- **Operations**: `SendMessage`, `GetTask`, `ListTasks`, `CancelTask`,
  `SubscribeToTask`; Agent Card at `/.well-known/agent-card.json`.
- **Kitty 0.48.2**: custom kitten `main(args)` with optional
  `handle_result(...)`; `no_ui=True` supported; `kitty @` remote-control
  commands (`get-text`, `set-tab-title`) and keyboard-mode mapping
  (`map --new-mode agent ...`) confirmed available.
- **Toolchain**: C++20 (g++ 15), libcurl, sqlite3, nlohmann/json, yaml-cpp
  all present.

These findings are recorded in `docs/architecture.md` under
"A2A versioning — what changed from v0.3 to v1.0".

## Milestone 1 — Repository skeleton (done)

Layout per DESIGN.md §24:

```
a2ad/
  CMakeLists.txt
  DESIGN.md              (authoritative, unchanged)
  daemon/
    include/kitty_a2a/   (public headers)
    src/                 (implementation)
  kitten/a2a.py          (NDJSON client)
  config/agents.example.yaml
  systemd/kitty-a2a.service
  tests/
    daemon/              (unit tests)
    ipc/e2e.cpp          (integration test)
  docs/                  (architecture, plan, protocol, kitty-integration)
```

## Milestone 2 — Daemon core (done)

Implementation order, each unit compile-checked in isolation before the next:

1. `types` — `TaskState` enum + `parse_state`/`to_state_string`, `TypedId`,
   `TaskId`, `ContextId`.
2. `Artifact` / `Message` / `Task` — domain models.
3. `AgentCard` — `Agent`, `AgentCard`, `requires_auth()`,
   `effective_endpoint()`, `supports_streaming()`.
4. `Config` — YAML load, `default_agent_for(cwd)` longest-match, `Paths`,
   env-substitution for socket/db overrides.
5. `Credentials` — `CredentialProvider` + env/file/none impls.
6. `A2AClient` — `HttpTransport` interface, `parseResponse`, `parseTask`,
   `sendMessage`, `getTask`, `cancelTask`.
7. `HttpTransport` (CurlTransport) — libcurl, 30s timeout, header capture.
8. `Database` — SQLite schema + CRUD, PIMPL.
9. `IpcServer` — Unix socket, NDJSON, accept thread, `broadcastEvent`.
10. `TaskManager` — the state owner; `createTask`, `getTask`, `listTasks`,
    `refreshTask`, `respondToTask`, `cancelTask`, `reconcileAll`,
    `discoverAgent`, `discoverAllAgents`.
11. `main` — wiring, arg parsing (`--config`, `--foreground`, `--socket`,
    `--reconcile`, `--no-reconcile`, `--version`), daemonize, signal handling.

## Milestone 3 — Kitten + config + systemd (done)

- `kitten/a2a.py` — NDJSON client with `list`, `status`, `agents`, `submit`,
  `respond`, `cancel` subcommands; colored state tags; `--json` passthrough.
- `config/agents.example.yaml` — documented example with auth reference and
  `ipc` overrides.
- `systemd/kitty-a2a.service` — `systemd --user` unit with hardening.

## Milestone 4 — Tests (done)

- **Unit tests** (`tests/daemon/`): `types_test`, `artifact_test`,
  `config_test`, `a2a_client_test` (with a mock `HttpTransport`),
  `credentials_test`, `database_test` (real SQLite in a temp dir),
  `protocol_test` (verifies the exact JSON-RPC request shapes the daemon
  emits). A single `test_main.cpp` drives all suites via a tiny header-only
  framework (`test_framework.hpp`) — no external test dependency.
- **Integration test** (`tests/ipc/e2e.cpp`): spins up a real mock A2A
  JSON-RPC server (Python `http.server`) on a free port, launches the actual
  `a2ad` binary as a subprocess with an isolated socket + DB, and drives the
  full IPC protocol — `ping`, `submit`, `list`, `status` (with refresh),
  `respond`, `cancel`, `agents` — over the live Unix socket.

## Milestone 5 — Build & verify (done)

- CMake configures and builds `a2ad`, `a2ad_tests`, and `a2ad_e2e`.
- `a2ad_tests`: **26 tests, 142 assertions, 0 failures**.
- `a2ad_e2e`: **15 assertions, 0 failures** — full submit→list→status→
  respond→cancel round-trip against the mock server.

## Future milestones (not yet built)

- SSE subscription (`SubscribeToTask`) for push state changes instead of
  polling.
- `ListTasks` as an IPC op (remote agent task listing).
- gRPC and REST bindings (JSON-RPC only today).
- Artifact download / file materialization.
- Multi-agent routing policies and per-project `default_agent` enforcement in
  `submit` (currently `submit` requires an explicit `agent`).
- `kitty @` tab-title / notification integration for state changes (the
  keyboard-mode and `set-tab-title` commands were validated but not yet wired
  into the kitten).
