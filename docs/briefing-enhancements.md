# Daily Tech Briefing enhancements

This change implements daemon-side foundations recovered from the September
11–14 briefing recommendations. It preserves the ownership split: Kitty is a
frontend, a2ad owns durable orchestration, A2A transports remote operations, and
workers own their harnesses. It does **not** implement every recommendation
end to end: remote sandbox enforcement and authenticated cross-daemon grant
propagation require a worker/gateway implementation outside this repository.

## Implemented behavior

| Recommendation | Behavior and entry points |
| --- | --- |
| Task / Execution / AgentSession separation | Existing tasks remain authoritative for remote task state. New execution records track delivery attempts and their results; durable sessions group attempts using `session_id`. `session.create/list/get`, `execution.list/get`. A delivered request is not a completed remote task. |
| Durable continuations and human handoffs | Existing `respond` handles A2A input/auth requirements. `continuation.create/list` records INPUT_REQUIRED, APPROVAL, RETRY and HANDOFF requests. Bind a submission to `continuation_id`; a successful dispatch marks it delivered. `retry_of` only accepts a definitively failed attempt. Neither feature invents a remote task state. |
| Task-scoped effects and policy | `policy.enforce: true` gates mutating IPC operations. Supplying `effects` also enables gating for that request in compatibility mode. Effects are validated against the fixed vocabulary below. Approval binds the entire canonical request, including task, agent, operation and parameters. |
| Protocol-visible approval | `approval.list/decide/revoke` persists owner decisions. Approval is a local IPC workflow, not a newly claimed standard A2A operation. Decisions expire after 300 seconds by default, configurable to 1–3600 seconds. |
| Delegation identity and attenuation | `parent_execution_id` records a local identity chain (maximum 16 entries), requires DELEGATE in the parent grant, and forbids child effects exceeding the parent. Every ancestor grant must remain approved and unexpired. Each child needs its own approval. This is a local delegation record, not a remotely authenticated identity chain. |
| Idempotency and uncertain delivery | Caller `request_id` is durable and bound to a request SHA-256. Exact completed repeats return their saved result; changed requests conflict. In-flight, interrupted and ambiguous network operations cannot be dispatched again under the same ID. `submit` and `respond` use that ID as the wire message ID. Remote exactly-once execution is not assumed. |
| Leases | `lease.acquire/renew/release` manages resource leases with owner/token checks, expiry and increasing fencing numbers. Attach the returned `lease` object to a mutation to check ownership and fencing at admission. Consumers must also enforce fencing at the resource; a lease expiry does not kill a remote process. Dispatch records expose a diagnostic deadline; it does not authorize redispatch. |
| Audit and observability | Append-only SQLite events form a SHA-256 chain. Control-record changes and their audit events commit together. `audit.verify` checks the chain and returns a head hash for external anchoring. SQL triggers reject event update/delete. Database-owner tampering or truncation is not prevented by a local hash chain. |
| Local event subscriptions | `channel.open/read/ack` provides persistent, independent frontend cursors, bounded pages of 1–500 events, explicit acknowledgements and replay after reconnect/restart. `events.read` supports stateless cursors. The transport uses short requests, not an unbounded response on the original task request. |
| Lazy discovery | No eager discovery of idle configured agents at startup. Cards are discovered on use, refreshed after five minutes, and conditionally fetched using ETag/If-None-Match when supported. Explicit `discover` forces validation. Discovery caches are process-local; a restart revalidates on use. |
| Deterministic and explainable routing | Existing longest-project-path routing is retained. `route.explain` returns the selected agent, reason, health and explicit no-fallback policy. Quarantined agents cannot receive new mutating dispatches through the control plane. |
| Agent health and quarantine | Three uncertain dispatches quarantine a target. `agent.health/quarantine/release` supports inspection and owner intervention. Successful delivery resets the consecutive-failure count but does not silently release a quarantine. |
| Provenance and SBOM | `provenance.record/list` stores owner-provided source/version/hash/SBOM references. `cmake --build build --target sbom` generates a CycloneDX 1.5 direct-dependency inventory with the actual executable digest, source revision and dirty flag. Neither is a remote attestation. |
| Artifact integrity | Optional request `sha256` or part metadata `sha256` is checked before saving. Files are created mode 0600 via exclusive, no-follow openat; existing files and symlinks are not overwritten. Audit events record digest, size and expected-digest verification. Credentials are withheld from cross-origin artifact requests; HTTP redirects are not automatically followed. Buffered HTTP bodies are capped at 64 MiB. |
| Lifecycle and local identity | Message-only interactions keep null remote IDs after reopen. Newly submitted remote tasks get independent local IDs, preventing different agents' identical remote IDs from overwriting each other. Subscription workers are joined before database teardown. The Unix socket verifies the peer UID and bounds idle reads/writes. |

## Effect vocabulary and trust boundary

`READ_REMOTE`, `WRITE_REMOTE`, `EXECUTE_REMOTE`, `PUBLISH`, `CREATE_IDENTITY`,
`STORE_EXTERNAL`, `SEND_MESSAGE`, `DELEGATE`.

The daemon automatically includes EXECUTE_REMOTE for submit/respond/stream_submit,
WRITE_REMOTE for cancellation/push configuration changes, and STORE_EXTERNAL
for artifact materialization. Other requested effects are declarations approved
for that exact operation. This policy does not parse a prompt to infer its
side effects, nor does it control what a remote agent does after receiving it.
Read operations and daemon reconciliation are not approval-gated in this version.

The owner Unix socket is an **administrative** interface: its peers can approve
requests. Do not mount it, its containing runtime directory, credentials or the
state database inside untrusted worker sandboxes. A same-UID peer is the same
local authority, not a separate authenticated human. There is no new TCP or
unauthenticated localhost control listener.

## Owner workflow

Existing configurations default to compatibility mode to preserve existing
Kitty commands. Enable policy explicitly in agents.yaml:

```yaml
policy:
  enforce: true
```

For a reviewable request, keep its JSON and stable request ID:

```sh
kitten a2a.py control '{"op":"session.create","id":"maintenance"}'
kitten a2a.py control '{"op":"submit","request_id":"build-001","session_id":"maintenance","agent":"cpp-specialist","message":"Build the project","effects":["EXECUTE_REMOTE"]}'
kitten a2a.py control '{"op":"approval.list"}'
kitten a2a.py control '{"op":"approval.decide","id":"build-001","allow":true,"reason":"Review completed"}'
```

Resend the **identical** submit JSON after approval. Its result includes an
`execution_id` and the task ID. Use `execution.get` for delivery diagnostics;
use the existing `status` operation for remote task progress. Altering the
request after approval requires a new request ID and a new decision.

```sh
kitten a2a.py control '{"op":"execution.get","id":"build-001"}'
kitten a2a.py events --channel workstation-panel --follow
kitten a2a.py control '{"op":"audit.verify"}'
```

Each concurrent frontend must use its own channel name. The event command
prints/flushed events before acknowledgement: interruption can replay events
(at-least-once delivery), so consumers should deduplicate by sequence.
Events are not pruned automatically; deployments should monitor database size.
Task snapshots remain authoritative: task writes and their notification are
not one transaction, so consumers should resnapshot after reconnect.

## Remaining integration work

- Authenticate and propagate effect grants and delegation identity between
  daemons/workers, with a negotiated A2A extension and independent enforcement.
- Enforce worker filesystem/network effects outside the worker, including
  protecting the owner control socket from all worker harnesses.
- Make tool/schema discovery lazy within individual worker harnesses. A2A
  Agent Card discovery is implemented here; it does not expose a generic tool
  catalog API for arbitrary harnesses.
- Add resource-side fencing integration, remote attestation, complete transitive
  SBOM generation, and an atomic task/event outbox if those stronger guarantees
  are required. Current records and APIs must not be represented as these guarantees.

## Validation

The build requires OpenSSL in addition to the existing dependencies. CMake now
supports distro Protobuf module discovery as well as Protobuf config packages.
The GitHub workflow builds on Ubuntu 24.04 and runs all four CTest suites.
For a runtime that forbids Unix sockets, `A2AD_TEST_EXCLUDE=idle_ipc` can run
non-IPC unit coverage; this is not equivalent to passing the integration suite.
