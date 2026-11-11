"""Student trainer: behavior cloning, TD3 fine-tuning, and a non-blocking label queue."""

from __future__ import annotations

import math
import os
import sys
import tempfile
import unittest

import torch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from actors import build_actor  # noqa: E402
from critic import TwinCritic  # noqa: E402
from project import constrain_actions  # noqa: E402
from replay import Replay  # noqa: E402
from student import CorrectiveQueue, StudentTrainer, TrainConfig, student_act  # noqa: E402
from teacher import FEAT_DIM, GLOBAL_DIM, Demonstration  # noqa: E402
from test_bc import rule_demo, rule_demos  # noqa: E402


def transition(nodes, feat, global_dim, proposal, executed, reward):
    obs = torch.zeros(nodes, feat)
    history = torch.zeros(1, nodes, feat)
    topology = torch.zeros(nodes, nodes)
    mask = torch.ones(nodes, dtype=torch.bool)
    gstate = torch.zeros(global_dim)
    return dict(
        obs=obs, history=history, topology=topology, node_mask=mask,
        action=proposal, executed=executed, reward=float(reward), done=False,
        next_obs=obs.clone(), next_history=history.clone(), next_topology=topology.clone(),
        next_mask=mask.clone(), global_state=gstate, next_global=gstate.clone(),
    )


class ConfigTest(unittest.TestCase):
    def test_actor_type_selects_the_module_and_rejects_an_unknown_mode(self):
        built = {}
        for kind in ("mlp", "gnn", "transformer"):
            trainer = StudentTrainer(TrainConfig(kind, "rl", "none", hidden=8, taps=2, layers=1, heads=2))
            built[kind] = type(trainer.actor).__name__
            self.assertEqual(trainer.actor.feat_dim, FEAT_DIM)
        self.assertEqual(len(set(built.values())), 3)
        with self.assertRaises(ValueError):
            TrainConfig("tree", "rl", "none")
        with self.assertRaises(ValueError):
            TrainConfig("mlp", "supervised", "llm")
        with self.assertRaises(ValueError):
            TrainConfig("mlp", "rl", "human")

    def test_teacher_none_forces_rl_only(self):
        trainer = StudentTrainer(TrainConfig("mlp", "rl", "none", imitation_weight=1.0, hidden=8))
        self.assertEqual(trainer.weight, 0.0)
        trainer.add_demonstrations(rule_demos(2, seed=0))
        for _ in range(2):
            trainer.online.add(**transition(
                2, FEAT_DIM, GLOBAL_DIM, torch.zeros(2, 1), torch.zeros(2, 1), 0.0,
            ))
        batch = trainer.online.sample(2, generator=torch.Generator().manual_seed(0))

        def boom(*_args, **_kwargs):
            raise AssertionError("imitation ran without a teacher")

        import student as student_mod
        real = student_mod.clone_step
        student_mod.clone_step = boom
        try:
            stats = trainer.update(batch)
        finally:
            student_mod.clone_step = real
        self.assertIsNone(stats["imitation_loss"])
        self.assertEqual(len(trainer.demos), 2)
        self.assertEqual(len(trainer.online), 2)
        with self.assertRaises(RuntimeError):
            StudentTrainer(TrainConfig("mlp", "bc", "none", hidden=8)).pretrain(1)


class TrainTest(unittest.TestCase):
    def test_bc_does_not_step_the_critic(self):
        trainer = StudentTrainer(TrainConfig(
            "mlp", "bc", "llm", imitation_weight=1.0, imitation_decay=0.5, hidden=8,
        ))
        trainer.add_demonstrations(rule_demos(4, seed=1))
        before = [param.detach().clone() for param in trainer.critic.parameters()]
        stats = trainer.update(None, seed=0)
        self.assertIsNone(stats["critic_loss"])
        self.assertIsNotNone(stats["imitation_loss"])
        self.assertAlmostEqual(trainer.weight, 0.5)
        for param, old in zip(trainer.critic.parameters(), before):
            self.assertTrue(torch.equal(param, old))

    def test_pretrain_loss_decreases_and_rl_then_fits_the_critic(self):
        trainer = StudentTrainer(TrainConfig(
            "mlp", "bc_then_rl", "llm", imitation_weight=0.0, hidden=16,
        ))
        for group in trainer.td3.actor_opt.param_groups:
            group["lr"] = 1e-2
        trainer.add_demonstrations(rule_demos(24, seed=2))
        losses = trainer.pretrain(40, batch_size=8, seed=0)
        self.assertLess(sum(losses[-10:]) / 10.0, losses[0])
        for _ in range(4):
            trainer.online.add(**transition(
                2, FEAT_DIM, GLOBAL_DIM, torch.zeros(2, 1), torch.full((2, 1), 0.2), 0.1,
            ))
        before = [param.detach().clone() for param in trainer.critic.parameters()]
        batch = trainer.online.sample(4, generator=torch.Generator().manual_seed(1))
        stats = trainer.update(batch, seed=1)
        self.assertTrue(math.isfinite(stats["critic_loss"]))
        self.assertIsNone(stats["imitation_loss"])
        moved = any(not torch.equal(param, old) for param, old in zip(trainer.critic.parameters(), before))
        self.assertTrue(moved)

    def test_imitation_weight_scales_the_actor_step(self):
        def moved(weight):
            torch.manual_seed(0)
            trainer = StudentTrainer(TrainConfig(
                "mlp", "rl", "llm", imitation_weight=weight, imitation_decay=1.0, hidden=8,
            ))
            trainer.td3.actor_opt = torch.optim.SGD(trainer.actor.parameters(), lr=0.1)
            trainer.td3.policy_delay = 10**9
            trainer.add_demonstrations(rule_demos(4, seed=0))
            before = [param.detach().clone() for param in trainer.actor.parameters()]
            for _ in range(2):
                trainer.online.add(**transition(
                    2, FEAT_DIM, GLOBAL_DIM, torch.zeros(2, 1), torch.zeros(2, 1), 0.0,
                ))
            trainer.update(trainer.online.sample(2, generator=torch.Generator().manual_seed(0)), seed=0)
            return sum((param - old).abs().sum().item() for param, old in zip(trainer.actor.parameters(), before))

        full = moved(1.0)
        quarter = moved(0.25)
        self.assertGreater(full, 0.0)
        self.assertAlmostEqual(full / quarter, 4.0, places=3)

    def test_the_critic_reads_the_executed_online_action(self):
        trainer = StudentTrainer(TrainConfig("mlp", "rl", "llm", imitation_weight=0.0, hidden=8))
        demo = rule_demo(True)
        demo.continuous = torch.full((2, 1), 0.8)
        trainer.add_demonstrations([demo])
        proposal = torch.full((2, 1), 0.15)
        executed = torch.full((2, 1), -0.4)
        for _ in range(4):
            trainer.online.add(**transition(2, FEAT_DIM, GLOBAL_DIM, proposal, executed, 0.0))
        batch = trainer.online.sample(4, generator=torch.Generator().manual_seed(0))
        seen = []
        real = trainer.td3.critic.forward

        def wrapped(global_state, actions, node_mask, node_feat):
            seen.append(actions.detach().clone())
            return real(global_state, actions, node_mask, node_feat)

        trainer.td3.critic.forward = wrapped
        trainer.td3.policy_delay = 2
        trainer.update(batch, seed=0)
        self.assertEqual(len(seen), 1)
        self.assertTrue(torch.equal(seen[0], batch["executed"]))
        self.assertFalse(torch.allclose(seen[0], batch["action"]))
        self.assertFalse(torch.allclose(seen[0], demo.continuous.expand_as(seen[0])))

    def test_rl_fine_tune_reduces_the_actor_error(self):
        torch.manual_seed(0)
        nodes, feat, gdim = 3, 3, 4
        obs = torch.zeros(nodes, feat)
        obs[:, 0] = torch.tensor([0.2, 0.6, 0.9])
        history = torch.zeros(2, nodes, feat)
        topology = torch.zeros(nodes, nodes)
        topology[0, 1] = topology[1, 0] = topology[1, 2] = topology[2, 1] = 1
        mask = torch.ones(nodes, dtype=torch.bool)
        target = 0.5 * obs[:, 0] - 0.2
        global_state = torch.tensor([0.25, 0.0, 0.0, 1.0])
        config = TrainConfig("mlp", "rl", "none", feat_dim=feat, hidden=32)
        actor = build_actor(config.actor_config())
        critic = TwinCritic(gdim, 1, feat, hidden=64)
        trainer = StudentTrainer(config, actor=actor, critic=critic)
        for opt in (trainer.td3.actor_opt, trainer.td3.critic_opt):
            for group in opt.param_groups:
                group["lr"] = 3e-3
        trainer.td3.gamma = 0.0
        trainer.td3.tau = 0.05
        trainer.td3.policy_noise = 0.0
        trainer.td3.noise_clip = 0.0
        trainer.td3.policy_delay = 10**9
        replay = trainer.online
        self.assertIsInstance(replay, Replay)
        for i in range(256):
            generator = torch.Generator().manual_seed(10_000 + i)
            proposal = torch.empty(nodes, 1).uniform_(-1.0, 1.0, generator=generator)
            executed = constrain_actions(proposal, limit=1.0, node_mask=mask)
            reward = -float(((executed[:, 0] - target) ** 2).mean())
            replay.add(
                obs=obs, history=history, topology=topology, node_mask=mask,
                action=proposal, executed=executed, reward=reward, done=False,
                next_obs=obs, next_history=history, next_topology=topology, next_mask=mask,
                global_state=global_state, next_global=global_state,
            )
        for step in range(80):
            stats = trainer.update(replay.sample(64, generator=torch.Generator().manual_seed(step)))
            self.assertTrue(math.isfinite(stats["critic_loss"]))
            self.assertIsNone(stats["imitation_loss"])
        with torch.no_grad():
            before = float(((actor(obs[None], history[None], topology[None], mask[None])[0, :, 0] - target) ** 2).mean())
        trainer.td3.policy_delay = 1
        trainer.td3.updates = 0
        for step in range(40):
            stats = trainer.update(replay.sample(64, generator=torch.Generator().manual_seed(2000 + step)))
            self.assertTrue(math.isfinite(stats["actor_loss"]))
        with torch.no_grad():
            after = float(((actor(obs[None], history[None], topology[None], mask[None])[0, :, 0] - target) ** 2).mean())
        self.assertLess(after, before, f"{before} -> {after}")

    def test_save_load_and_inference_do_not_call_the_teacher(self):
        config = TrainConfig("mlp", "rl", "none", hidden=8)
        trainer = StudentTrainer(config)
        self.assertNotIn("llmctl", student_act.__code__.co_names)
        self.assertNotIn("execute", student_act.__code__.co_names)
        obs = torch.zeros(1, 2, FEAT_DIM)
        history = torch.zeros(1, 1, 2, FEAT_DIM)
        topology = torch.zeros(1, 2, 2)
        mask = torch.ones(1, 2, dtype=torch.bool)
        trainer.queue.offer("pending", {"daemons": []}, run="/data/hotel/r")
        action = student_act(trainer.actor, obs, history, topology, mask)
        self.assertEqual(tuple(action.shape), (1, 2, 1))
        self.assertIn("pending", trainer.queue.pending)
        self.assertEqual(trainer.queue.ready, [])
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "actor.pt")
            trainer.save(path)
            other = StudentTrainer(config)
            other.load(path)
        for left, right in zip(trainer.actor.parameters(), other.actor.parameters()):
            self.assertTrue(torch.equal(left, right))


class QueueTest(unittest.TestCase):
    def test_offer_returns_before_the_label_arrives(self):
        queue = CorrectiveQueue()
        called = []
        queue.label = lambda *_args, **_kwargs: called.append("label")
        state = {
            "daemons": [{
                "daemon_id": "a", "cpu_use": 0.4, "occupancy": 0.5, "pressure": 0.0,
                "cpus": [1, 2], "l3": ["d0"],
            }],
            "pool": {"cores": 4, "max": 16, "busy": 0.2, "pressure": 0.0, "allocated": 4},
            "latency": {"now": {"p95_ms": 10.0, "error_rate": 0.0, "req_per_s": 5.0}},
        }
        self.assertIsNone(queue.offer("k", state, run="/runs/hotel/r1", t_s=3.0))
        self.assertEqual(called, [])
        self.assertIn("k", queue.pending)
        queue = CorrectiveQueue()
        queue.offer("k", state, run="/runs/hotel/r1", t_s=3.0)
        demo = queue.label("k", {"name": "scale_daemon", "input": {"daemon_id": "a", "cpus": 2}, "ok": True})
        self.assertEqual(demo.workload, "hotel")
        self.assertTrue(demo.student_valid)
        self.assertAlmostEqual(demo.continuous[0, 0].item(), 0.5)
        drained = queue.drain()
        self.assertEqual(drained, [demo])
        self.assertEqual(queue.drain(), [])
        self.assertIsInstance(demo, Demonstration)


if __name__ == "__main__":
    unittest.main()
