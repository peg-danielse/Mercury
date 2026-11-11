"""Teacher demonstrations from an llmctl.jsonl fixture.

The fixture matches the episode / state / tool records `llmctl.py` already
writes. Nothing here reads a live campaign directory.
"""

from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest

import torch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from teacher import (  # noqa: E402
    CPU_SCALE,
    FEAT_DIM,
    decode_action,
    demonstration_from_episode,
    demonstrations_from_run,
    discrete_index,
    encode_call,
    local_features,
    observe,
    parse_llmctl_jsonl,
    workload_of,
)


def daemon(ident, cpu_use, l3, **extra):
    row = {
        "daemon_id": ident,
        "cpu_use": cpu_use,
        "occupancy": 0.4,
        "pressure": 0.05,
        "cpus": [1],
        "replica": False,
        "throttled": False,
        "cpu_wait": 0.0,
        "ipc": 1.2,
        "llc_per_ms": 4.0,
        "l3": list(l3),
    }
    row.update(extra)
    return row


def state(daemons):
    return {
        "pool": {"cores": 8, "max": 40, "busy": 0.3, "pressure": 0.02, "allocated": 12},
        "latency": {"now": {"p95_ms": 25.0, "error_rate": 0.0, "req_per_s": 80.0}},
        "daemons": daemons,
    }


def fixture_records():
    first = state([
        daemon("b-search", 0.2, ["d0"]),
        daemon("a-front", 0.8, ["d0"]),
        daemon("hotel-loadgen", 9.0, ["d1"]),
        daemon("jaeger", 1.0, ["d1"]),
    ])
    second = state([
        daemon("b-search", 0.2, ["d0"]),
        daemon("c-new", 0.3, ["d2"]),
        daemon("a-front", 0.5, ["d0"]),
    ])
    third = state([daemon("a-front", 0.4, ["d0"])])
    fourth = state([daemon("a-front", 0.9, ["d0"])])
    return [
        {"event": "tool", "t": 1.0, "name": "hold", "input": {}, "ok": True},
        {"event": "episode", "t": 1000.0, "trigger": "short"},
        {"event": "state", "t": 1000.1, "state": first},
        {"event": "tool", "t": 1000.2, "name": "inspect", "input": {"wait_seconds": 1}, "ok": True},
        {"event": "tool", "t": 1001.5, "name": "scale_daemon",
         "input": {"daemon_id": "a-front", "cpus": 2, "reason": "short"}, "ok": True},
        {"event": "episode", "t": 1010.0, "trigger": "idle"},
        {"event": "state", "t": 1010.1, "state": second},
        {"event": "tool", "t": 1011.0, "name": "hold", "input": {"reason": "fine"}, "ok": True},
        {"event": "episode", "t": 1020.0, "trigger": "pool"},
        {"event": "state", "t": 1020.1, "state": third},
        {"event": "tool", "t": 1021.0, "name": "grow_pool", "input": {"l3": "d1", "reason": "pressure"}, "ok": True},
        {"event": "episode", "t": 1030.0, "trigger": "short"},
        {"event": "state", "t": 1030.1, "state": fourth},
        {"event": "tool", "t": 1031.0, "name": "scale_daemon",
         "input": {"daemon_id": "a-front", "cpus": -4, "reason": "back"}, "ok": False},
        {"event": "tool", "t": 1032.0, "name": "hold", "input": {"reason": "gave up"}, "ok": True},
    ]


class MapTest(unittest.TestCase):
    def test_scale_and_hold_roundtrip_inside_the_student_range(self):
        for cpus in (-4, -2, -1, 1, 2, 4):
            mapped = encode_call(
                ["svc"],
                {"name": "scale_daemon", "input": {"daemon_id": "svc", "cpus": cpus}, "ok": True},
            )
            self.assertTrue(mapped["student_valid"], cpus)
            self.assertFalse(mapped["clipped"], cpus)
            self.assertAlmostEqual(mapped["continuous"][0, 0].item(), cpus / CPU_SCALE)
            self.assertEqual(decode_action(mapped["continuous"], ["svc"]), ("scale_daemon", "svc", cpus))
        zeros = torch.zeros(2, 1)
        self.assertEqual(decode_action(zeros, ["a", "b"]), ("hold", None, 0))

    def test_a_step_past_the_scale_clamps_and_decodes_to_the_limit(self):
        mapped = encode_call(["a"], {"name": "scale_daemon", "input": {"daemon_id": "a", "cpus": 8}, "ok": True})
        self.assertTrue(mapped["student_valid"])
        self.assertTrue(mapped["clipped"])
        self.assertEqual(mapped["continuous"][0, 0].item(), 1.0)
        self.assertEqual(decode_action(mapped["continuous"], ["a"]), ("scale_daemon", "a", 4))

    def test_pool_and_unknown_targets_stay_out_of_the_student_vector(self):
        pool = encode_call(["a"], {"name": "grow_pool", "input": {"l3": "d1"}, "ok": True})
        self.assertFalse(pool["student_valid"])
        self.assertEqual(pool["discrete"], discrete_index("grow_pool"))
        self.assertTrue(torch.equal(pool["continuous"], torch.zeros(1, 1)))
        missing = encode_call(["a"], {"name": "scale_daemon", "input": {"daemon_id": "nope", "cpus": 1}, "ok": True})
        self.assertFalse(missing["student_valid"])
        self.assertTrue(torch.equal(missing["continuous"], torch.zeros(1, 1)))

    def test_local_rows_ignore_the_other_process(self):
        one = daemon("a", 0.2, ["d0"])
        other = daemon("b", 0.8, ["d0"], pressure=0.9, ipc=3.0)
        ids, obs, history, topology, mask, global_state = observe(state([one, other]))
        self.assertEqual(ids, ["a", "b"])
        self.assertTrue(torch.equal(obs[0], torch.tensor(local_features(one))))
        changed = daemon("b", 0.1, ["d0"], pressure=0.0)
        ids2, obs2, _, topology2, _, _ = observe(state([one, changed]))
        self.assertEqual(ids2, ["a", "b"])
        self.assertTrue(torch.equal(obs[0], obs2[0]))
        self.assertFalse(torch.equal(obs[1], obs2[1]))
        self.assertTrue(torch.equal(topology[0], topology2[0]))
        self.assertEqual(obs.shape[-1], FEAT_DIM)
        self.assertEqual(global_state.shape, (8,))
        self.assertEqual(float(topology.diag().sum()), 0.0)
        self.assertGreater(topology[0, 1].item(), 0.0)
        alone = observe(state([daemon("a", 0.2, ["d0"]), daemon("b", 0.2, ["d9"])]))[3]
        self.assertEqual(float(alone.sum()), 0.0)
        self.assertTrue(torch.equal(history, torch.zeros_like(history)))
        self.assertEqual(mask.dtype, torch.bool)

    def test_history_follows_daemon_id_and_a_new_process_starts_at_zero(self):
        first_ids, first_obs, _, _, _, _ = observe(state([
            daemon("b", 0.2, ["d0"]),
            daemon("a", 0.7, ["d0"]),
        ]))
        ids, obs, history, _, _, _ = observe(
            state([daemon("a", 0.1, ["d0"]), daemon("c", 0.3, ["d1"]), daemon("b", 0.2, ["d0"])]),
            previous=(first_ids, first_obs),
        )
        self.assertEqual(ids, ["a", "b", "c"])
        locate = {ident: index for index, ident in enumerate(first_ids)}
        self.assertTrue(torch.equal(history[0, ids.index("a")], first_obs[locate["a"]]))
        self.assertTrue(torch.equal(history[0, ids.index("b")], first_obs[locate["b"]]))
        self.assertTrue(torch.equal(history[0, ids.index("c")], torch.zeros(FEAT_DIM)))
        self.assertFalse(torch.equal(history[0, ids.index("a")], obs[ids.index("a")]))

    def test_loadgen_and_jaeger_are_not_student_processes(self):
        ids, obs, _, _, _, _ = observe(state([
            daemon("jaeger", 1.0, ["d0"]),
            daemon("svc", 0.3, ["d0"]),
            daemon("hotel-loadgen", 4.0, ["d1"]),
        ]))
        self.assertEqual(ids, ["svc"])
        self.assertEqual(obs.shape[0], 1)

    def test_workload_id_comes_from_the_run_path(self):
        self.assertEqual(workload_of("/data/dsb/compare/hotel/r1"), "hotel")
        self.assertEqual(workload_of("/data/social-eval/r1"), "social")
        self.assertEqual(workload_of("/data/media/wave"), "media")
        self.assertEqual(workload_of("/tmp/plain"), "other")

    def test_a_refused_scale_keeps_the_proposal_and_applies_nothing(self):
        episode = {
            "run": "/data/media/r1",
            "t_s": 4.0,
            "trigger": "short",
            "state": state([daemon("svc", 0.9, ["d0"])]),
            "calls": [{"name": "scale_daemon", "input": {"daemon_id": "svc", "cpus": -2}, "ok": False, "t": 9.0}],
        }
        demo, _ = demonstration_from_episode(episode)
        self.assertEqual(demo.workload, "media")
        self.assertTrue(demo.student_valid)
        self.assertFalse(demo.ok)
        self.assertAlmostEqual(demo.continuous[0, 0].item(), -2 / CPU_SCALE)
        self.assertTrue(torch.equal(demo.applied, torch.zeros(1, 1)))
        self.assertIsNone(demo.reward)
        self.assertEqual(demo.tool_t, 9.0)

    def test_an_explicit_reward_is_kept(self):
        episode = {
            "run": "/data/social/r",
            "t_s": 1.0,
            "state": state([daemon("svc", 0.2, ["d0"])]),
            "calls": [{"name": "hold", "input": {}, "ok": True}],
            "reward": 0.25,
        }
        demo, _ = demonstration_from_episode(episode)
        self.assertEqual(demo.reward, 0.25)
        self.assertEqual(demo.tool_name, "hold")
        self.assertTrue(torch.equal(demo.applied, torch.zeros(1, 1)))


class LogTest(unittest.TestCase):
    def test_jsonl_fixture_matches_from_log_and_keeps_the_tool_time(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = os.path.join(tmp, "hotel", "r1")
            os.makedirs(run)
            path = os.path.join(run, "llmctl.jsonl")
            with open(path, "w") as handle:
                for record in fixture_records():
                    handle.write(json.dumps(record) + "\n")
            parsed = parse_llmctl_jsonl(path)
            via_log = demonstrations_from_run(run)
        self.assertEqual([demo.tool_name for demo in parsed], ["scale_daemon", "hold", "grow_pool", "scale_daemon"])
        self.assertEqual([demo.tool_name for demo in via_log], [demo.tool_name for demo in parsed])
        self.assertEqual([demo.cpus for demo in via_log], [demo.cpus for demo in parsed])
        self.assertEqual([demo.ids for demo in via_log], [demo.ids for demo in parsed])
        self.assertEqual([demo.student_valid for demo in parsed], [True, True, False, True])
        self.assertEqual(parsed[0].workload, "hotel")
        self.assertEqual(parsed[0].tool_t, 1001.5)
        self.assertTrue(all(demo.tool_t is None for demo in via_log))
        self.assertEqual(parsed[0].ids, ["a-front", "b-search"])
        self.assertAlmostEqual(parsed[0].continuous[parsed[0].ids.index("a-front"), 0].item(), 0.5)
        self.assertEqual(parsed[0].continuous[parsed[0].ids.index("b-search"), 0].item(), 0.0)
        self.assertTrue(torch.equal(parsed[0].history, torch.zeros_like(parsed[0].history)))
        self.assertTrue(torch.equal(parsed[1].history[0, parsed[1].ids.index("a-front")], parsed[0].obs[0]))
        self.assertTrue(torch.equal(parsed[1].history[0, parsed[1].ids.index("c-new")], torch.zeros(FEAT_DIM)))
        self.assertFalse(parsed[2].student_valid)
        self.assertIsNone(parsed[2].applied)
        self.assertTrue(parsed[3].student_valid)
        self.assertFalse(parsed[3].ok)
        self.assertTrue(torch.equal(parsed[3].applied, torch.zeros_like(parsed[3].continuous)))
        self.assertEqual(parsed[3].tool_name, "scale_daemon")
        self.assertEqual(len(parsed[3].tools), 2)
        self.assertIsNone(parsed[0].reward)
        self.assertTrue(torch.equal(parsed[1].history, via_log[1].history))


if __name__ == "__main__":
    unittest.main()
