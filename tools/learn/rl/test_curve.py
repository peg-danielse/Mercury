"""The learning-curve plant has a reward gap, and a run records both scores."""

from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest

import torch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from curve import outcome, run_setting, teacher_action, teacher_reference  # noqa: E402


class PlantTest(unittest.TestCase):
    def test_the_teacher_action_beats_ignoring_a_hot_process(self):
        hot = torch.tensor([0.9, 0.9])
        taught, _ = outcome(hot, teacher_action(hot))
        ignored, _ = outcome(hot, torch.zeros(2, 1))
        self.assertGreater(taught, ignored)
        self.assertGreater(teacher_reference()["reward"], ignored)

    def test_a_curve_records_return_and_performance(self):
        rows = run_setting("tf-imitation-rl", steps=1, eval_every=1, seed=0, warmup=8, pretrain_steps=1, batch_size=4)
        self.assertEqual([row["step"] for row in rows], [0, 1])
        for row in rows:
            self.assertEqual(row["setting"], "tf-imitation-rl")
            self.assertTrue(torch.isfinite(torch.tensor(row["reward"])))
            self.assertIn("goodput_fraction", row)
            self.assertIn("latency_violation", row)
            self.assertIn("error_fraction", row)
            self.assertIsNotNone(row["train_reward"])
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "curve.jsonl")
            with open(path, "w") as handle:
                handle.write(json.dumps(rows[0]) + "\n")
            with open(path) as handle:
                loaded = json.loads(handle.read())
        self.assertEqual(loaded["training_mode"], "bc_then_rl")


if __name__ == "__main__":
    unittest.main()
