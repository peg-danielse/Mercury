"""The process-control call `decide` hands to llmctl.execute."""

from __future__ import annotations

import os
import sys
import unittest

import torch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))

from control import decide, legal_delta  # noqa: E402
from replay import Replay  # noqa: E402
from teacher import CPU_SCALE, encode_call  # noqa: E402


def daemon(ident, cpu_use, ncpus, l3):
    return {
        "daemon_id": ident,
        "cpu_use": cpu_use,
        "occupancy": 0.5,
        "pressure": 0.1,
        "cpus": list(range(ncpus)),
        "l3": list(l3),
    }


def state(rows):
    return {
        "pool": {"cores": 8, "max": 40, "busy": 0.2, "pressure": 0.0, "allocated": 8},
        "latency": {"now": {"p95_ms": 20.0, "error_rate": 0.0, "req_per_s": 10.0}},
        "daemons": rows,
    }


class Fixed(torch.nn.Module):
    """Returns one stored vector, in actor layout [B, N, 1]."""

    def __init__(self, values):
        super().__init__()
        self.values = torch.tensor(values, dtype=torch.float32).view(-1, 1)
        self.feat_dim = 10
        self.action_dim = 1

    def forward(self, obs, history, topology, mask):
        return self.values.view(1, -1, 1).expand(obs.shape[0], -1, 1) * mask.unsqueeze(-1).to(self.values.dtype)


class ControlTest(unittest.TestCase):
    def test_a_scale_step_is_the_call_execute_posts(self):
        from llmctl import scale_cpus

        rows = [daemon("b-search", 0.2, 4, ["d0"]), daemon("a-front", 0.9, 2, ["d0"])]
        # observe sorts by daemon_id, so the vector is [a-front, b-search].
        decision, carry = decide(Fixed([0.5, 0.1]), state(rows))
        self.assertEqual(decision.ids, ["a-front", "b-search"])
        self.assertEqual(decision.name, "scale_daemon")
        self.assertEqual(decision.daemon_id, "a-front")
        self.assertEqual(decision.cpus, 2)
        self.assertEqual(decision.arguments["daemon_id"], "a-front")
        self.assertEqual(decision.arguments["cpus"], 2)
        self.assertEqual(scale_cpus(decision.arguments["cpus"]), 2)
        mapped = encode_call(decision.ids, {"name": "scale_daemon", "input": decision.arguments, "ok": True})
        self.assertTrue(torch.equal(decision.executed, mapped["continuous"]))
        self.assertEqual(carry[0], decision.ids)

    def test_a_small_vector_holds(self):
        decision, _ = decide(Fixed([0.05, -0.05]), state([
            daemon("a", 0.2, 2, ["d0"]), daemon("b", 0.2, 2, ["d1"]),
        ]))
        self.assertEqual(decision.name, "hold")
        self.assertIsNone(decision.daemon_id)
        self.assertEqual(decision.cpus, 0)
        self.assertNotIn("daemon_id", decision.arguments)
        self.assertTrue(torch.equal(decision.executed, torch.zeros(2, 1)))

    def test_remove_leaves_one_cpu_and_the_last_cpu_holds(self):
        self.assertEqual(legal_delta(daemon("a", 0.1, 1, ["d0"]), -4), 0)
        self.assertEqual(legal_delta(daemon("a", 0.1, 4, ["d0"]), -4), -3)
        alone = state([daemon("solo", 0.9, 1, ["d0"])])
        held, _ = decide(Fixed([-1.0]), alone)
        self.assertEqual(held.name, "hold")
        self.assertTrue(torch.equal(held.executed, torch.zeros(1, 1)))
        self.assertAlmostEqual(held.proposal[0, 0].item(), -1.0)
        room = state([daemon("solo", 0.9, 4, ["d0"])])
        stepped, _ = decide(Fixed([-1.0]), room)
        self.assertEqual((stepped.name, stepped.cpus), ("scale_daemon", -3))
        self.assertAlmostEqual(stepped.executed[0, 0].item(), -3 / CPU_SCALE)

    def test_loadgen_and_jaeger_are_not_controllable_processes(self):
        rows = [
            daemon("hotel-loadgen", 9.0, 8, ["d1"]),
            daemon("jaeger", 1.0, 2, ["d1"]),
            daemon("svc", 0.4, 2, ["d0"]),
        ]
        decision, _ = decide(Fixed([0.5]), state(rows))
        self.assertEqual(decision.ids, ["svc"])
        self.assertEqual(decision.daemon_id, "svc")
        self.assertFalse(decision.daemon_id.endswith("loadgen"))
        self.assertFalse(decision.daemon_id.endswith("jaeger"))

    def test_the_larger_request_wins_and_replay_accepts_the_row(self):
        rows = [daemon("a", 0.2, 4, ["d0"]), daemon("b", 0.8, 4, ["d0"])]
        decision, carry = decide(Fixed([-0.25, 0.5]), state(rows))
        self.assertEqual((decision.daemon_id, decision.cpus), ("b", 2))
        again, _ = decide(Fixed([0.0, 0.0]), state(rows), previous=carry)
        self.assertTrue(torch.equal(again.history[0, again.ids.index("a")], decision.obs[decision.ids.index("a")]))
        replay = Replay(4)
        replay.add(
            obs=decision.obs, history=decision.history, topology=decision.topology, node_mask=decision.mask,
            action=decision.proposal, executed=decision.executed, reward=0.0, done=False,
            next_obs=again.obs, next_history=again.history, next_topology=again.topology, next_mask=again.mask,
            global_state=decision.global_state, next_global=again.global_state,
        )
        self.assertEqual(len(replay), 1)
        self.assertFalse(torch.equal(decision.proposal, decision.executed))

    def test_an_empty_machine_holds_and_training_mode_is_restored(self):
        actor = Fixed([0.0])
        actor.train()
        decision, _ = decide(actor, state([]))
        self.assertEqual(decision.name, "hold")
        self.assertEqual(decision.ids, [])
        self.assertTrue(actor.training)
        self.assertNotIn("execute", decide.__code__.co_names)


if __name__ == "__main__":
    unittest.main()
