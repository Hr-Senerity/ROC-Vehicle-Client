"""Test-only HTTP/WS fault proxy for one explicitly configured Device.

Uses protected files, emits only closed aliases/digests/counters, never installs
itself as a gateway. HTTP Demo only; not a TLS verification bypass or production
transport. No management requests, task discovery, sampling, or auto resolve.
"""
import argparse
import hashlib
import http.client
import http.server
import json
import math
import os
import re
import select
import socket
import stat
import threading
import time
import urllib.parse

MODES = {"pass", "offline", "ws_flap", "artifact_reset_once", "artifact_timeout_once",
         "artifact_hash", "artifact_mime", "artifact_missing", "manifest_schema",
         "status_drop_hold", "status_invalid_lease", "artifact_hold", "artifact_length", "manifest_package"}
MAX_BODY = 4 * 1024 * 1024


def private_bytes(path, limit=65536):
    parent = os.lstat(os.path.dirname(path))
    if not stat.S_ISDIR(parent.st_mode) or stat.S_IMODE(parent.st_mode) != 0o700 or parent.st_uid != os.getuid():
        raise ValueError("private_parent_invalid")
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
    try:
        s = os.fstat(fd)
        if not stat.S_ISREG(s.st_mode) or stat.S_IMODE(s.st_mode) != 0o600 or s.st_uid != os.getuid() or s.st_nlink != 1 or s.st_size > limit:
            raise ValueError("private_file_invalid")
        return os.read(fd, limit + 1)
    finally:
        os.close(fd)


def alias(value):
    return hashlib.sha256(value.encode()).hexdigest()[:12]


class Frames:
    def __init__(self, callback):
        self.buffer = bytearray()
        self.message = bytearray()
        self.callback = callback

    def feed(self, data):
        self.buffer.extend(data)
        while len(self.buffer) >= 2:
            first, second = self.buffer[:2]
            length, offset = second & 127, 2
            if length == 126:
                if len(self.buffer) < 4:
                    return
                length, offset = int.from_bytes(self.buffer[2:4], "big"), 4
            elif length == 127:
                if len(self.buffer) < 10:
                    return
                length, offset = int.from_bytes(self.buffer[2:10], "big"), 10
            if length > 65536:
                raise ValueError("websocket_observation_budget")
            masked = bool(second & 128)
            total = offset + (4 if masked else 0) + length
            if len(self.buffer) < total:
                return
            mask = bytes(self.buffer[offset:offset + 4]) if masked else b""
            start = offset + len(mask)
            payload = bytes(self.buffer[start:total])
            del self.buffer[:total]
            if masked:
                payload = bytes(v ^ mask[i % 4] for i, v in enumerate(payload))
            opcode = first & 15
            if opcode in (0, 1):
                self.message.extend(payload)
                if len(self.message) > 65536:
                    raise ValueError("websocket_message_budget")
                if first & 128:
                    try:
                        self.callback(json.loads(self.message))
                    except (ValueError, UnicodeDecodeError):
                        pass
                    self.message.clear()


class State:
    def __init__(self, config_path, token_path, control):
        config = json.loads(private_bytes(config_path))
        u = urllib.parse.urlsplit(config["base_url"])
        if config.get("transport") != "http-ws-demo" or u.scheme != "http" or u.username or u.password or u.query or u.fragment:
            raise ValueError("explicit_http_demo_required")
        self.host, self.port = u.hostname, u.port or 80
        self.token = private_bytes(token_path, 4096).rstrip(b"\r\n").decode("ascii")
        self.control = control
        self.lock = threading.RLock()
        self.generation = None
        self.used = 0
        self.first_events = {}  # Lease/body stay in memory only, never in receipts.

    def plan(self):
        try:
            p = json.loads(private_bytes(self.control + "/plan.json"))
            if set(p) - {"generation", "mode", "count", "delay_seconds"} or p["mode"] not in MODES or not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", p["generation"]):
                raise ValueError("plan_invalid")
            if type(p.get("count", 1)) is not int or not 1 <= p.get("count", 1) <= 10:
                raise ValueError("plan_count_invalid")
            delay = p.get("delay_seconds", 2)
            if type(delay) not in (int, float) or not math.isfinite(delay) or not 0 <= delay <= 5:
                raise ValueError("plan_delay_invalid")
            with self.lock:
                if self.generation != p["generation"]:
                    self.generation, self.used = p["generation"], 0
            return p
        except (OSError, ValueError, KeyError):
            return {"generation": "invalid", "mode": "offline"}

    def take(self, mode, count=1):
        p = self.plan()
        with self.lock:
            if p["mode"] == mode and self.used < count:
                self.used += 1
                return True
        return False

    def record(self, code, **fields):
        item = {"code": code, "monotonic_ns": time.monotonic_ns(), "generation": self.plan()["generation"]}
        item.update(fields)
        with self.lock:
            fd = os.open(self.control + "/events.jsonl", os.O_WRONLY | os.O_APPEND | os.O_CREAT | os.O_NOFOLLOW, 0o600)
            try:
                s = os.fstat(fd)
                if not stat.S_ISREG(s.st_mode) or s.st_uid != os.getuid() or s.st_nlink != 1 or stat.S_IMODE(s.st_mode) != 0o600:
                    raise ValueError("receipt_permissions_invalid")
                os.write(fd, (json.dumps(item, separators=(",", ":")) + "\n").encode())
                os.fsync(fd)
                directory=os.open(self.control,os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW)
                try:os.fsync(directory)
                finally:os.close(directory)
            finally:
                os.close(fd)


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args):
        pass

    def do_GET(self):
        self.handle_device()

    def do_POST(self):
        self.handle_device()

    def reply(self, status, body, headers=(), declared_size=None):
        self.send_response(status)
        for key, value in headers:
            if key.lower() in {"content-type", "x-content-sha256", "cache-control", "x-content-type-options"}:
                self.send_header(key, value)
        self.send_header("Content-Length", str(len(body) if declared_size is None else declared_size))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)
        self.close_connection = True

    def handle_device(self):
        state = self.server.state
        try:
            if self.headers.get("Authorization") != "Device " + state.token:
                self.reply(401, b'{"ok":false,"code":"authentication_failed","message":"test proxy rejected identity"}', [("Content-Type", "application/json")])
                return
            if self.path == "/ws/device" and self.headers.get("Upgrade", "").lower() == "websocket":
                self.websocket()
                return
            match = re.fullmatch(r"/api/device/tasks/([0-9a-f-]{36})/(accept|manifest|status|artifact)(?:/([0-9a-f-]{36}))?", self.path)
            if not match or (self.command == "POST") != (match[2] in ("accept", "status")):
                self.reply(404, b"", [])
                return
            task, kind = alias(match[1]), match[2]
            if state.plan()["mode"] == "offline":
                state.record("local_offline", kind=kind, task_alias=task)
                self.reply(503, b"", [])
                return
            length = int(self.headers.get("Content-Length", "0"))
            if length < 0 or length > 65536:
                self.reply(413, b"", [])
                return
            body = self.rfile.read(length) if length else None
            headers = {k: v for k, v in self.headers.items() if k.lower() not in {"host", "connection", "content-length", "proxy-connection", "transfer-encoding", "upgrade"}}
            terminal = False
            fields = {}
            if kind == "status":
                request = json.loads(body)
                terminal = request.get("state") in ("delivered", "failed")
                fields = {"state": request.get("state"), "progress": request.get("progress"), "event_alias": alias(request["event_id"]), "request_digest": hashlib.sha256(body).hexdigest()}
                with state.lock:
                    previous = state.first_events.setdefault((task, request["event_id"]), (body, headers.get("X-Task-Lease")))
                fields["exact_body_and_lease_as_first"] = previous == (body, headers.get("X-Task-Lease"))
                if terminal and state.take("status_invalid_lease"):
                    headers["X-Task-Lease"] = "deliberately-invalid-test-lease"
                    fields["wire_lease_fault_injected"] = True
            if kind == "artifact" and state.plan()["mode"] == "artifact_missing":
                state.record("artifact_missing_injected", task_alias=task)
                self.reply(404, b'{"ok":false,"code":"artifact_not_found","message":"isolated injected absence"}', [("Content-Type", "application/json")])
                return
            connection = http.client.HTTPConnection(state.host, state.port, timeout=15)
            connection.request(self.command, self.path, body, headers)
            response = connection.getresponse()
            status, response_headers = response.status, response.getheaders()
            payload = response.read(MAX_BODY + 1)
            connection.close()
            if len(payload) > MAX_BODY:
                raise ValueError("proxy_response_budget")
            if kind in ("accept", "status"):
                parsed = json.loads(payload)
                if parsed.get("ok") is True:
                    fields.update(replayed=parsed["replayed"], attempt=parsed["task"]["attempt"])
                elif parsed.get("code") in {"invalid_lease", "lease_expired", "invalid_state", "invalid_transition", "event_conflict", "authentication_failed", "internal_error"}:
                    fields["server_code"] = parsed["code"]
            state.record("upstream_response", kind=kind, task_alias=task, http_status=status, **fields)
            if kind == "artifact" and state.plan()["mode"] == "artifact_hold":
                state.record("artifact_response_held", task_alias=task)
                while state.plan()["mode"] == "artifact_hold":
                    if select.select([self.connection], [], [], 0.02)[0] and not self.connection.recv(1, socket.MSG_PEEK):
                        break
                self.close_connection = True
                return
            if kind == "status" and terminal and state.take("status_drop_hold"):
                state.record("terminal_upstream_processed_response_held", task_alias=task, http_status=status, **fields)
                # Exact operator kill window, not a random post-log race.
                while state.plan()["mode"] == "status_drop_hold":
                    if select.select([self.connection], [], [], 0.02)[0] and not self.connection.recv(1, socket.MSG_PEEK):
                        break
                self.close_connection = True
                return
            if kind == "manifest" and state.plan()["mode"] == "manifest_schema" and status == 200:
                parsed = json.loads(payload)
                parsed["manifest"]["schema_version"] = 99
                payload = json.dumps(parsed, separators=(",", ":")).encode()
                state.record("manifest_schema_injected", task_alias=task)
            if kind == "manifest" and state.plan()["mode"] == "manifest_package" and status == 200:
                parsed = json.loads(payload)
                parsed["manifest"]["package_version"] = 99
                payload = json.dumps(parsed, separators=(",", ":")).encode()
                state.record("manifest_package_injected", task_alias=task)
            if kind == "artifact" and status == 200:
                if state.take("artifact_reset_once"):
                    self.send_response(status)
                    for key, value in response_headers:
                        if key.lower() in {"content-type", "x-content-sha256"}:
                            self.send_header(key, value)
                    self.send_header("Content-Length", str(len(payload)))
                    self.send_header("Connection", "close")
                    self.end_headers()
                    self.wfile.write(payload[:max(1, len(payload) // 2)])
                    self.wfile.flush()
                    state.record("artifact_truncated_once", task_alias=task)
                    self.close_connection = True
                    return
                if state.take("artifact_timeout_once"):
                    state.record("artifact_delay_once", task_alias=task)
                    time.sleep(min(max(float(state.plan().get("delay_seconds", 2)), 0), 5))
                if state.plan()["mode"] == "artifact_hash":
                    payload = bytes([payload[0] ^ 1]) + payload[1:]
                    state.record("artifact_hash_injected", task_alias=task)
                if state.plan()["mode"] == "artifact_mime":
                    response_headers = [(k, v) for k, v in response_headers if k.lower() != "content-type"] + [("Content-Type", "application/octet-stream")]
                    state.record("artifact_mime_injected", task_alias=task)
                if state.plan()["mode"] == "artifact_length":
                    state.record("artifact_length_injected", task_alias=task)
                    self.reply(status,payload,response_headers,len(payload)+1)
                    return
            self.reply(status, payload, response_headers)
        except (OSError, ValueError, KeyError, http.client.HTTPException):
            self.close_connection = True

    def websocket(self):
        state = self.server.state
        if state.plan()["mode"] == "offline":
            self.reply(503, b"", [])
            return
        upstream = socket.create_connection((state.host, state.port), timeout=15)
        try:
            values = {k: v for k, v in self.headers.items() if k.lower() != "host"}
            values["Host"] = state.host + ":" + str(state.port)
            request = "GET /ws/device HTTP/1.1\r\n" + "".join(k + ": " + v + "\r\n" for k, v in values.items()) + "\r\n"
            upstream.sendall(request.encode("ascii"))
            data = bytearray()
            while b"\r\n\r\n" not in data:
                chunk = upstream.recv(4096)
                if not chunk:
                    raise ValueError("upstream_handshake_eof")
                data.extend(chunk)
                if len(data) > 65536:
                    raise ValueError("handshake_budget")
            head, tail = bytes(data).split(b"\r\n\r\n", 1)
            if not head.startswith(b"HTTP/1.1 101 "):
                state.record("websocket_upstream_rejected")
                self.connection.sendall(bytes(data))
                return
            hello = [False]
            def server_message(message):
                kind = message.get("type")
                if kind == "hello":
                    hello[0] = True
                    state.record("server_hello", last_client_sequence=message.get("payload", {}).get("last_client_sequence"))
                elif kind == "ack" and message.get("payload", {}).get("accepted_type") in ("heartbeat", "telemetry"):
                    state.record("server_ack", accepted_type=message["payload"]["accepted_type"], duplicate=message["payload"]["duplicate"])
                elif kind == "task.available":
                    state.record("server_task_available", task_alias=alias(message["payload"]["task_id"]), notification_attempt=message["payload"]["attempt"])
            server_frames = Frames(server_message)
            def client_message(message):
                kind=message.get("type")
                if kind not in ("heartbeat","telemetry"):
                    return
                sequence=message.get("sequence")
                valid=isinstance(sequence,str) and bool(re.fullmatch(r"[1-9][0-9]{0,18}",sequence)) and int(sequence)<=9223372036854775807
                state.record("client_uplink",kind=kind,sequence=sequence if valid else None,positive_int64=valid)
            client_frames=Frames(client_message)
            self.connection.sendall(head + b"\r\n\r\n" + tail)
            server_frames.feed(tail)
            upstream.settimeout(None)
            self.connection.settimeout(None)
            state.record("websocket_connected")
            while True:
                plan = state.plan()
                if plan["mode"] == "offline" or (hello[0] and state.take("ws_flap", min(int(plan.get("count", 3)), 10))):
                    state.record("websocket_injected_disconnect")
                    return
                readable, _, _ = select.select([upstream, self.connection], [], [], 0.02)
                for source in readable:
                    payload = source.recv(65536)
                    if not payload:
                        return
                    destination = self.connection if source is upstream else upstream
                    destination.sendall(payload)
                    if source is upstream:
                        server_frames.feed(payload)
                    else:
                        client_frames.feed(payload)
        finally:
            upstream.close()
            self.close_connection = True


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def handle_error(self, *_args):
        pass  # Never print an exception containing a target or request.


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", required=True)
    parser.add_argument("--token-file", required=True)
    parser.add_argument("--control", required=True)
    parser.add_argument("--port", type=int, default=18080)
    args = parser.parse_args()
    server = Server(("0.0.0.0", args.port), Handler)
    server.state = State(args.config, args.token_file, args.control)
    print("integration_test_proxy_ready", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
