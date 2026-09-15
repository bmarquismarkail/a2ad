#!/usr/bin/env python3
"""Exercise the actual owner socket, policy wrapper and restart persistence."""
import json
import pathlib
import socket
import subprocess
import sys
import tempfile
import time

binary = str(pathlib.Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix="a2ad-control-e2e-") as directory:
    root = pathlib.Path(directory)
    sock = root / "daemon.sock"
    config = root / "agents.yaml"
    config.write_text(f"agents: {{}}\npolicy:\n  enforce: true\nipc:\n  socket: {sock}\n  db: {root / 'state.db'}\n")
    log = (root / "daemon.log").open("w+")
    process = None

    def rpc(request):
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.settimeout(5)
            client.connect(str(sock))
            client.sendall(json.dumps(request).encode() + b"\n")
            with client.makefile("rb") as stream:
                return json.loads(stream.readline())

    def start():
        global process
        process = subprocess.Popen([binary, "--foreground", "--config", str(config)], stdout=log, stderr=log)
        for _ in range(100):
            if process.poll() is not None:
                raise RuntimeError("daemon exited during startup")
            try:
                if rpc({"op": "ping"})["ok"]:
                    return
            except (FileNotFoundError, ConnectionRefusedError):
                pass
            time.sleep(0.05)
        raise RuntimeError("daemon did not become ready")

    def stop():
        global process
        if process is not None:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
                raise
            process = None

    try:
        start()
        assert rpc({"op": "policy.info"})["enforce"] is True
        assert rpc({"op": "session.create", "id": "persistent"})["ok"]
        request = {"op": "submit", "request_id": "once", "agent": "missing", "message": "test", "session_id": "persistent"}
        assert rpc(request)["error_kind"] == "approval_required"
        assert rpc({"op": "approval.decide", "id": "once", "allow": True})["ok"]
        response = rpc(request)
        assert response["error_kind"] == "unknown_agent"
        assert rpc(request) == response
        assert rpc({**request, "message": "changed"})["error_kind"] == "idempotency_conflict"
        assert rpc({"op": "channel.open", "id": "viewer"})["ok"]
        page = rpc({"op": "channel.read", "id": "viewer", "limit": 2})
        assert len(page["events"]) == 2
        assert rpc({"op": "channel.ack", "id": "viewer", "cursor": page["next_cursor"]})["ok"]
        expected = rpc({"op": "channel.read", "id": "viewer"})
        stop()
        start()
        assert rpc(request) == response
        assert rpc({"op": "session.get", "id": "persistent"})["ok"]
        assert rpc({"op": "channel.read", "id": "viewer"}) == expected
        assert rpc({"op": "audit.verify"})["ok"]
        # A second daemon must not unlink the first daemon's live socket.
        second = subprocess.run([binary, "--foreground", "--config", str(config)], stdout=log, stderr=log, timeout=10)
        assert second.returncode != 0
        assert rpc({"op": "ping"})["ok"]
    except Exception:
        log.flush()
        print((root / "daemon.log").read_text(), file=sys.stderr)
        raise
    finally:
        stop()
        log.close()
print("control-plane IPC and restart integration passed")
