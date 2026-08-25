"""
kitty-a2a — talk to the a2ad daemon from inside Kitty.

Usage (from a kitty terminal, or via a mapped shortcut):

    kitten a2a.py list                 # show non-terminal tasks
    kitten a2a.py status <task-id>     # detailed status (refresh from agent)
    kitten a2a.py agents               # list configured agents + availability
    kitten a2a.py submit <agent> [message...]   # submit a prompt
    kitten a2a.py respond <task-id> [text...]   # answer an input-required task
    kitten a2a.py cancel <task-id>     # request cancellation

The kitten is a thin NDJSON client over the daemon's Unix socket. All
intelligence lives in the daemon; this script only formats output for the
terminal (DESIGN.md §7, §16).

The daemon socket path is resolved in the same order as the daemon:
  $A2AD_SOCK, then $XDG_RUNTIME_DIR/kitty-a2a/a2ad.sock, then a per-user
  temp fallback. This mirrors daemon/src/Config.cpp::Paths::resolve().
"""
from __future__ import annotations

import argparse
import json
import os
import socket
import sys
import tempfile
import time

STATE_COLORS = {
    "WORKING": "\033[34m",       # blue
    "INPUT_REQUIRED": "\033[33m", # yellow
    "COMPLETED": "\033[32m",     # green
    "FAILED": "\033[31m",        # red
    "CANCELED": "\033[90m",      # gray
    "SUBMITTED": "\033[36m",     # cyan
}
RESET = "\033[0m"


def _socket_path() -> str:
    p = os.environ.get("A2AD_SOCK")
    if p:
        return p
    rtd = os.environ.get("XDG_RUNTIME_DIR")
    if rtd:
        return os.path.join(rtd, "kitty-a2a", "a2ad.sock")
    return os.path.join(tempfile.gettempdir(), f"kitty-a2a-{os.getuid()}", "a2ad.sock")


class A2AError(RuntimeError):
    pass


def _rpc(path: str, payload: dict, timeout: float = 10.0) -> dict:
    """Send one NDJSON request, read one NDJSON response. Raises A2AError."""
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        sock.settimeout(timeout)
        try:
            sock.connect(path)
        except FileNotFoundError as e:
            raise A2AError(
                f"cannot connect to a2ad at {path}\n"
                f"is the daemon running? try: a2ad --foreground   (or enable the systemd unit)"
            ) from e
        sock.sendall((json.dumps(payload) + "\n").encode())
        buf = b""
        while b"\n" not in buf:
            chunk = sock.recv(65536)
            if not chunk:
                raise A2AError(f"daemon closed the connection (request: {payload.get('op')})")
            buf += chunk
        line = buf.split(b"\n", 1)[0].decode().strip()
        if not line:
            raise A2AError("daemon returned an empty response")
        return json.loads(line)
    finally:
        sock.close()


def _die(msg: str) -> None:
    sys.stderr.write(msg + "\n")
    sys.exit(1)


def _state_tag(state: str) -> str:
    color = STATE_COLORS.get(state, "")
    return f"{color}{state}{RESET}"


def _short(s: str, n: int = 70) -> str:
    s = s.replace("\n", " ")
    return s if len(s) <= n else s[: n - 1] + "…"


def _task_line(task: dict) -> str:
    tid = task.get("task_id", "")
    state = task.get("state", "?")
    title = task.get("title", "") or task.get("message", "")
    age = task.get("age_s")
    age_str = f" {int(age):>5}s" if isinstance(age, (int, float)) else ""
    parts = [f"{_state_tag(state):<22}", f" {tid:<12}", f" {age_str}"]
    if title:
        parts.append(f" {_short(title)}")
    return "".join(parts)


def _detail(t: dict) -> list[str]:
    lines = []
    tid = t.get("task_id", "")
    lines.append(f"task_id     : {tid}")
    lines.append(f"state       : {_state_tag(t.get('state', '?'))}")
    if t.get("agent"):
        lines.append(f"agent       : {t['agent']}")
    if t.get("endpoint"):
        lines.append(f"endpoint    : {t['endpoint']}")
    if t.get("cwd"):
        lines.append(f"cwd         : {t['cwd']}")
    if t.get("context_id"):
        lines.append(f"context_id  : {t['context_id']}")
    if t.get("created_at"):
        lines.append(f"created     : {t['created_at']}")
    if t.get("updated_at"):
        lines.append(f"updated     : {t['updated_at']}")
    if t.get("title"):
        lines.append(f"title       : {t['title']}")
    if t.get("message"):
        lines.append(f"message     : {_short(t['message'], 100)}")
    if t.get("error"):
        lines.append(f"error       : {t['error']}")
    msgs = t.get("messages") or []
    arts = t.get("artifacts") or []
    if msgs:
        lines.append("")
        lines.append(f"messages    : {len(msgs)}")
    if arts:
        lines.append(f"artifacts   : {len(arts)}")
        for i, a in enumerate(arts[:20]):
            blob = a.get("text") or a.get("uri") or a.get("mime_type", "")
            lines.append(f"  [{i}] {_short(blob, 80)}")
    return lines


def main(args: list[str]) -> int:
    parser = argparse.ArgumentParser(prog="kitten a2a.py", description="kitty-a2a client")
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_list = sub.add_parser("list", help="list tasks")
    p_list.add_argument("--all", action="store_true", help="include terminal tasks")
    p_list.add_argument("--json", action="store_true", help="raw JSON output")

    p_status = sub.add_parser("status", help="task detail")
    p_status.add_argument("task_id")
    p_status.add_argument("--json", action="store_true")

    sub.add_parser("agents", help="list agents")

    p_submit = sub.add_parser("submit", help="submit a prompt to an agent")
    p_submit.add_argument("agent")
    p_submit.add_argument("message", nargs="*", help="prompt text (words joined by spaces)")

    p_respond = sub.add_parser("respond", help="answer an input-required task")
    p_respond.add_argument("task_id")
    p_respond.add_argument("text", nargs="*", help="answer text")

    p_cancel = sub.add_parser("cancel", help="cancel a task")
    p_cancel.add_argument("task_id")

    ns = parser.parse_args(args)
    path = _socket_path()

    try:
        if ns.cmd == "list":
            r = _rpc(path, {"op": "list", "include_terminal": ns.all})
            if not r.get("ok"):
                _die(f"error: {r.get('error', 'unknown')}")
            if ns.json:
                print(json.dumps(r["tasks"], indent=2))
            else:
                tasks = r["tasks"]
                if not tasks:
                    print("(no non-terminal tasks)")
                for t in tasks:
                    print(_task_line(t))
                n = len(tasks)
                print(f"\n{n} task(s)")
            return 0

        if ns.cmd == "status":
            r = _rpc(path, {"op": "status", "task_id": ns.task_id, "refresh": True})
            if not r.get("ok"):
                _die(f"error: {r.get('error', 'unknown')}")
            t = r["task"]
            if ns.json:
                print(json.dumps(t, indent=2))
            else:
                for line in _detail(t):
                    print(line)
            return 0

        if ns.cmd == "agents":
            r = _rpc(path, {"op": "agents"})
            if not r.get("ok"):
                _die(f"error: {r.get('error', 'unknown')}")
            agents = r["agents"]
            if not agents:
                print("(no agents configured)")
            for a in agents:
                avail = "ok" if a.get("available") else "UNAVAILABLE"
                name = a.get("name", "")
                endpoint = a.get("endpoint", "")
                desc = a.get("description", "")
                print(f"  {name:<20} {avail:<11} {endpoint}")
                if desc:
                    print(f"    {desc}")
            return 0

        if ns.cmd == "submit":
            msg = " ".join(ns.message).strip()
            if not msg:
                _die("submit requires a non-empty message")
            r = _rpc(path, {
                "op": "submit",
                "agent": ns.agent,
                "message": msg,
                "cwd": os.getcwd(),
            })
            if not r.get("ok"):
                _die(f"error: {r.get('error', 'unknown')}")
            tid = r.get("task_id", "")
            state = r.get("state", "?")
            print(f"submitted task {tid} to agent '{ns.agent}'")
            print(f"state: {_state_tag(state)}")
            print(f"track with:  kitten a2a.py status {tid}")
            return 0

        if ns.cmd == "respond":
            text = " ".join(ns.text).strip()
            if not text:
                _die("respond requires non-empty text")
            r = _rpc(path, {"op": "respond", "task_id": ns.task_id, "message": text})
            if not r.get("ok"):
                _die(f"error: {r.get('error', 'unknown')}")
            print(f"responded to {ns.task_id}; state now {_state_tag(r.get('state','?'))}")
            return 0

        if ns.cmd == "cancel":
            r = _rpc(path, {"op": "cancel", "task_id": ns.task_id})
            if not r.get("ok"):
                _die(f"error: {r.get('error', 'unknown')}")
            print(f"cancel requested for {ns.task_id}")
            return 0

    except A2AError as e:
        _die(str(e))
    except json.JSONDecodeError as e:
        _die(f"daemon returned invalid JSON: {e}")

    return 0


if __name__ == "__main__":
    # Standalone entry point so the kitten works both as a Kitty kitten
    # (Kitty calls main(args)) and as a plain script (python3 a2a.py ...).
    sys.exit(main(sys.argv[1:]))
