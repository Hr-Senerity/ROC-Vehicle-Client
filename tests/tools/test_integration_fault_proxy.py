import importlib.util
import json
import http.client
import http.server
import os
import pathlib
import socket
import tempfile
import threading
import time
import unittest

spec = importlib.util.spec_from_file_location("proxy", pathlib.Path(__file__).with_name("integration_fault_proxy.py"))
proxy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(proxy)
TOKEN = "synthetic-proxy-test-token-never-real"
TASK = "00000000-0000-4000-8000-000000000001"


class Upstream(http.server.BaseHTTPRequestHandler):
    protocol_version="HTTP/1.1"
    def log_message(self, *_args):
        pass

    def do_GET(self):
        if self.path=="/ws/device":
            if getattr(self.server,"handshake_eof",False):
                self.close_connection=True
                return
            self.send_response(101)
            self.send_header("Upgrade","websocket")
            self.send_header("Connection","Upgrade")
            self.end_headers()
            raw=b'{"type":"hello","payload":{}}'
            self.wfile.write(bytes([0x81,len(raw)])+raw);self.wfile.flush()
            self.connection.settimeout(0.2)
            try:
                self.connection.recv(1)
            except OSError:
                pass
            self.close_connection=True
            return
        if self.path.endswith("manifest"):
            data = b'{"ok":true,"manifest":{"schema_version":2}}'
        else:
            data = b"original-fixture-bytes"
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("X-Content-SHA256", "0" * 64)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        self.server.requests.append((body, self.headers.get("X-Task-Lease")))
        bad = self.headers.get("X-Task-Lease") == "deliberately-invalid-test-lease"
        data = json.dumps({"ok": False, "code": "invalid_lease", "message": "fixture"} if bad else {"ok": True, "replayed": len(self.server.requests) > 1, "task": {"attempt": 1}}).encode()
        self.send_response(401 if bad else 200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


class Tests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = self.temp.name
        os.chmod(self.root, 0o700)
        self.up = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Upstream)
        self.up.requests = []
        self.up_thread = threading.Thread(target=self.up.serve_forever, daemon=True)
        self.up_thread.start()
        self.write("config.json", {"base_url": "http://127.0.0.1:" + str(self.up.server_port), "transport": "http-ws-demo"})
        self.write("token", TOKEN, raw=True)
        self.plan("pass")
        self.server = proxy.Server(("127.0.0.1", 0), proxy.Handler)
        self.server.state = proxy.State(self.root + "/config.json", self.root + "/token", self.root)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def tearDown(self):
        self.plan("pass")
        self.server.shutdown()
        self.server.server_close()
        self.up.shutdown()
        self.up.server_close()
        self.temp.cleanup()

    def write(self, name, value, raw=False):
        path = self.root + "/" + name
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "w") as f:
            f.write(value if raw else json.dumps(value))

    def plan(self, mode):
        self.write("plan.json", {"generation": mode, "mode": mode})

    def request(self, kind="artifact", token=TOKEN, body=None, lease="synthetic-test-lease"):
        c = http.client.HTTPConnection("127.0.0.1", self.server.server_port, timeout=1)
        headers = {"Authorization": "Device " + token, "X-Task-Lease": lease}
        try:
            c.request("POST" if body else "GET", "/api/device/tasks/" + TASK + "/" + kind, body, headers)
            r = c.getresponse()
            status, mime = r.status, r.getheader("Content-Type")
            data = r.read()
            return status, mime, data
        finally:
            c.close()

    def test_identity_and_no_management_forward(self):
        self.assertEqual(self.request(token="wrong")[0], 401)
        c = http.client.HTTPConnection("127.0.0.1", self.server.server_port)
        c.request("GET", "/api/projects", headers={"Authorization": "Device " + TOKEN})
        self.assertEqual(c.getresponse().status, 404)
        c.close()

    def test_bytes_unchanged(self):
        self.assertEqual(self.request()[2], b"original-fixture-bytes")

    def test_hash_mutation(self):
        self.plan("artifact_hash")
        result = self.request()[2]
        self.assertEqual(len(result), len(b"original-fixture-bytes"))
        self.assertNotEqual(result, b"original-fixture-bytes")

    def test_mime_missing_and_schema(self):
        self.plan("artifact_mime")
        self.assertEqual(self.request()[1], "application/octet-stream")
        self.plan("artifact_missing")
        self.assertEqual(self.request()[0], 404)
        self.plan("manifest_schema")
        self.assertEqual(json.loads(self.request("manifest")[2])["manifest"]["schema_version"], 99)

    def test_truncation_once_then_exact_retry(self):
        self.plan("artifact_reset_once")
        with self.assertRaises(http.client.IncompleteRead):
            self.request()
        self.assertEqual(self.request()[2], b"original-fixture-bytes")

    def test_offline_and_invalid_plan_fail_closed(self):
        self.plan("offline")
        self.assertEqual(self.request()[0], 503)
        self.write("plan.json", {"generation":"bad", "mode":"pass", "count":0})
        self.assertEqual(self.request()[0], 503)

    def test_length_and_package_faults(self):
        self.plan("artifact_length")
        with self.assertRaises(http.client.IncompleteRead):self.request()
        self.plan("manifest_package")
        self.assertEqual(json.loads(self.request("manifest")[2])["manifest"]["package_version"],99)

    def test_artifact_hold_disconnect_does_not_reply(self):
        self.plan("artifact_hold")
        with self.assertRaises(TimeoutError):self.request()
        self.plan("pass")
        self.assertEqual(self.request()[2],b"original-fixture-bytes")

    def test_delay_is_once(self):
        self.write("plan.json", {"generation":"delay", "mode":"artifact_timeout_once", "delay_seconds":0.05})
        start=time.monotonic()
        self.assertEqual(self.request()[2], b"original-fixture-bytes")
        self.assertGreaterEqual(time.monotonic()-start, 0.05)
        self.assertEqual(self.request()[2], b"original-fixture-bytes")

    def test_terminal_processed_but_response_held(self):
        self.plan("status_drop_hold")
        body=json.dumps({"event_id": TASK, "state": "delivered", "progress":100}).encode()
        c=http.client.HTTPConnection("127.0.0.1",self.server.server_port,timeout=0.15)
        c.request("POST","/api/device/tasks/"+TASK+"/status",body,
                  {"Authorization":"Device "+TOKEN,"X-Task-Lease":"synthetic-test-lease"})
        with self.assertRaises(TimeoutError):
            c.getresponse()
        c.close()
        with open(self.root+"/events.jsonl") as receipt:
            records=[json.loads(line) for line in receipt]
        held=[r for r in records if r["code"]=="terminal_upstream_processed_response_held"]
        self.assertEqual(len(held),1)
        self.assertEqual(held[0]["http_status"],200)
        self.plan("pass")
        self.assertEqual(self.request("status",body=body)[0],200)

    def test_lease_fault_and_exact_identity_receipts(self):
        self.plan("status_invalid_lease")
        body=json.dumps({"event_id": TASK, "state": "delivered", "progress": 100}).encode()
        self.assertEqual(self.request("status",body=body)[0], 401)
        self.plan("pass")
        self.assertEqual(self.request("status",body=body)[0], 200)
        with open(self.root+"/events.jsonl") as receipt:
            text=receipt.read()
        lines=[json.loads(line) for line in text.splitlines()]
        responses=[line for line in lines if line["code"]=="upstream_response"]
        self.assertTrue(all(r["exact_body_and_lease_as_first"] for r in responses))
        self.assertNotIn(TOKEN,text)
        self.assertNotIn("synthetic-test-lease",text)
        self.assertNotIn(TASK,text)

    def test_fragmented_masked_frames(self):
        results=[];frames=proxy.Frames(results.append)
        raw=b'{"type":"hello"}'
        mask=b"abcd"
        wire=bytes([0x81,0x80|len(raw)])+mask+bytes(v^mask[i%4] for i,v in enumerate(raw))
        for value in wire:
            frames.feed(bytes([value]))
        self.assertEqual(results,[{"type":"hello"}])

    def test_websocket_relay_flap_and_handshake_eof(self):
        for mode in ("pass","ws_flap"):
            self.plan(mode)
            with socket.create_connection(("127.0.0.1",self.server.server_port),timeout=1) as c:
                c.sendall(("GET /ws/device HTTP/1.1\r\nHost: fixture\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nAuthorization: Device "+TOKEN+"\r\n\r\n").encode())
                data=bytearray()
                while True:
                    chunk=c.recv(4096)
                    if not chunk:break
                    data.extend(chunk)
                self.assertIn(b"101 Switching Protocols",data)
                self.assertIn(b'"type":"hello"',data)
        with open(self.root+"/events.jsonl") as receipt:
            records=[json.loads(line) for line in receipt]
        self.assertEqual(sum(r['code']=='server_hello' for r in records),2)
        self.assertEqual(sum(r['code']=='websocket_injected_disconnect' for r in records),1)
        self.up.handshake_eof=True;self.plan("pass")
        with socket.create_connection(("127.0.0.1",self.server.server_port),timeout=1) as c:
            c.sendall(("GET /ws/device HTTP/1.1\r\nHost: fixture\r\nUpgrade: websocket\r\nAuthorization: Device "+TOKEN+"\r\n\r\n").encode())
            self.assertEqual(c.recv(1),b'')

    def test_private_permissions_rejected(self):
        os.chmod(self.root+"/token",0o644)
        with self.assertRaises(ValueError):
            proxy.State(self.root+"/config.json",self.root+"/token",self.root)


if __name__ == "__main__":
    unittest.main()
