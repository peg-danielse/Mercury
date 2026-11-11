"""Stage 4: the critic pools a variable number of processes and passes action gradients."""

from __future__ import annotations

import os
import sys
import unittest

import torch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from actors import ActorConfig, build_actor  # noqa: E402
from critic import TwinCritic  # noqa: E402


class CriticTest(unittest.TestCase):
    def test_padding_does_not_change_the_value(self):
        torch.manual_seed(0)
        critic = TwinCritic(global_dim=3, action_dim=1, feat_dim=2, hidden=16)
        g = torch.randn(1, 3)
        actions = torch.randn(1, 2, 1)
        feat = torch.randn(1, 2, 2)
        mask = torch.ones(1, 2, dtype=torch.bool)
        q1, q2 = critic(g, actions, mask, feat)
        wide_a = torch.cat([actions, torch.full((1, 1, 1), 9.0)], 1)
        wide_f = torch.cat([feat, torch.full((1, 1, 2), 9.0)], 1)
        wide_m = torch.tensor([[True, True, False]])
        p1, p2 = critic(g, wide_a, wide_m, wide_f)
        self.assertTrue(torch.allclose(q1, p1, atol=1e-6))
        self.assertTrue(torch.allclose(q2, p2, atol=1e-6))

    def test_masked_rows_get_no_action_gradient(self):
        torch.manual_seed(1)
        critic = TwinCritic(4, 1, 2, hidden=16)
        actions = torch.randn(2, 3, 1, requires_grad=True)
        mask = torch.tensor([[True, True, False], [True, False, False]])
        feat = torch.randn(2, 3, 2)
        g = torch.randn(2, 4)
        q1, _ = critic(g, actions, mask, feat)
        q1.sum().backward()
        self.assertGreater(actions.grad[0, 0].abs().sum().item(), 0)
        self.assertTrue(torch.all(actions.grad[:, :, :][mask.logical_not()] == 0))

    def test_the_same_critic_backpropagates_into_every_actor(self):
        obs = torch.randn(2, 3, 2)
        history = torch.randn(2, 2, 3, 2)
        topology = torch.zeros(2, 3, 3)
        mask = torch.ones(2, 3, dtype=torch.bool)
        g = torch.randn(2, 3)
        critic = TwinCritic(3, 1, 2, hidden=16)
        for kind in ("mlp", "gnn", "transformer"):
            actor = build_actor(ActorConfig(kind, 2, 1, hidden=8, taps=2, layers=1, heads=2))
            proposal = actor(obs, history, topology, mask)
            q1, q2 = critic(g, proposal, mask, obs)
            self.assertEqual(tuple(q1.shape), (2,))
            self.assertEqual(tuple(q2.shape), (2,))
            actor.zero_grad(set_to_none=True)
            q1.sum().backward()
            grads = [p.grad.abs().sum().item() for p in actor.parameters() if p.grad is not None]
            self.assertTrue(any(value > 0 for value in grads), kind)

    def test_an_empty_mask_is_finite(self):
        critic = TwinCritic(2, 1, 2, hidden=8)
        q1, q2 = critic(
            torch.zeros(1, 2),
            torch.ones(1, 2, 1),
            torch.zeros(1, 2, dtype=torch.bool),
            torch.ones(1, 2, 2),
        )
        self.assertTrue(torch.isfinite(q1).all() and torch.isfinite(q2).all())


if __name__ == "__main__":
    unittest.main()
