"""The Mercury cycle scores each decision by what followed it and trains on that."""

from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from actions import TOOLS, build_action_model  # noqa: E402
from cycle import (  # noqa: E402
    advantage, better, cycle_loop, decision_rows, meets, mercury_env, mix_probs, phase_at, phase_baseline, score_run,
)
from train import examples_from  # noqa: E402


def state():
    return {
        "pool": {"cores": 4, "max": 40, "start": 4, "busy": 0.1, "pressure": 0.0, "allocated": 2,
                 "free_domains": ["l3.1"]},
        "latency": {"now": {"p95_ms": 10.0, "error_rate": 0.0, "req_per_s": 1.0}},
        "l3": [
            {"name": "l3.0", "in_pool": True, "load": 0.2, "cpus": [0, 1, 2, 3], "free": [], "daemons": ["hotel-frontend"]},
            {"name": "l3.1", "in_pool": False, "load": 0.0, "cpus": [4, 5, 6, 7], "free": [4, 5, 6, 7], "daemons": []},
        ],
        "daemons": [
            {"daemon_id": "hotel-frontend", "cpu_use": 0.2, "occupancy": 0.2, "pressure": 0.0,
             "cpus": [0], "l3": ["l3.0"]},
            {"daemon_id": "hotel-geo", "cpu_use": 0.2, "occupancy": 0.2, "pressure": 0.0,
             "cpus": [1], "l3": ["l3.0"]},
        ],
    }


def episode():
    return {"run": "teacher", "state": state(), "calls": []}


def check(stamp, error):
    return {"event": "check", "t": stamp, "outcome": {"p95_ms": 10.0, "error_rate": error, "cores": 4, "max": 40,
                                                      "busy": 0.2}}


class ScoreTest(unittest.TestCase):
    def test_p95_and_error_come_from_the_critical_routes_and_split_by_level(self):
        with tempfile.TemporaryDirectory() as directory:
            with open(os.path.join(directory, "loadgen.hist.jsonl"), "w") as handle:
                for t0, rate, errors in ((0, 1500, 0), (10**9, 3100, 5)):
                    span = 500_000_000
                    handle.write(json.dumps({
                        "route": "hotels", "t0_ns": t0, "t1_ns": t0 + span, "sent": rate // 2, "errors": errors,
                        "hist": [[1_000_000, rate // 2]],
                    }) + "\n")
                handle.write(json.dumps({
                    "route": "review", "t0_ns": 0, "t1_ns": 500_000_000, "sent": 100, "errors": 50,
                    "hist": [[9_000_000, 100]],
                }) + "\n")
            score = score_run(directory, ["hotels"], [1500, 3100, 5300], "hotels")
        self.assertEqual(score["sent"], 750 + 1550)
        self.assertEqual(score["errors"], 5)
        self.assertEqual(set(score["by_level"]), {"1500", "3100"})
        self.assertEqual(score["by_level"]["1500"]["errors"], 0)
        self.assertEqual(score["by_level"]["3100"]["errors"], 5)

    def test_a_lower_error_is_better_and_the_bar_needs_both(self):
        slow = {"error_pct": 2.0, "p95_ms": 50.0}
        fast_but_failing = {"error_pct": 3.0, "p95_ms": 10.0}
        self.assertTrue(better(slow, fast_but_failing))
        self.assertFalse(meets(slow, 100.0, 1.0))
        self.assertTrue(meets({"error_pct": 0.5, "p95_ms": 40.0}, 100.0, 1.0))

    def test_the_teacher_keeps_half_and_student_tools_are_drawn_evenly(self):
        scale, grow = TOOLS.index("scale_daemon"), TOOLS.index("grow_pool")
        teacher = [(None, {"tool": scale}) for _ in range(3)]
        student = [(None, {"tool": scale}) for _ in range(99)] + [(None, {"tool": grow})]
        rows, probs = mix_probs(teacher, student)
        self.assertEqual(len(rows), 103)
        self.assertAlmostEqual(float(probs[:3].sum()), 0.5, places=6)
        self.assertAlmostEqual(float(probs[-1]), 0.25, places=6)
        self.assertAlmostEqual(float(probs[3:-1].sum()), 0.25, places=6)


class DecisionTest(unittest.TestCase):
    def test_advantage_is_after_minus_before(self):
        series = [(0.0, -1.0), (1.0, -1.0), (3.0, 0.5), (4.0, 0.5)]
        self.assertAlmostEqual(advantage(series, 1.0, horizon=5.0, delay=1.0), 1.5)
        self.assertIsNone(advantage(series, 10.0))

    def test_the_baseline_is_the_load_phase(self):
        phases = [(0.0, 0), (5.0, 0), (20.0, 1), (25.0, 1), (40.0, 1)]
        self.assertEqual(phase_at(phases, 3.0), (0, True))
        self.assertEqual(phase_at(phases, 12.0), (0, False))
        self.assertEqual(phase_at(phases, 22.0), (1, True))
        self.assertEqual(phase_at(phases, -1.0), (None, False))
        ramp = [-1.0] * 5
        calm = [0.5] * 5
        base = phase_baseline(ramp + calm + [9.0], [(1, True)] * 5 + [(0, False)] * 5 + [(2, False)])
        self.assertEqual(base[:5], [-1.0] * 5)
        self.assertEqual(base[5:10], [0.5] * 5)
        self.assertAlmostEqual(base[10], (9.0 - 5 + 2.5) / 11)

    def test_a_better_call_is_cloned_a_worse_one_avoided_and_a_refused_one_avoided(self):
        records = [check(t, 0.4) for t in (0.0, 0.5, 1.0)]
        records += [
            {"event": "episode", "t": 1.0},
            {"event": "state", "t": 1.0, "state": state()},
            {"event": "tool", "name": "grow_pool", "input": {"l3": "l3.1"}, "ok": True, "t": 1.0},
            {"event": "tool", "name": "scale_daemon", "input": {"daemon_id": "hotel-geo", "cpus": 2}, "ok": False,
             "t": 1.0},
        ]
        records += [check(t, 0.0) for t in (2.5, 3.0, 3.5)]
        records += [
            {"event": "episode", "t": 4.0},
            {"event": "state", "t": 4.0, "state": state()},
            {"event": "tool", "name": "hold", "input": {}, "ok": True, "t": 4.0},
        ]
        records += [check(t, 0.3) for t in (5.5, 6.0)]
        rows, stats = decision_rows(records, "run", slo_ms=100.0)
        self.assertEqual(stats["accepted"], 2)
        self.assertEqual(stats["refused"], 1)
        self.assertEqual((stats["imitate"], stats["worse"]), (1, 1))
        cloned = [TOOLS[target["tool"]] for _scene, target in rows if not target.get("avoid")]
        avoided = sorted(TOOLS[target["tool"]] for _scene, target in rows if target.get("avoid"))
        self.assertEqual(cloned, ["grow_pool"])
        self.assertEqual(avoided, ["hold", "scale_daemon"])
        self.assertGreater(stats["by_tool"]["grow_pool"]["z_mean"], 0)

    def test_a_round_runs_at_the_teacher_cadence_on_the_gate(self):
        env = mercury_env("/tmp/round-1.pt", base={"LLM_ARGS": "--every 0.5"})
        self.assertEqual(env["EVALSET_EVERY"], "0.5")
        self.assertEqual(env["EVALSET_WINDOW_MS"], "1000")
        self.assertIn("--gate", env["LLM_ARGS"].split())
        self.assertEqual(env["LLM_MODELS"], "cycle=/tmp/round-1.pt@rl")
        with self.assertRaises(ValueError):
            mercury_env("/tmp/a@b.pt", base={})


class PhaseLogTest(unittest.TestCase):
    def test_llmctl_logs_the_load_phase_with_each_outcome(self):
        sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))
        import llmctl

        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "loadgen.hist.jsonl")
            with open(path, "w") as handle:
                for window, (level, clients) in enumerate([(0, 1500)] * 3 + [(1, 3100)] * 2):
                    t0 = window * 500_000_000
                    handle.write(json.dumps({"route": "hotels", "level": level, "clients": clients, "t0_ns": t0,
                                             "t1_ns": t0 + 500_000_000, "sent": 10, "done": 10, "errors": 0,
                                             "hist": [[1_000_000, 10]]}) + "\n")
            latency = llmctl.Latency(path)
            latency.summary()
            outcome = llmctl.outcome_of({"pool": {"cores": 8}}, latency)
        self.assertEqual((outcome["phase"], outcome["clients"]), (1, 3100))
        self.assertNotIn("phase", llmctl.outcome_of({"pool": {"cores": 8}}))


class LoopTest(unittest.TestCase):
    def test_every_round_trains_and_a_good_one_stops(self):
        scores = [
            {"error_pct": 5.0, "p95_ms": 200.0},
            {"error_pct": 6.0, "p95_ms": 50.0},
            {"error_pct": 0.4, "p95_ms": 40.0},
        ]
        teacher = examples_from([episode()])

        def collect(checkpoint, rnd):
            return list(teacher), scores[rnd - 1], {"accepted": 1, "avoid": 0}

        model = build_action_model("mlp", hidden=8)
        with tempfile.TemporaryDirectory() as directory:
            history = cycle_loop(
                model, "mlp-action", teacher, rounds=5, collect=collect, steps=1,
                max_p95_ms=100.0, max_error_pct=1.0, out=directory,
            )
            with open(os.path.join(directory, "cycle.jsonl")) as handle:
                written = [json.loads(line) for line in handle]
            self.assertTrue(os.path.isfile(os.path.join(directory, "winner.pt")))
            self.assertTrue(os.path.isfile(os.path.join(directory, "best.pt")))
        self.assertEqual([row["best"] for row in history], [True, False, True])
        self.assertEqual([row["stopped"] for row in history], [False, False, True])
        self.assertIn("loss_last", history[0])
        self.assertEqual(len(written), 3)

    def test_without_a_teacher_the_cycle_trains_on_its_own_rows_only(self):
        own = examples_from([episode()])

        def collect(checkpoint, rnd):
            return list(own), {"error_pct": 5.0 - rnd, "p95_ms": 200.0}, {}

        model = build_action_model("mlp", hidden=8)
        history = cycle_loop(model, "mlp-action", [], rounds=2, collect=collect, steps=1,
                             max_p95_ms=100.0, max_error_pct=1.0, teacher_share=0.0)
        self.assertEqual([row["student_rows"] for row in history], [1, 2])
        with self.assertRaises(ValueError):
            cycle_loop(model, "mlp-action", [], rounds=1, collect=collect, steps=1,
                       max_p95_ms=100.0, max_error_pct=1.0)
