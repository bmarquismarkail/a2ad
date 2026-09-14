# Kitty Integration

How `a2a` plugs into Kitty 0.48.2, and what was validated against the installed
Kitty before this was built.

## The kitten

`kitten/a2a.py` is a standard Kitty custom kitten. It exposes:

```
kitten a2a.py list [--all] [--json]
kitten a2a.py status <task-id> [--json]
kitten a2a.py agents
kitten a2a.py submit <agent> [message words...]
kitten a2a.py stream-submit <agent> [message words...]
kitten a2a.py remote-list <agent> [filters]
kitten a2a.py subscribe <task-id>
kitten a2a.py respond <task-id> [text words...]
kitten a2a.py cancel <task-id>
kitten a2a.py push-create <agent> <task-id> <https-url> [--token-file PATH]
kitten a2a.py push-get <agent> <task-id> <config-id>
kitten a2a.py push-list <agent> <task-id>
kitten a2a.py push-delete <agent> <task-id> <config-id>
kitten a2a.py extended-card <agent>
```

It is a thin NDJSON client over the daemon's Unix socket (see
`docs/protocol.md` §1). It carries **no** A2A knowledge — all orchestration is
in the daemon. The kitten only:
- resolves the socket path (`$A2AD_SOCK` → `$XDG_RUNTIME_DIR/kitty-a2a/a2ad.sock`
  → per-user temp fallback, mirroring the daemon's `Paths::resolve()`),
- sends one request, reads one response, formats it for the terminal,
- colors states (blue=WORKING, yellow=INPUT_REQUIRED, green=COMPLETED,
  red=FAILED, gray=CANCELED).

`remote-list --json` retains the task-array output used by existing scripts;
`--response-json` prints the pagination envelope. Push tokens and callback
credentials are accepted only as file references and are redacted from IPC.

### Installing the kitten

Kitty discovers kittens in `$KITTY_DIRECTORY/kittens/` and
`~/.config/kitty/kittens/`. To install:

```sh
# symlink or copy into the user kitten directory
mkdir -p ~/.config/kitty/kittens
ln -s /home/brandon/src/projects/a2ad/kitten/a2a.py ~/.config/kitty/kittens/a2a.py
```

After that, `kitty +kitten a2a list` works from any kitty shell, or map a
shortcut (below).

The explicit script form also works:

```sh
kitten a2a.py list
```

Kitty includes `a2a.py` in `main(args)` for this form; the kitten normalizes
that launcher token before parsing the `list` subcommand.

### Validated against the installed Kitty 0.48.2

Before writing the kitten, the following were confirmed from the installed
Kitty sources (`/usr/lib/kitty/`):

- A custom kitten is a Python file with a `main(args: list[str]) -> int`
  function (and an optional `handle_result(...)`). `a2a.py` provides
  `main(args)` and returns an exit code. ✓
- `handle_result(..., no_ui=True)` is supported for headless result
  handling — not needed for v0 (the kitten prints to stdout), but available
  for a future TUI. ✓
- `kitty @ get-text --match <win> --extent ...` and
  `kitty @ set-tab-title --match <tab> <title>` are valid remote-control
  commands (the `@` subcommand). ✓
- Keyboard-mode mapping is supported:
  `map --new-mode agent <keys>`, `map --mode agent <keys> <action>`, and
  `pop_keyboard_mode`. ✓

## Suggested `kitty` keymaps

In `~/.config/kitty/kitty.conf`, map the common operations:

```conf
# Open the a2a task list
map f8                    kitten a2a.py list

# Submit the current selection (or a literal prompt) to the default agent
map f9                    kitten a2a.py submit cpp-specialist

# A small "agent mode": F10 cycles through agent shortcuts
map --new-mode agent f10  pop_keyboard_mode
map --mode agent f10      kitten a2a.py agents
```

(Exact key choices are up to the user; the above are placeholders.)

## Tab-title / notification integration

The daemon broadcasts `task.state_changed` events over the socket. The kitten's
`watch <task-id>` command:

- refreshes the task and prints state transitions;
- calls `kitty @ set-tab-title` on each state change;
- emits an OSC 99 notification for `INPUT_REQUIRED`, `AUTH_REQUIRED`, and
  terminal states;
- exits when the task reaches a terminal state.

The OSC notification path does not require remote-control permission.

## Remote control prerequisites

`kitty @` (used by the tab-title integration and by the daemon if it
ever drives the terminal) requires `allow_remote_control = yes` in
`kitty.conf` and a `kitty` running with a control socket. The v0 daemon does
**not** call `kitty @`; it only talks to the local IPC socket. No
`allow_remote_control` is required for the v0 kitten.

## Process relationship

```
kitty (terminal)
   └── kitten a2a.py   (short-lived, one request per invocation)
            │
            └──→ a2ad socket  →  a2ad daemon (persistent, owns state)
```

The kitten is launched per-invocation and exits. The daemon persists
independently (user systemd unit or a manual `a2ad --foreground`). A task's
state survives the kitten exiting because the daemon — not the kitten — is
the state owner (DESIGN.md §16).

## Failure modes the kitten handles

- **Daemon not running**: `A2AError` with a hint to start `a2ad --foreground`
  or enable the systemd unit. Exits 1.
- **Unknown op / bad request**: the daemon returns `{ok:false, error:...}`;
  the kitten prints the error and exits 1.
- **Network / agent failure on submit**: the daemon returns the classified
  `error_kind`; the kitten surfaces `error` and exits 1.
