#!/usr/bin/env python3
"""Tests for the zeroctl model step. No network, no API key: python3 test_zeroctl.py"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import zeroctl  # noqa: E402

STATE = {
    "mercury_cpu": 0,
    "allocatable": [1, 2, 3, 4, 5, 6, 7],
    "interval_s": 1,
    "move_level": "node",
    "max_cpus": 7,
    "groups": [
        {"name": "socket.0", "level": "socket", "parent": "", "linux_cpus": [0, 1, 2, 3, 4, 5, 6, 7], "allocatable": 7,
         "loads": [0.5, 0.5, 0.5, 0.5, 0.5], "daemons": ["svc0", "svc1"]},
        {"name": "node.0", "level": "node", "parent": "socket.0", "linux_cpus": [0, 1, 2, 3], "allocatable": 3,
         "loads": [0.9, 0.95, 0.97, 0.99, 1.0], "daemons": ["svc0", "svc1"]},
        {"name": "node.1", "level": "node", "parent": "socket.0", "linux_cpus": [4, 5, 6, 7], "allocatable": 4,
         "loads": [0.1, 0.1, None, 0.1, 0.12], "daemons": []},
    ],
    "daemons": [
        {"daemon_id": "svc0", "exec_path": "/x/hello_server", "cpus": [1], "groups": ["socket.0", "node.0"],
         "scale": {"enabled": False, "min": 1, "max": 16}, "components": [{"name": "extra", "cost": 0.3, "enabled": True}]},
        {"daemon_id": "svc1", "exec_path": "/x/hello_server", "cpus": [2], "groups": ["socket.0", "node.0"],
         "scale": {"enabled": False, "min": 1, "max": 16}, "components": []},
    ],
    "history": [{"at_seconds": 20, "action": "move", "workload": "svc1", "node": "node.0", "error": "svc1 already runs on node.0"}],
    "events": [{"tick": 4, "policy": "shift", "action": "move", "daemon_id": "svc1", "from": "node.1", "to": "node.0"}],
}


class Prompt(unittest.TestCase):
    def test_sections(self):
        prompt = zeroctl.build_prompt(STATE, {"now": {"seconds": 20, "requests": 300, "p50_ms": 140.0, "p95_ms": 170.0,
                                                       "error_rate": 0.0, "requests_per_s": 15.0},
                                              "reference": {"p50_ms": 95.0, "p95_ms": 110.0, "error_rate": 0.0}})
        for heading in ("## Situation", "## CPU groups", "## Daemons", "## History", "## Rules", "## Actions", "## Output"):
            self.assertIn(heading, prompt)
        self.assertIn("now (last 20s, 300 requests): p50 140.0 ms, p95 170.0 ms, errors 0.0%, 15.0 req/s", prompt)
        self.assertIn("healthy reference: p50 95.0 ms", prompt)
        self.assertIn("node.1  node  load 10% 10% -- 10% 12%  allocatable 4", prompt)
        self.assertIn("svc0  cpus 1 (1)  on node.0  scale off  extra on cost 0.3", prompt)
        self.assertIn("t+20s  move svc1 node.0  FAILED: svc1 already runs on node.0", prompt)
        self.assertIn("Never use CPU 0", prompt)
        self.assertIn("action: move | scale_out | degrade | restore | none | stop", prompt)
        self.assertIn("Daemons you may change: svc0, svc1.", prompt)
        self.assertNotIn("{", prompt.split("## Output")[1])


class Parse(unittest.TestCase):
    def test_fenced_yaml(self):
        text = "Here you go:\n```yaml\naction: move\nworkload: svc0\nnode: node.1   # coolest\nreason: 'node.0 is hot'\n```\n"
        self.assertEqual(zeroctl.to_action(zeroctl.parse_docs(text)),
                         {"action": "move", "workload": "svc0", "node": "node.1", "reason": "node.0 is hot"})

    def test_json(self):
        text = '{"action": "scale_out", "workload": "svc0", "cpus": 2}'
        self.assertEqual(zeroctl.to_action(zeroctl.parse_docs(text)), {"action": "scale_out", "workload": "svc0", "cpus": 2})

    def test_replicas_alias(self):
        doc = zeroctl.to_action(zeroctl.parse_docs("action: scale_out\nworkload: svc0\nreplicas: 3\n"))
        self.assertEqual(doc["cpus"], 3)

    def test_garbage_is_none(self):
        self.assertEqual(zeroctl.to_action(zeroctl.parse_docs("I think you should buy more servers."))["action"], "none")

    def test_unknown_action_is_none(self):
        doc = zeroctl.to_action(zeroctl.parse_docs("action: add_worker\nspawn: true\n"))
        self.assertEqual(doc["action"], "none")
        self.assertIn("add_worker", doc["error"])

    def test_bad_cpus_is_none(self):
        self.assertEqual(zeroctl.to_action(zeroctl.parse_docs("action: scale_out\nworkload: a\ncpus: lots\n"))["action"], "none")

    def test_first_action_document(self):
        text = "reason: preface\n---\naction: degrade\nworkload: svc0\ncomponent: extra\n---\naction: none\n"
        self.assertEqual(zeroctl.to_action(zeroctl.parse_docs(text))["action"], "degrade")


class Mock(unittest.TestCase):
    def test_moves_off_hot_node(self):
        doc = zeroctl.to_action(zeroctl.parse_docs(zeroctl.mock_reply(STATE)))
        self.assertEqual((doc["action"], doc["workload"], doc["node"]), ("move", "svc0", "node.1"))

    def test_calm_is_none(self):
        calm = json.loads(json.dumps(STATE))
        calm["groups"][1]["loads"] = [0.2] * 5
        self.assertEqual(zeroctl.to_action(zeroctl.parse_docs(zeroctl.mock_reply(calm)))["action"], "none")

    def test_replay(self):
        with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as handle:
            handle.write("action: restore\nworkload: svc0\ncomponent: extra\n")
        os.environ["ZEROCTL_MOCK_RESPONSE"] = handle.name
        try:
            self.assertEqual(zeroctl.step(STATE, "mock")["action"], "restore")
        finally:
            del os.environ["ZEROCTL_MOCK_RESPONSE"]
            os.unlink(handle.name)


class FakeProvider(BaseHTTPRequestHandler):
    seen: list = []

    def do_POST(self):  # noqa: N802
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])).decode())
        FakeProvider.seen.append({"path": self.path, "auth": self.headers.get("Authorization"),
                                  "goog": self.headers.get("x-goog-api-key"), "body": body})
        text = "```yaml\naction: degrade\nworkload: svc0\ncomponent: extra\nreason: shed\n```"
        if "generateContent" in self.path:
            reply = {"candidates": [{"content": {"parts": [{"text": text}]}}]}
        else:
            reply = {"choices": [{"message": {"content": text}}]}
        data = json.dumps(reply).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *args):
        pass


class Providers(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = HTTPServer(("127.0.0.1", 0), FakeProvider)
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()
        cls.base = f"http://127.0.0.1:{cls.server.server_port}"

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()

    def setUp(self):
        FakeProvider.seen.clear()
        self.saved = {k: os.environ.get(k) for k in ("OPENAI_API_KEY", "OPENAI_BASE_URL", "OPENAI_MODEL",
                                                     "GEMINI_API_KEY", "GEMINI_BASE_URL", "GEMINI_MODEL")}

    def tearDown(self):
        for key, value in self.saved.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value

    def test_openai_shape(self):
        os.environ.update(OPENAI_API_KEY="test-key", OPENAI_BASE_URL=self.base + "/v1", OPENAI_MODEL="m-test")
        action = zeroctl.step(STATE, "openai")
        self.assertEqual(action["action"], "degrade")
        seen = FakeProvider.seen[0]
        self.assertEqual(seen["path"], "/v1/chat/completions")
        self.assertEqual(seen["auth"], "Bearer test-key")
        self.assertEqual(seen["body"]["model"], "m-test")
        self.assertEqual([m["role"] for m in seen["body"]["messages"]], ["system", "user"])

    def test_gemini_shape(self):
        os.environ.update(GEMINI_API_KEY="g-key", GEMINI_BASE_URL=self.base + "/v1beta")
        os.environ.pop("GEMINI_MODEL", None)
        action = zeroctl.step(STATE, "gemini")
        self.assertEqual(action["component"], "extra")
        seen = FakeProvider.seen[0]
        self.assertEqual(seen["path"], "/v1beta/models/gemini-2.5-flash:generateContent")
        self.assertEqual(seen["goog"], "g-key")
        self.assertNotIn("g-key", seen["path"])

    def test_missing_key_is_none(self):
        os.environ.pop("OPENAI_API_KEY", None)
        action = zeroctl.step(STATE, "openai")
        self.assertEqual(action, {"action": "none", "error": "OPENAI_API_KEY is not set"})


class Cli(unittest.TestCase):
    def test_stdin_stdout(self):
        with tempfile.TemporaryDirectory() as logs:
            out = subprocess.run([sys.executable, str(HERE / "zeroctl.py"), "--llm", "mock", "--log-dir", logs],
                                 input=json.dumps(STATE), capture_output=True, text=True, check=True)
            self.assertEqual(json.loads(out.stdout.strip().splitlines()[-1])["action"], "move")
            names = sorted(p.name.split("-", 1)[1] for p in Path(logs).iterdir())
            self.assertEqual(names, ["action.json", "prompt.txt", "response.txt"])

    def test_bad_state(self):
        out = subprocess.run([sys.executable, str(HERE / "zeroctl.py"), "--llm", "mock"], input="not json",
                             capture_output=True, text=True, check=True)
        self.assertEqual(json.loads(out.stdout)["action"], "none")


if __name__ == "__main__":
    unittest.main(verbosity=1)
