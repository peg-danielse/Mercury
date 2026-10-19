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

import rca  # noqa: E402
import zeroctl  # noqa: E402

# No test may reach a real Jaeger on this host; the Jaeger tests opt back in.
os.environ["ZEROCTL_JAEGER"] = "off"

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
        cls.server.server_close()

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


GNN = {"enabled": True, "available": True, "model": "/srv/Mercury/models/gnn_v5.json", "error": "", "tick_ms": 10,
       "window_s": 5, "daemons": [
           {"daemon_id": "svc0", "cpus": 1, "ticks": 500, "required_mean": 2.6, "required_max": 4, "required_p90": 3,
            "under_share": 0.92, "pred_demand_mean": 2.31, "threads_mean": 9},
           {"daemon_id": "svc1", "cpus": 1, "ticks": 500, "required_mean": 1.0, "required_max": 1, "required_p90": 1,
            "under_share": 0.0, "pred_demand_mean": 0.4, "threads_mean": 5}]}


def with_rca(gnn=None, jaeger="off"):
    state = json.loads(json.dumps(STATE))
    state["rca"] = {"gnn": gnn if gnn is not None else {"enabled": False, "available": False, "model": "", "error": "",
                                                        "daemons": []},
                    "jaeger": jaeger, "application": ""}
    return state


def span(span_id, service, op, duration, parent=None):
    refs = [{"refType": "CHILD_OF", "spanID": parent}] if parent else []
    return {"spanID": span_id, "operationName": op, "duration": duration, "processID": service, "references": refs}


def jaeger_reply(slow=6, fast=30):
    """Hotel-like traces: frontend root; slow ones spend their time in srv-reservation."""
    data = []
    for i in range(fast):
        data.append({"traceID": f"f{i}", "processes": {"p1": {"serviceName": "frontend"}, "p2": {"serviceName": "srv-search"}},
                     "spans": [span("a", "p1", "HTTP GET /hotels", 10_000 + 100 * (i % 5)),
                               span("b", "p2", "/search.Search/Nearby", 6_000, "a")]})
    for i in range(slow):
        data.append({"traceID": f"s{i}", "processes": {"p1": {"serviceName": "frontend"}, "p3": {"serviceName": "srv-reservation"}},
                     "spans": [span("a", "p1", "HTTP POST /reservation", 90_000),
                               span("c", "p3", "/reservation.Reservation/MakeReservation", 80_000, "a")]})
    return {"data": data}


class FakeJaeger(BaseHTTPRequestHandler):
    def do_GET(self):  # noqa: N802
        if self.path.startswith("/api/services"):
            reply = {"data": ["frontend", "srv-reservation", "jaeger-query"]}
        elif self.path.startswith("/api/traces"):
            reply = jaeger_reply()
        else:
            self.send_response(404)
            self.end_headers()
            return
        data = json.dumps(reply).encode()
        self.send_response(200)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *args):
        pass


class Rca(unittest.TestCase):
    def tearDown(self):
        os.environ["ZEROCTL_JAEGER"] = "off"
        os.environ.pop("ZEROCTL_JAEGER_TRACES", None)
        os.environ.pop("JAEGER_URL", None)

    def test_with_gnn_localizes_starved_daemon(self):
        result = rca.run_rca(with_rca(GNN))
        self.assertEqual(result["localized"], ["svc0"])
        self.assertEqual(result["method"], "GNN per-thread demand (gnn_v5.json)")
        text = rca.format_rca(result)
        self.assertIn("svc0  holds 1 cpus, needs 2.6 (p90 3, max 4), under-provisioned 92% of 500 ticks", text)
        self.assertIn("STARVED", text)
        self.assertIn("traces: Jaeger off", text)

    def test_without_gnn_is_labelled_heuristic(self):
        result = rca.run_rca(with_rca())
        self.assertEqual(result["method"], "heuristic ring-load ranking (no GNN)")
        self.assertIn("GNN: off (zeroctl.gnn false)", rca.format_rca(result))

    def test_gnn_unavailable_reports_error(self):
        gnn = {"enabled": True, "available": False, "model": "/x/models/gnn_v5.json", "error": "cannot read /x/models/gnn_v5.json"}
        text = rca.format_rca(rca.run_rca(with_rca(gnn)))
        self.assertIn("GNN: unavailable: cannot read /x/models/gnn_v5.json", text)
        self.assertIn("heuristic ring-load ranking", text)

    def test_old_state_without_rca(self):
        self.assertIn("## Root cause analysis", zeroctl.build_prompt(STATE))

    def test_mock_scales_starved_daemon(self):
        state = with_rca(GNN)
        doc = zeroctl.to_action(zeroctl.parse_docs(zeroctl.mock_reply(state, rca.run_rca(state))))
        self.assertEqual((doc["action"], doc["workload"], doc["cpus"]), ("scale_out", "svc0", 3))
        self.assertEqual(zeroctl.step(state, "mock")["action"], "scale_out")

    def test_jaeger_auto_skips_without_jaeger(self):
        os.environ.pop("ZEROCTL_JAEGER", None)
        os.environ["JAEGER_URL"] = "http://127.0.0.1:9"  # discard port: refused
        text = rca.format_rca(rca.run_rca(with_rca(GNN, "auto")))
        self.assertIn("traces: skipped: no Jaeger, not a traced (DSB) workload", text)

    def test_jaeger_explicit_url_unreachable_is_error(self):
        os.environ.pop("ZEROCTL_JAEGER", None)
        result = rca.run_rca(with_rca(None, "http://127.0.0.1:9"))
        self.assertTrue(result["jaeger"]["status"].startswith("error: "))

    def test_jaeger_replay_file(self):
        os.environ.pop("ZEROCTL_JAEGER", None)
        with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as handle:
            json.dump(jaeger_reply(), handle)
        os.environ["ZEROCTL_JAEGER_TRACES"] = handle.name
        try:
            result = rca.run_rca(with_rca(GNN, "auto"))
        finally:
            os.unlink(handle.name)
        self.assertEqual((result["jaeger"]["traces"], result["jaeger"]["anomalous"]), (36, 6))
        self.assertEqual(result["jaeger"]["ranking"], [("srv-reservation", 6)])

    def test_jaeger_http_api_with_reference(self):
        os.environ.pop("ZEROCTL_JAEGER", None)
        server = HTTPServer(("127.0.0.1", 0), FakeJaeger)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        try:
            state = with_rca(None, f"http://127.0.0.1:{server.server_port}")
            result = rca.run_rca(state, {"reference": {"p95_ms": 20.0}})
        finally:
            server.shutdown()
            server.server_close()
        jaeger = result["jaeger"]
        self.assertEqual(jaeger["status"], "ok")
        self.assertEqual(jaeger["threshold_ms"], 20.0)
        self.assertEqual(jaeger["ranking"], [("srv-reservation", 6)])
        text = rca.format_rca(result)
        self.assertIn("where the degradation shows, most affected first: srv-reservation (100%)", text)
        self.assertIn("not infrabench's Isolation Forest", text)


class Cli(unittest.TestCase):
    def test_stdin_stdout(self):
        with tempfile.TemporaryDirectory() as logs:
            out = subprocess.run([sys.executable, str(HERE / "zeroctl.py"), "--llm", "mock", "--log-dir", logs],
                                 input=json.dumps(STATE), capture_output=True, text=True, check=True)
            self.assertEqual(json.loads(out.stdout.strip().splitlines()[-1])["action"], "move")
            names = sorted(p.name.split("-", 1)[1] for p in Path(logs).iterdir())
            self.assertEqual(names, ["action.json", "prompt.txt", "rca.json", "response.txt"])

    def test_bad_state(self):
        out = subprocess.run([sys.executable, str(HERE / "zeroctl.py"), "--llm", "mock"], input="not json",
                             capture_output=True, text=True, check=True)
        self.assertEqual(json.loads(out.stdout)["action"], "none")


if __name__ == "__main__":
    unittest.main(verbosity=1)
