"""Stage 2: transformer tokens, age, and the attention mask."""

from __future__ import annotations

import os
import sys
import unittest

import torch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from actors import TransformerActor, attention_mask, hop_distances  # noqa: E402


def line(nodes):
    topology = torch.zeros(1, nodes, nodes)
    for i in range(nodes - 1):
        topology[0, i, i + 1] = 1
        topology[0, i + 1, i] = 1
    return topology


class DistanceTest(unittest.TestCase):
    def test_line_and_unreachable(self):
        topology = torch.zeros(1, 4, 4)
        topology[0, 0, 1] = 0.5
        topology[0, 1, 0] = 0.5
        dist = hop_distances(topology)
        self.assertEqual(dist[0, 0, 0].item(), 0)
        self.assertEqual(dist[0, 0, 1].item(), 1)
        self.assertEqual(dist[0, 0, 2].item(), 10**6)
        self.assertEqual(dist[0, 2, 3].item(), 10**6)


class MaskTest(unittest.TestCase):
    def test_current_neighbor_and_future_are_closed(self):
        nodes, history = 4, 3
        topology = line(nodes)
        mask_nodes = torch.ones(1, nodes, dtype=torch.bool)
        mask = attention_mask(topology, mask_nodes, history)[0, 0]
        ticks = history + 1

        def allowed(query_node, query_time, key_node, key_time):
            q = query_node * ticks + query_time
            k = key_node * ticks + key_time
            return torch.isfinite(mask[q, k]).item()

        current = history
        previous = history - 1
        self.assertTrue(allowed(0, current, 0, current))
        self.assertFalse(allowed(0, current, 1, current))
        self.assertTrue(allowed(0, current, 1, previous))
        self.assertFalse(allowed(0, current, 3, previous))
        self.assertFalse(allowed(0, previous, 0, current))
        self.assertTrue(allowed(0, current, 0, 0))

    def test_absent_process_is_not_a_key(self):
        topology = line(3)
        present = torch.tensor([[True, True, False]])
        mask = attention_mask(topology, present, 2)[0, 0]
        ticks = 3
        current = 2
        q = 0 * ticks + current
        k = 2 * ticks + current
        self.assertFalse(torch.isfinite(mask[q, k]).item())


class TokenTest(unittest.TestCase):
    def test_age_is_added_to_otherwise_equal_features(self):
        actor = TransformerActor(2, 1, hidden=8, layers=1, heads=2, max_age=8)
        torch.nn.init.zeros_(actor.inp.weight)
        torch.nn.init.zeros_(actor.inp.bias)
        obs = torch.zeros(1, 2, 2)
        history = torch.zeros(1, 2, 2, 2)
        topology = torch.zeros(1, 2, 2)
        present = torch.ones(1, 2, dtype=torch.bool)
        captured = {}

        def pre(_module, inputs):
            captured["tokens"] = inputs[0].detach()

        handle = actor.blocks[0].register_forward_pre_hook(pre)
        try:
            action = actor(obs, history, topology, present)
        finally:
            handle.remove()
        self.assertEqual(tuple(action.shape), (1, 2, 1))
        # history length 2: token 0 of process 0 is age 2, token 2 is the current age 0.
        self.assertTrue(torch.allclose(captured["tokens"][0, 0], actor.age.weight[2]))
        self.assertTrue(torch.allclose(captured["tokens"][0, 2], actor.age.weight[0]))
        self.assertFalse(torch.allclose(actor.age.weight[0], actor.age.weight[2]))

    def test_history_longer_than_the_age_table_is_rejected(self):
        actor = TransformerActor(2, 1, hidden=8, layers=1, heads=2, max_age=3)
        obs = torch.zeros(1, 1, 2)
        history = torch.zeros(1, 3, 1, 2)
        topology = torch.zeros(1, 1, 1)
        present = torch.ones(1, 1, dtype=torch.bool)
        with self.assertRaises(ValueError):
            actor(obs, history, topology, present)

    def test_output_shape_for_a_changing_process_count(self):
        actor = TransformerActor(3, 2, hidden=8, layers=1, heads=2)
        for nodes in (1, 2, 5):
            obs = torch.randn(2, nodes, 3)
            history = torch.randn(2, 4, nodes, 3)
            topology = torch.zeros(2, nodes, nodes)
            present = torch.ones(2, nodes, dtype=torch.bool)
            present[:, -1] = False
            action = actor(obs, history, topology, present)
            self.assertEqual(tuple(action.shape), (2, nodes, 2))
            self.assertTrue(torch.all(action[:, -1] == 0))


if __name__ == "__main__":
    unittest.main()
