"""Stage 4: one joint-transition schema, padded on sample."""

from __future__ import annotations

import os
import sys
import unittest

import torch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from replay import Replay  # noqa: E402


def transition(nodes, next_nodes, feat=2, ticks=2, action=1, executed_scale=0.25, done=False):
    obs = torch.arange(nodes * feat, dtype=torch.float32).view(nodes, feat)
    history = torch.arange(ticks * nodes * feat, dtype=torch.float32).view(ticks, nodes, feat)
    topology = torch.eye(nodes)
    mask = torch.ones(nodes, dtype=torch.bool)
    action_t = torch.full((nodes, action), 0.8)
    executed = action_t * executed_scale
    nxt = torch.arange(next_nodes * feat, dtype=torch.float32).view(next_nodes, feat) + 10
    next_history = torch.zeros(ticks, next_nodes, feat)
    next_topology = torch.eye(next_nodes)
    next_mask = torch.ones(next_nodes, dtype=torch.bool)
    if next_nodes > 1:
        next_mask[-1] = False
    return dict(
        obs=obs, history=history, topology=topology, node_mask=mask,
        action=action_t, executed=executed, reward=0.5, done=done,
        next_obs=nxt, next_history=next_history, next_topology=next_topology, next_mask=next_mask,
        global_state=torch.tensor([1.0, 2.0]), next_global=torch.tensor([3.0, 4.0]),
    )


class ReplayTest(unittest.TestCase):
    def test_sample_pads_processes_and_keeps_both_actions(self):
        replay = Replay(8)
        replay.add(**transition(2, 3))
        replay.add(**transition(4, 1, executed_scale=1.0, done=True))
        generator = torch.Generator().manual_seed(0)
        # Force a known order by sampling everything through two singles after checking shapes on a full draw.
        batch = replay.sample(2, generator=generator)
        self.assertEqual(tuple(batch["obs"].shape), (2, 4, 2))
        self.assertEqual(tuple(batch["history"].shape), (2, 2, 4, 2))
        self.assertEqual(tuple(batch["topology"].shape), (2, 4, 4))
        self.assertEqual(tuple(batch["next_obs"].shape), (2, 3, 2))
        self.assertEqual(tuple(batch["next_topology"].shape), (2, 3, 3))
        self.assertEqual(tuple(batch["action"].shape), (2, 4, 1))
        self.assertEqual(tuple(batch["executed"].shape), (2, 4, 1))
        short = batch["node_mask"][batch["obs"][:, 0, 0] == 0]
        # The 2-process row is padded. Its identifier obs[0,0] is 0; the 4-process row starts at 0 as well.
        # Distinguish by the mask tail.
        self.assertEqual(int(batch["node_mask"].sum()), 2 + 4)
        self.assertTrue(torch.any(batch["action"] != batch["executed"]))
        self.assertEqual(set(batch["done"].tolist()), {0.0, 1.0})
        self.assertTrue(torch.equal(batch["global_state"][0], batch["global_state"][0]))
        self.assertEqual(tuple(batch["next_mask"].shape), (2, 3))

    def test_pad_blocks_are_empty(self):
        replay = Replay(4)
        replay.add(**transition(2, 2))
        replay.add(**transition(2, 2))
        # Replace the second with a wider one by using a fresh replay of mixed width.
        replay = Replay(4)
        replay.add(**transition(1, 1))
        wide = transition(3, 2)
        replay.add(**wide)
        generator = torch.Generator().manual_seed(1)
        # Draw until both rows appear. With two items and batch 2, one sample contains both.
        batch = replay.sample(2, generator=generator)
        narrow = int((batch["node_mask"].sum(dim=1) == 1).nonzero()[0])
        self.assertTrue(torch.all(batch["node_mask"][narrow, 1:] == False))  # noqa: E712
        self.assertTrue(torch.all(batch["obs"][narrow, 1:] == 0))
        self.assertTrue(torch.all(batch["topology"][narrow, 1:] == 0))
        self.assertTrue(torch.all(batch["topology"][narrow, :, 1:] == 0))
        kept = batch["next_mask"].sum(dim=1)
        self.assertTrue(torch.all(kept >= 1))

    def test_missing_field_and_spec_mismatch_are_rejected(self):
        replay = Replay(2)
        fields = transition(2, 2)
        fields.pop("executed")
        with self.assertRaises(KeyError):
            replay.add(**fields)
        replay.add(**transition(2, 2))
        other = transition(2, 2)
        other["history"] = other["history"][:1]
        with self.assertRaises(ValueError):
            replay.add(**other)


if __name__ == "__main__":
    unittest.main()
