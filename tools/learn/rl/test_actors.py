"""Stage 1: shared actor interface and the LocalGNNDB adapter."""

from __future__ import annotations

import os
import sys
import unittest

import torch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

from actors import ActorConfig, GnnActor, MlpActor, build_actor, sequence  # noqa: E402
from train_gnn import LocalGNNDB  # noqa: E402


def batch(batch_size, nodes, feat, ticks, seed=0):
    g = torch.Generator().manual_seed(seed)
    obs = torch.randn(batch_size, nodes, feat, generator=g)
    history = torch.randn(batch_size, ticks, nodes, feat, generator=g)
    topology = torch.rand(batch_size, nodes, nodes, generator=g)
    eye = torch.eye(nodes).expand(batch_size, nodes, nodes)
    topology = (topology * (1 - eye))
    topology = topology / topology.sum(-1, keepdim=True).clamp(min=1e-6)
    mask = torch.ones(batch_size, nodes, dtype=torch.bool)
    return obs, history, topology, mask


class InterfaceTest(unittest.TestCase):
    def test_build_mlp_and_gnn_share_the_forward_shape(self):
        obs, history, topology, mask = batch(2, 5, 4, 3)
        for kind in ("mlp", "gnn"):
            actor = build_actor(ActorConfig(kind, feat_dim=4, action_dim=2, hidden=8, taps=2))
            action = actor(obs, history, topology, mask)
            self.assertEqual(tuple(action.shape), (2, 5, 2))
            self.assertTrue(torch.all(action <= 1) and torch.all(action >= -1))

    def test_variable_width_and_a_masked_tail(self):
        actor = build_actor(ActorConfig("mlp", feat_dim=3, action_dim=1, hidden=8))
        for nodes in (1, 3, 7):
            obs, history, topology, mask = batch(1, nodes, 3, 2, seed=nodes)
            mask[:, -1] = False
            action = actor(obs, history, topology, mask)
            self.assertEqual(tuple(action.shape), (1, nodes, 1))
            self.assertEqual(action[0, -1, 0].item(), 0.0)

    def test_bad_shapes_are_rejected(self):
        actor = MlpActor(4, 1)
        obs, history, topology, mask = batch(1, 2, 4, 1)
        with self.assertRaises(ValueError):
            actor(obs, history[:, :, :1], topology, mask)
        with self.assertRaises(ValueError):
            actor(obs, history, topology, mask.float())


class GnnAdapterTest(unittest.TestCase):
    def test_demand_matches_local_gnn_bit_for_bit(self):
        torch.manual_seed(0)
        feat, nodes, ticks, hidden, taps = 5, 4, 6, 8, 3
        standalone = LocalGNNDB(feat, hidden, taps)
        actor = GnnActor(feat, action_dim=2, hidden=hidden, taps=taps)
        actor.backbone.load_state_dict(standalone.state_dict())
        features = torch.randn(ticks, nodes, feat)
        shift = torch.rand(ticks, nodes, nodes)
        shift = shift * (1 - torch.eye(nodes))
        reference = standalone(features, shift)
        got = actor.demand(features, shift)
        self.assertTrue(torch.equal(reference, got))

    def test_forward_calls_the_backbone_on_the_stacked_sequence(self):
        torch.manual_seed(1)
        actor = GnnActor(3, action_dim=1, hidden=4, taps=2)
        obs, history, topology, mask = batch(2, 3, 3, 4, seed=1)
        seen = {}
        real = actor.backbone.forward

        def record(features, shift):
            seen.setdefault("features", []).append(features.detach().clone())
            seen.setdefault("shift", []).append(shift.detach().clone())
            return real(features, shift)

        actor.backbone.forward = record
        actor(obs, history, topology, mask)
        stacked = sequence(obs, history)
        self.assertEqual(len(seen["features"]), 2)
        for row in range(2):
            self.assertTrue(torch.equal(seen["features"][row], stacked[row]))
            self.assertTrue(torch.equal(seen["shift"][row], topology[row].expand_as(seen["shift"][row])))

    def test_masked_process_does_not_change_the_others(self):
        torch.manual_seed(2)
        actor = GnnActor(3, action_dim=1, hidden=4, taps=2)
        obs, history, topology, mask = batch(1, 4, 3, 3, seed=2)
        mask = mask.clone()
        mask[0, 3] = False
        clean = actor(obs, history, topology, mask)
        poisoned = obs.clone()
        poisoned[0, 3] = 50
        history_p = history.clone()
        history_p[0, :, 3] = -50
        topology_p = topology.clone()
        topology_p[0, :, 3] = 1
        topology_p[0, 3, :] = 1
        dirty = actor(poisoned, history_p, topology_p, mask)
        self.assertTrue(torch.allclose(clean[0, :3], dirty[0, :3], atol=1e-6))
        self.assertEqual(dirty[0, 3, 0].item(), 0.0)

    def test_permutation_equivariance(self):
        torch.manual_seed(3)
        actor = GnnActor(3, action_dim=2, hidden=4, taps=2)
        obs, history, topology, mask = batch(1, 5, 3, 3, seed=3)
        perm = torch.tensor([2, 0, 4, 1, 3])
        action = actor(obs, history, topology, mask)
        moved = actor(
            obs[:, perm],
            history[:, :, perm],
            topology[:, perm][:, :, perm],
            mask[:, perm],
        )
        self.assertTrue(torch.allclose(moved, action[:, perm], atol=1e-5))


class MlpLocalTest(unittest.TestCase):
    def test_other_processes_history_and_topology_are_ignored(self):
        torch.manual_seed(4)
        actor = MlpActor(3, 1, hidden=8)
        obs, history, topology, mask = batch(1, 4, 3, 2, seed=4)
        base = actor(obs, history, topology, mask)
        obs2 = obs.clone()
        obs2[0, 1:] += 3
        history2 = history + 4
        topology2 = torch.ones_like(topology)
        other = actor(obs2, history2, topology2, mask)
        self.assertTrue(torch.allclose(base[0, 0], other[0, 0]))
        self.assertFalse(torch.allclose(base[0, 1], other[0, 1]))


if __name__ == "__main__":
    unittest.main()
