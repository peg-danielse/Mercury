"""Stage 5: one TD3 update for every actor, and a deterministic smoke run."""

from __future__ import annotations

import math
import os
import sys
import unittest

import torch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from actors import ActorConfig, build_actor  # noqa: E402
from critic import TwinCritic  # noqa: E402
from project import constrain_actions  # noqa: E402
from replay import Replay  # noqa: E402
from td3 import TD3Trainer  # noqa: E402


def fill(replay, nodes, steps, actor, obs, history, topology, mask, target, global_state):
    for _ in range(steps):
        with torch.no_grad():
            proposal = actor(obs[None], history[None], topology[None], mask[None])[0]
        executed = constrain_actions(proposal, limit=0.25, node_mask=mask)
        reward = -((executed[:, 0] - target) ** 2).mean()
        replay.add(
            obs=obs, history=history, topology=topology, node_mask=mask,
            action=proposal, executed=executed.detach(), reward=float(reward), done=False,
            next_obs=obs, next_history=history, next_topology=topology, next_mask=mask,
            global_state=global_state, next_global=global_state,
        )


def world(seed):
    torch.manual_seed(seed)
    nodes, feat, ticks, gdim = 3, 3, 2, 4
    obs = torch.zeros(nodes, feat)
    obs[:, 0] = torch.tensor([0.2, 0.6, 0.9])
    history = torch.zeros(ticks, nodes, feat)
    topology = torch.zeros(nodes, nodes)
    topology[0, 1] = topology[1, 0] = topology[1, 2] = topology[2, 1] = 1
    mask = torch.ones(nodes, dtype=torch.bool)
    target = torch.tensor([0.35, -0.2, 0.15])
    global_state = torch.tensor([0.25, 0.0, 0.0, 1.0])
    return obs, history, topology, mask, target, global_state


class UpdateTest(unittest.TestCase):
    def test_one_update_moves_each_actor_and_keeps_a_finite_critic(self):
        for kind in ("mlp", "gnn", "transformer"):
            torch.manual_seed(0)
            obs, history, topology, mask, target, global_state = world(0)
            actor = build_actor(ActorConfig(kind, 3, 1, hidden=16, taps=2, layers=1, heads=2))
            critic = TwinCritic(4, 1, 3, hidden=16)
            trainer = TD3Trainer(actor, critic, lr=1e-2, policy_delay=1, policy_noise=0.0, noise_clip=0.0)
            replay = Replay(32)
            fill(replay, 3, 8, actor, obs, history, topology, mask, target, global_state)
            before = {name: param.detach().clone() for name, param in actor.named_parameters()}
            stats = trainer.update(replay.sample(8, generator=torch.Generator().manual_seed(1)))
            self.assertTrue(math.isfinite(stats["critic_loss"]), kind)
            self.assertIsNotNone(stats["actor_loss"], kind)
            self.assertTrue(math.isfinite(stats["actor_loss"]), kind)
            moved = any(not torch.equal(param, before[name]) for name, param in actor.named_parameters())
            self.assertTrue(moved, kind)

    def test_the_update_does_not_call_the_constraint(self):
        import project
        obs, history, topology, mask, target, global_state = world(1)
        actor = build_actor(ActorConfig("mlp", 3, 1, hidden=8))
        critic = TwinCritic(4, 1, 3, hidden=8)
        trainer = TD3Trainer(actor, critic, policy_delay=1)
        replay = Replay(8)
        fill(replay, 3, 4, actor, obs, history, topology, mask, target, global_state)
        real = project.constrain_actions

        def boom(*_args, **_kwargs):
            raise AssertionError("constraint ran inside the update")

        project.constrain_actions = boom
        try:
            trainer.update(replay.sample(4, generator=torch.Generator().manual_seed(2)))
        finally:
            project.constrain_actions = real

    def test_constraint_is_what_makes_executed_differ(self):
        proposal = torch.tensor([[0.9], [-0.8], [0.1]])
        mask = torch.tensor([True, True, False])
        executed = constrain_actions(proposal, limit=0.25, node_mask=mask)
        self.assertTrue(torch.allclose(executed[:2], proposal[:2].clamp(-0.25, 0.25)))
        self.assertEqual(executed[2, 0].item(), 0.0)
        # The clip is not a rounding. A value inside the limit is unchanged.
        inside = torch.tensor([[0.2], [-0.2]])
        self.assertTrue(torch.equal(constrain_actions(inside, limit=0.25), inside))


class SmokeTest(unittest.TestCase):
    def test_each_actor_reduces_error_on_a_fixed_bandit(self):
        """Off-policy bandit: the reward is the squared error of the joint action.

        The target is an affine function of the local feature, which each actor
        can represent. The critic is fit on random proposals first (gamma 0, so
        the backup is the reward). Actor updates then start. The constraint
        limit is 1, inside the tanh range, so executed equals the proposal.
        """
        for kind in ("mlp", "gnn", "transformer"):
            torch.manual_seed(0)
            obs, history, topology, mask, _unused, global_state = world(0)
            target = 0.5 * obs[:, 0] - 0.2
            actor = build_actor(ActorConfig(kind, 3, 1, hidden=32, taps=2, layers=2, heads=4))
            critic = TwinCritic(4, 1, 3, hidden=64)
            trainer = TD3Trainer(
                actor, critic, lr=3e-3, policy_delay=10**9, tau=0.05,
                policy_noise=0.0, noise_clip=0.0, gamma=0.0,
            )
            replay = Replay(256)
            for i in range(256):
                generator = torch.Generator().manual_seed(10_000 + i)
                proposal = torch.empty(3, 1).uniform_(-1.0, 1.0, generator=generator)
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
                self.assertTrue(math.isfinite(stats["critic_loss"]), kind)
                self.assertIsNone(stats["actor_loss"], kind)

            def error():
                with torch.no_grad():
                    proposal = actor(obs[None], history[None], topology[None], mask[None])[0, :, 0]
                return float(((proposal - target) ** 2).mean())

            before = error()
            trainer.policy_delay = 1
            trainer.updates = 0
            for step in range(40):
                stats = trainer.update(replay.sample(64, generator=torch.Generator().manual_seed(2000 + step)))
                self.assertTrue(math.isfinite(stats["critic_loss"]), kind)
                self.assertTrue(math.isfinite(stats["actor_loss"]), kind)
            after = error()
            self.assertLess(after, before, f"{kind}: {before} -> {after}")


if __name__ == "__main__":
    unittest.main()
