"""Stage 3: isolation, causality, permutation, padding, and action gradients."""

from __future__ import annotations

import os
import sys
import unittest

import torch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from actors import ActorConfig, build_actor  # noqa: E402


def line_batch(nodes, feat, ticks, seed=0):
    g = torch.Generator().manual_seed(seed)
    obs = torch.randn(1, nodes, feat, generator=g)
    history = torch.randn(1, ticks, nodes, feat, generator=g)
    topology = torch.zeros(1, nodes, nodes)
    for i in range(nodes - 1):
        topology[0, i, i + 1] = 1
        topology[0, i + 1, i] = 1
    mask = torch.ones(1, nodes, dtype=torch.bool)
    return obs, history, topology, mask


def action_grad(actor, obs, history, topology, mask, index, source):
    obs = obs.detach().clone().requires_grad_(source == "obs")
    history = history.detach().clone().requires_grad_(source == "history")
    actor.zero_grad(set_to_none=True)
    action = actor(obs, history, topology, mask)
    action[0, index, 0].backward()
    return obs.grad if source == "obs" else history.grad


class ShapeTest(unittest.TestCase):
    def test_all_three_types_match(self):
        obs, history, topology, mask = line_batch(4, 3, 3)
        for kind in ("mlp", "gnn", "transformer"):
            actor = build_actor(ActorConfig(kind, 3, 2, hidden=8, taps=2, layers=1, heads=2))
            action = actor(obs, history, topology, mask)
            self.assertEqual(tuple(action.shape), (1, 4, 2), kind)
            self.assertTrue(torch.isfinite(action).all(), kind)


class IsolationTest(unittest.TestCase):
    def test_mlp_ignores_every_other_process(self):
        actor = build_actor(ActorConfig("mlp", 3, 1, hidden=8))
        obs, history, topology, mask = line_batch(4, 3, 2, seed=1)
        grad = action_grad(actor, obs, history, topology, mask, 0, "obs")
        self.assertTrue(torch.all(grad[0, 1:] == 0))
        self.assertGreater(grad[0, 0].abs().sum().item(), 0)

    def test_transformer_cannot_read_a_neighbors_current_features(self):
        actor = build_actor(ActorConfig("transformer", 3, 1, hidden=8, layers=2, heads=2))
        obs, history, topology, mask = line_batch(4, 3, 3, seed=2)
        grad = action_grad(actor, obs, history, topology, mask, 0, "obs")
        self.assertTrue(torch.all(grad[0, 1:] == 0))
        self.assertGreater(grad[0, 0].abs().sum().item(), 0)

    def test_transformer_blocks_history_that_has_not_arrived(self):
        actor = build_actor(ActorConfig("transformer", 3, 1, hidden=8, layers=2, heads=2))
        obs, history, topology, mask = line_batch(4, 3, 3, seed=3)
        grad = action_grad(actor, obs, history, topology, mask, 0, "history")
        # history[:, -1] is age 1. Node 1 is one hop, so that frame is legal.
        # Node 2 is two hops and node 3 is three hops: age 1 has not arrived.
        self.assertGreater(grad[0, -1, 1].abs().sum().item(), 0)
        self.assertTrue(torch.all(grad[0, -1, 2] == 0))
        self.assertTrue(torch.all(grad[0, -1, 3] == 0))
        # Oldest frame is age 3, which reaches the three-hop neighbor.
        self.assertGreater(grad[0, 0, 3].abs().sum().item(), 0)

    def test_gnn_delay_hides_the_neighbors_current_features(self):
        actor = build_actor(ActorConfig("gnn", 3, 1, hidden=8, taps=2))
        obs, history, topology, mask = line_batch(4, 3, 4, seed=4)
        grad = action_grad(actor, obs, history, topology, mask, 0, "obs")
        self.assertTrue(torch.all(grad[0, 1:] == 0))
        self.assertGreater(grad[0, 0].abs().sum().item(), 0)
        hist = action_grad(actor, obs, history, topology, mask, 0, "history")
        # One delayed tap per layer, two layers: the previous frame reaches
        # one hop, and a frame two steps back reaches two hops. Not three.
        self.assertGreater(hist[0, -1, 1].abs().sum().item(), 0)
        self.assertTrue(torch.all(hist[0, -1, 2] == 0))
        self.assertGreater(hist[0, -2, 2].abs().sum().item(), 0)
        self.assertTrue(torch.all(hist[0, -2, 3] == 0))


class PermutationTest(unittest.TestCase):
    def test_each_actor_follows_a_reordering_of_processes(self):
        perm = torch.tensor([2, 0, 3, 1])
        obs, history, topology, mask = line_batch(4, 3, 3, seed=5)
        for kind in ("mlp", "gnn", "transformer"):
            actor = build_actor(ActorConfig(kind, 3, 1, hidden=8, taps=2, layers=2, heads=2))
            action = actor(obs, history, topology, mask)
            moved = actor(
                obs[:, perm],
                history[:, :, perm],
                topology[:, perm][:, :, perm],
                mask[:, perm],
            )
            self.assertTrue(torch.allclose(moved, action[:, perm], atol=1e-5), kind)


class PaddingTest(unittest.TestCase):
    def test_a_masked_pad_cannot_move_the_real_actions(self):
        for kind in ("mlp", "gnn", "transformer"):
            actor = build_actor(ActorConfig(kind, 3, 1, hidden=8, taps=2, layers=2, heads=2))
            obs, history, topology, mask = line_batch(3, 3, 3, seed=6)
            base = actor(obs, history, topology, mask)
            wide_obs = torch.cat([obs, torch.full((1, 1, 3), 40.0)], 1)
            wide_hist = torch.cat([history, torch.full((1, 3, 1, 3), -40.0)], 2)
            wide_top = torch.zeros(1, 4, 4)
            wide_top[0, :3, :3] = topology[0]
            wide_top[0, :, 3] = 1
            wide_top[0, 3, :] = 1
            wide_mask = torch.tensor([[True, True, True, False]])
            wide = actor(wide_obs, wide_hist, wide_top, wide_mask)
            self.assertTrue(torch.allclose(wide[0, :3], base[0], atol=1e-5), kind)
            self.assertEqual(wide[0, 3, 0].item(), 0.0, kind)


class GradientTest(unittest.TestCase):
    def test_action_reaches_every_actor_parameter_set(self):
        obs, history, topology, mask = line_batch(3, 3, 2, seed=7)
        for kind in ("mlp", "gnn", "transformer"):
            actor = build_actor(ActorConfig(kind, 3, 1, hidden=8, taps=2, layers=1, heads=2))
            action = actor(obs, history, topology, mask)
            action.sum().backward()
            touched = [name for name, param in actor.named_parameters() if param.grad is not None and param.grad.abs().sum() > 0]
            self.assertTrue(touched, kind)


if __name__ == "__main__":
    unittest.main()
