"""The seven evaluation rows are one setting, and the checkpoint is that setting."""

from __future__ import annotations

import argparse
import json
import os
import sys
import tempfile
import unittest

import torch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from actors import ActorConfig, build_actor  # noqa: E402
from control import decide  # noqa: E402
from replay import Replay  # noqa: E402
from settings import SETTINGS, get, load_controller, save_controller  # noqa: E402
from student import TrainConfig  # noqa: E402
from teacher import FEAT_DIM, GLOBAL_DIM  # noqa: E402
from test_bc import rule_demos  # noqa: E402
from train import fit  # noqa: E402


ROWS = {
    "llm": (None, None, None),
    "gnn-imitation": ("gnn", "bc", "llm"),
    "tf-imitation": ("transformer", "bc", "llm"),
    "gnn-rl": ("gnn", "rl", "none"),
    "tf-rl": ("transformer", "rl", "none"),
    "gnn-imitation-rl": ("gnn", "bc_then_rl", "llm"),
    "tf-imitation-rl": ("transformer", "bc_then_rl", "llm"),
    "mlp-action": ("mlp", "bc", "llm"),
    "gnn-action": ("gnn", "bc", "llm"),
    "tf-action": ("transformer", "bc", "llm"),
}


def transition(nodes: int = 2) -> dict:
    obs = torch.zeros(nodes, FEAT_DIM)
    obs[:, 0] = 0.4
    history = torch.zeros(1, nodes, FEAT_DIM)
    topology = torch.zeros(nodes, nodes)
    mask = torch.ones(nodes, dtype=torch.bool)
    action = torch.zeros(nodes, 1)
    globe = torch.zeros(GLOBAL_DIM)
    return {
        "obs": obs, "history": history, "topology": topology, "node_mask": mask,
        "action": action, "executed": action.clone(), "reward": 0.0, "done": False,
        "next_obs": obs.clone(), "next_history": history.clone(),
        "next_topology": topology.clone(), "next_mask": mask.clone(),
        "global_state": globe, "next_global": globe.clone(),
    }


def machine():
    return {
        "pool": {"cores": 8, "max": 40, "busy": 0.2, "pressure": 0.0, "allocated": 4},
        "latency": {"now": {"p95_ms": 20.0, "error_rate": 0.0, "req_per_s": 10.0}},
        "daemons": [{
            "daemon_id": "hotel-frontend",
            "cpu_use": 0.8, "occupancy": 0.9, "pressure": 0.2,
            "cpus": [1, 2], "l3": ["l3-0"],
        }],
    }


class TableTest(unittest.TestCase):
    def test_the_seven_rows_are_the_setting_table(self):
        self.assertEqual([item.name for item in SETTINGS], list(ROWS))
        for item in SETTINGS:
            self.assertEqual(
                (item.actor_type, item.training_mode, item.teacher_type),
                ROWS[item.name],
            )
            self.assertTrue(item.establishes)

    def test_imitation_plus_rl_keeps_the_teacher_term(self):
        for name in ("gnn-imitation-rl", "tf-imitation-rl"):
            config = get(name).train_config()
            self.assertEqual(config.imitation_weight, 1.0)
            self.assertEqual(config.teacher_type, "llm")
        self.assertEqual(get("gnn-imitation").train_config().imitation_weight, 0.0)
        self.assertEqual(get("gnn-rl").train_config().teacher_type, "none")

    def test_the_teacher_has_no_actor_and_unknown_names_fail(self):
        with self.assertRaises(ValueError):
            get("llm").train_config()
        with self.assertRaises(ValueError):
            get("gnn-action").train_config()
        with self.assertRaises(ValueError):
            get("gru")


class CheckpointTest(unittest.TestCase):
    def test_the_file_rebuilds_the_widths_it_was_trained_with(self):
        setting = get("gnn-imitation")
        config = setting.train_config()
        config.hidden = 8
        actor = build_actor(config.actor_config())
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "actor.pt")
            save_controller(actor, path, setting, config)
            loaded, spec = load_controller(path)
        self.assertEqual(spec.name, "gnn-imitation")
        self.assertEqual(loaded.backbone.g1.W.shape[1], FEAT_DIM)
        self.assertEqual(loaded.backbone.g1.W.shape[2], 8)
        default = build_actor(ActorConfig("gnn", FEAT_DIM, 1, hidden=32, taps=config.taps))
        with self.assertRaises(RuntimeError):
            default.load_state_dict(loaded.state_dict())

    def test_a_loaded_actor_is_the_call_execute_posts(self):
        setting = get("tf-imitation")
        config = TrainConfig("transformer", "bc", "llm", hidden=8, layers=1, heads=2)
        actor = build_actor(config.actor_config())
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "actor.pt")
            save_controller(actor, path, setting, config)
            loaded, _ = load_controller(path)
        decision, carry = decide(loaded, machine(), None)
        self.assertIn(decision.name, ("hold", "scale_daemon"))
        self.assertEqual(decision.arguments["reason"], "ctde")
        if decision.name == "scale_daemon":
            self.assertEqual(decision.daemon_id, "hotel-frontend")
            self.assertIsInstance(decision.cpus, int)
            self.assertNotEqual(decision.cpus, 0)
        again, _ = decide(loaded, machine(), carry)
        self.assertIn(again.name, ("hold", "scale_daemon"))


class FitTest(unittest.TestCase):
    def test_imitation_writes_its_setting(self):
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "actor.pt")
            trainer = fit("gnn-imitation", rule_demos(4, 0), steps=2, out=path, seed=0, batch_size=4)
            _, spec = load_controller(path)
        self.assertEqual(spec.name, "gnn-imitation")
        self.assertEqual(trainer.config.training_mode, "bc")

    def test_rl_from_scratch_refuses_to_run_without_a_replay(self):
        with self.assertRaises(ValueError):
            fit("gnn-rl", steps=2, rl_steps=2)

    def test_imitation_plus_rl_runs_both_procedures(self):
        replay = Replay(4)
        for _ in range(4):
            replay.add(**transition())
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "actor.pt")
            trainer = fit(
                "tf-imitation-rl", rule_demos(4, 1), replay,
                steps=1, rl_steps=1, out=path, seed=1, batch_size=4,
            )
            _, spec = load_controller(path)
        self.assertEqual(spec.name, "tf-imitation-rl")
        self.assertEqual(trainer.config.training_mode, "bc_then_rl")
        self.assertEqual(trainer.weight, 1.0)


class LiveTest(unittest.TestCase):
    def test_rl_episode_posts_the_decision_and_keeps_the_carry(self):
        tools = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
        sys.path.insert(0, tools)
        import llmctl

        llmctl.RL.clear()
        setting = get("gnn-imitation")
        config = TrainConfig("gnn", "bc", "llm", hidden=8)
        actor = build_actor(config.actor_config())
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "actor.pt")
            save_controller(actor, path, setting, config)
            args = argparse.Namespace(model=path, current=[], max_actions=6)
            mercury = llmctl.Mercury("http://127.0.0.1:9", 50, True)
            log = llmctl.Log(os.path.join(directory, "llmctl.jsonl"))
            try:
                first = llmctl.rl_episode(args, mercury, log, "rl period", machine())
                carry = llmctl.RL["previous"]
                second = llmctl.rl_episode(args, mercury, log, "rl period", machine())
            finally:
                log.file.close()
            with open(os.path.join(directory, "llmctl.jsonl")) as handle:
                records = [json.loads(line) for line in handle]
        turns = [row for row in records if row["event"] == "turn"]
        posted = [row for row in records if row["event"] == "tool"]
        self.assertEqual(len(turns), 2)
        self.assertEqual(turns[0]["setting"], "gnn-imitation")
        self.assertEqual(posted[0]["name"], turns[0]["tool"])
        self.assertTrue(posted[0]["ok"])
        self.assertIn("rl gnn-imitation:", first)
        self.assertIn("rl gnn-imitation:", second)
        self.assertEqual(carry[0], ["hotel-frontend"])
        self.assertFalse(llmctl.RL["actor"].training)
        llmctl.RL.clear()


if __name__ == "__main__":
    unittest.main()
