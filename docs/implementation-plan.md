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
- **Operations**: all eleven v1 operations, including explicit streaming send,
  push-notification configuration CRUD, and the extended Agent Card; public
  Agent Card at `/.well-known/agent-card.json`.
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

## Milestone 6 — Streaming, remote listing, and routing (done)

- `SubscribeToTask` uses a long-lived SSE request, parses events incrementally,
  persists each task update, broadcasts state changes over IPC, and reconnects
  with bounded backoff. Shutdown interrupts idle libcurl streams cleanly.
- `remote_list` exposes the remote agent's A2A `ListTasks` operation separately
  from the daemon's persisted `list`; context and page-size filters are passed
  through.
- `submit` accepts an omitted agent and enforces longest-prefix project routing.
  The kitten exposes this as `submit --route ...`; an explicit agent continues
  to override the project default.
- Periodic reconciliation (`ipc.reconcile_interval_sec`) is active as a polling
  fallback for agents that do not advertise streaming.

## Milestone 7 — Binding negotiation and artifacts (done)

- Agent Card interface negotiation honors the server's preferred-interface
  ordering across JSON-RPC, HTTP+JSON, and gRPC.
- The HTTP+JSON/REST binding implements the v1.0 operation paths and media type
  for send, get, list, cancel, and SSE subscribe.
- `artifact.materialize` writes structured artifact parts without scraping
  terminal output. Text, structured JSON, base64 raw data, and authenticated
  artifact URLs are supported; filenames are confined to the requested output
  directory.
- The native gRPC binding uses the gRPC C++ runtime and build-generated sources
  from the checked-in A2A v1 Protobuf schema. Send, get, list, cancel, and
  server-streaming subscribe are supported, including TLS, credentials,
  deadlines, cancellation, and gRPC status classification.

## Milestone 8 — Kitty state integration (done)

- `watch` refreshes a task, prints state transitions, updates the current Kitty
  tab title through remote control, and emits OSC 99 notifications for
  interrupted and terminal states.
- `remote-list` and `artifact` expose the new daemon operations.

## Milestone 9 — Final verification (done)

- Native gRPC operations are exercised against an in-process HTTP/2/Protobuf
  server, including a server-streaming subscription.
- Unit tests: **196 assertions, 0 failures**, including an idle-CPU regression
  check for the IPC listener.
- Full CTest: **3/3 suites passed** (`unit`, IPC end-to-end, daemon mode).

## Milestone 10 — A2A v1.0 full client conformance (done)

- Selected interfaces carry binding, v1 minor version, and optional tenant.
- All eleven operations are available over JSON-RPC, HTTP+JSON, and gRPC.
- Ordinary sends request immediate return; streaming send remains explicit.
- Errors, pagination, filters, version headers, UUIDs, metadata, unknown states,
  TLS-by-default gRPC, and direct Message responses follow v1 semantics.
- Card security requirements are parsed; legacy bearer configuration remains
  shorthand while API keys, HTTP auth, OAuth/OIDC, and mTLS are supported.
- SQLite schema v2 migration separates local interaction and remote task IDs.
- Usable nonconforming cards remain available with compatibility warnings.
