#!/usr/bin/env python3
"""Faultline dashboard server. Python 3 standard library only.

Connects to fl-bridge on the QNX target (TCP 7700), keeps the latest state and
the event log, and serves the dashboard on http://localhost:8800.

    python3 dashboard/server.py [--target IP] [--port 8800]

The target address defaults to the one tools/vm.sh saved in ~/fl-vm/.ip.
"""
import argparse
import json
import os
import queue
import socket
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import campaign  # noqa: E402

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent


class Target:
    """Keeps one connection to fl-bridge, and fans its lines out to listeners."""

    def __init__(self, host, port=7700):
        self.host, self.port = host, port
        self.lock = threading.Lock()
        self.sock = None
        self.state = None
        self.events = {}            # seq -> event, so a reconnect replay doesn't duplicate
        self.history = []           # (t_ms, flow, target, late_us, mode) samples for the chart
        self.listeners = set()
        self.acks = queue.Queue()
        self.connected = False
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self):
        while True:
            try:
                s = socket.create_connection((self.host, self.port), timeout=5)
                s.settimeout(None)
                with self.lock:
                    self.sock = s
                    self.connected = True
                self._publish({"kind": "link", "connected": True, "target": self.host})
                buf = b""
                while True:
                    chunk = s.recv(65536)
                    if not chunk:
                        break
                    buf += chunk
                    while b"\n" in buf:
                        line, buf = buf.split(b"\n", 1)
                        self._line(line)
            except OSError:
                pass
            with self.lock:
                self.sock = None
                self.connected = False
            self._publish({"kind": "link", "connected": False, "target": self.host})
            time.sleep(1)

    def _line(self, raw):
        try:
            msg = json.loads(raw)
        except ValueError:
            return
        kind = msg.get("kind")
        if kind == "state":
            with self.lock:
                self.state = msg
                c = msg["ctrl"]
                self.history.append((msg["t_ms"], msg["plant"]["flow_mlh"], c["target_mlh"],
                                     c["last_late_us"], c["mode"]))
                if len(self.history) > 1200:
                    del self.history[:200]
        elif kind == "event":
            with self.lock:
                if msg["seq"] in self.events:
                    return
                self.events[msg["seq"]] = msg
                if len(self.events) > 2000:
                    for k in sorted(self.events)[:500]:
                        del self.events[k]
        elif kind == "ack":
            self.acks.put(msg)
        self._publish(msg)

    def _publish(self, msg):
        for q in list(self.listeners):
            try:
                q.put_nowait(msg)
            except queue.Full:
                pass

    def send(self, cmd, timeout=2.0):
        """Sends one fault command and waits for the bridge's ack."""
        with self.lock:
            s = self.sock
        if not s:
            return {"ok": False, "error": "not connected to the target"}
        while not self.acks.empty():
            self.acks.get_nowait()
        s.sendall((cmd.strip() + "\n").encode())
        end = time.time() + timeout
        while time.time() < end:
            try:
                ack = self.acks.get(timeout=max(0.01, end - time.time()))
            except queue.Empty:
                break
            if ack.get("cmd") == cmd.strip():
                return ack
        return {"ok": False, "error": "no answer from the target"}

    def snapshot(self):
        with self.lock:
            return {
                "connected": self.connected,
                "target": self.host,
                "state": self.state,
                "events": [self.events[k] for k in sorted(self.events)][-300:],
                "history": self.history[-600:],
            }

    def events_since(self, seq):
        with self.lock:
            return [self.events[k] for k in sorted(self.events) if k >= seq]


class Handler(BaseHTTPRequestHandler):
    target = None
    runner = None

    def log_message(self, fmt, *args):  # keep the console quiet
        pass

    def _json(self, obj, code=200):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _file(self, path, ctype):
        try:
            body = path.read_bytes()
        except OSError:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _body(self):
        n = int(self.headers.get("Content-Length") or 0)
        if n > 10000:
            return {}
        try:
            return json.loads(self.rfile.read(n) or b"{}")
        except ValueError:
            return {}

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path in ("/", "/index.html"):
            return self._file(HERE / "index.html", "text/html; charset=utf-8")
        if path == "/api/snapshot":
            return self._json(self.target.snapshot())
        if path == "/api/campaign":
            return self._json(self.runner.status())
        if path.startswith("/reports/"):
            name = Path(path).name
            ctype = "text/html; charset=utf-8" if name.endswith(".html") else "text/markdown; charset=utf-8"
            return self._file(ROOT / "reports" / name, ctype)
        if path == "/stream":
            return self._stream()
        self.send_error(404)

    def do_POST(self):
        path = self.path.split("?", 1)[0]
        if path == "/api/inject":
            cmd = str(self._body().get("cmd", ""))[:120]
            if not cmd.strip():
                return self._json({"ok": False, "error": "empty command"}, 400)
            return self._json(self.target.send(cmd))
        if path == "/api/campaign":
            name = str(self._body().get("name", "pump"))
            return self._json(self.runner.start(name))
        self.send_error(404)

    def _stream(self):
        q = queue.Queue(maxsize=2000)
        self.target.listeners.add(q)
        try:
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            while True:
                try:
                    msg = q.get(timeout=10)
                    data = "data: " + json.dumps(msg) + "\n\n"
                except queue.Empty:
                    data = ": keepalive\n\n"
                self.wfile.write(data.encode())
                self.wfile.flush()
        except OSError:
            pass
        finally:
            self.target.listeners.discard(q)


def default_target():
    ip_file = Path(os.path.expanduser("~/fl-vm/.ip"))
    if ip_file.exists():
        return ip_file.read_text().strip()
    return "192.168.122.80"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--target", default=default_target())
    ap.add_argument("--port", type=int, default=8800)
    args = ap.parse_args()
    target = Target(args.target)
    Handler.target = target
    Handler.runner = campaign.Runner(target, ROOT / "campaigns", ROOT / "reports")
    server = ThreadingHTTPServer(("0.0.0.0", args.port), Handler)
    print(f"Faultline dashboard on http://localhost:{args.port}  (target {args.target})", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
