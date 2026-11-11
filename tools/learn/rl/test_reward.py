"""The scalar in tools/learn/rl/SPEC.md, on a hand-computed window."""

from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from reward import step_reward  # noqa: E402


class RewardTest(unittest.TestCase):
    def test_inside_the_slo(self):
        # goodput 1, violation 0, errors 0, bonus on, busy 0.5, cores/max 8/40, cost 0
        value, terms = step_reward(10, 10, 100, 100, 0, 0.5, 8, 40, 0)
        self.assertAlmostEqual(value, 1.0 + 0.2 * 0.5 - 0.2 * 0.2)
        self.assertEqual(terms["latency_violation"], 0.0)
        self.assertEqual(terms["utilization"], 0.5)

    def test_violation_clips_and_cancels_the_bonus(self):
        value, terms = step_reward(5, 10, 400, 100, 2, 0.9, 40, 40, 1.0)
        # goodput 0.5, violation clipped to 2, error 0.2, no bonus, resource 1, cost 1
        expected = 0.5 - 2.0 * 2.0 - 3.0 * 0.2 + 0.0 - 0.2 * 1.0 - 0.1 * 1.0
        self.assertAlmostEqual(value, expected)
        self.assertEqual(terms["latency_violation"], 2.0)
        self.assertEqual(terms["utilization"], 0.0)

    def test_slo_must_be_positive(self):
        with self.assertRaises(ValueError):
            step_reward(1, 1, 1, 0, 0, 0, 1, 1, 0)


if __name__ == "__main__":
    unittest.main()
