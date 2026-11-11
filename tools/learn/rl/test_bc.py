"""Behavior cloning on decentralized observations, for every actor type."""

from __future__ import annotations

import os
import sys
import unittest

import torch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from actors import ActorConfig, build_actor  # noqa: E402
from bc import agreement, clone, clone_step, collate, imitation_loss, load_actor, save_actor  # noqa: E402
from teacher import CPU_SCALE, FEAT_DIM, GLOBAL_DIM, Demonstration, discrete_index  # noqa: E402


def rule_demo(high: bool, ident: int = 0) -> Demonstration:
    obs = torch.zeros(2, FEAT_DIM)
    obs[0, 0] = 0.9 if high else 0.1
    obs[1, 0] = 0.1
    continuous = torch.zeros(2, 1)
    if high:
        continuous[0, 0] = 2.0 / CPU_SCALE
    return Demonstration(
        run="fixture",
        workload="hotel",
        t_s=float(ident),
        tool_t=None,
        trigger="short" if high else "idle",
        tool_name="scale_daemon" if high else "hold",
        tool_input={"daemon_id": "a", "cpus": 2} if high else {},
        tools=[],
        ok=True,
        student_valid=True,
        clipped=False,
        discrete=discrete_index("scale_daemon" if high else "hold"),
        daemon_id="a" if high else None,
        cpus=2 if high else 0,
        ids=["a", "b"],
        obs=obs,
        history=torch.zeros(1, 2, FEAT_DIM),
        topology=torch.zeros(2, 2),
        mask=torch.ones(2, dtype=torch.bool),
        global_state=torch.zeros(GLOBAL_DIM),
        continuous=continuous,
        applied=continuous.clone(),
        reward=None,
    )


def rule_demos(count: int, seed: int) -> list[Demonstration]:
    generator = torch.Generator().manual_seed(seed)
    demos = []
    for ident in range(count):
        high = bool(torch.randint(0, 2, (1,), generator=generator).item())
        demos.append(rule_demo(high, ident))
    return demos


class CloneTest(unittest.TestCase):
    def test_loss_ignores_demonstrations_the_student_cannot_represent(self):
        good = rule_demo(True)
        bad = rule_demo(False)
        bad.student_valid = False
        bad.tool_name = "grow_pool"
        bad.continuous = torch.ones(2, 1)
        prediction = torch.zeros(2, 2, 1)
        prediction[0, 0, 0] = 0.25
        mixed = imitation_loss(prediction, collate([good, bad]))
        alone = imitation_loss(prediction[:1], collate([good]))
        self.assertAlmostEqual(mixed.item(), alone.item())

    def test_the_actor_is_called_with_local_tensors_only(self):
        demo = rule_demo(True)
        actor = build_actor(ActorConfig("mlp", FEAT_DIM, 1, hidden=8))
        seen = []
        real = actor.forward

        def wrapped(obs, history, topology, mask):
            seen.append((obs, history, topology, mask))
            return real(obs, history, topology, mask)

        actor.forward = wrapped
        clone_step(actor, torch.optim.Adam(actor.parameters(), lr=1e-2), [demo])
        self.assertEqual(len(seen), 1)
        obs, history, topology, mask = seen[0]
        self.assertEqual(tuple(obs.shape), (1, 2, FEAT_DIM))
        self.assertEqual(tuple(history.shape), (1, 1, 2, FEAT_DIM))
        self.assertEqual(topology.dim(), 3)
        self.assertEqual(mask.dtype, torch.bool)
        demo.global_state = torch.ones(GLOBAL_DIM)
        after = real(
            demo.obs.unsqueeze(0), demo.history.unsqueeze(0), demo.topology.unsqueeze(0), demo.mask.unsqueeze(0),
        )
        repeated = real(
            demo.obs.unsqueeze(0), demo.history.unsqueeze(0), demo.topology.unsqueeze(0), demo.mask.unsqueeze(0),
        )
        self.assertTrue(torch.equal(after, repeated))
        self.assertFalse(any(tensor.shape == demo.global_state.shape for tensor in (obs, history, topology, mask)))

    def test_padding_a_shorter_demonstration_adds_an_absent_row(self):
        short = rule_demo(False)
        short.obs = short.obs[:1]
        short.history = short.history[:, :1]
        short.topology = short.topology[:1, :1]
        short.mask = short.mask[:1]
        short.continuous = short.continuous[:1]
        short.ids = ["a"]
        batch = collate([short, rule_demo(True)])
        self.assertEqual(tuple(batch["obs"].shape), (2, 2, FEAT_DIM))
        self.assertFalse(bool(batch["mask"][0, 1]))
        self.assertTrue(bool(batch["mask"][1, 1]))
        self.assertEqual(batch["obs"][0, 1].abs().sum().item(), 0.0)

    def test_imitation_loss_decreases_for_each_actor_on_the_same_demos(self):
        demos = rule_demos(32, seed=0)
        for kind in ("mlp", "gnn", "transformer"):
            torch.manual_seed(0)
            actor = build_actor(ActorConfig(kind, FEAT_DIM, 1, hidden=32, taps=2, layers=2, heads=4))
            losses = clone(actor, demos, steps=40, lr=1e-2, batch_size=16, seed=0)
            self.assertLess(losses[-1], losses[0], f"{kind}: {losses[0]} -> {losses[-1]}")

    def test_held_out_mlp_decodes_the_teacher_action(self):
        demos = rule_demos(64, seed=1)
        order = torch.randperm(len(demos), generator=torch.Generator().manual_seed(2)).tolist()
        held = [demos[i] for i in order[:16]]
        train = [demos[i] for i in order[16:]]
        torch.manual_seed(0)
        actor = build_actor(ActorConfig("mlp", FEAT_DIM, 1, hidden=32))
        clone(actor, train, steps=250, lr=1e-2, batch_size=16, seed=0)
        score = agreement(actor, held)
        self.assertGreaterEqual(score, 0.8, score)

    def test_agreement_skips_actions_outside_the_student_vector(self):
        class Echo(torch.nn.Module):
            def __init__(self):
                super().__init__()
                self.bias = torch.nn.Parameter(torch.zeros(1))
                self.feat_dim = FEAT_DIM
                self.action_dim = 1

            def forward(self, obs, history, topology, mask):
                return torch.zeros(obs.shape[0], obs.shape[1], 1)

        hold = rule_demo(False)
        pool = rule_demo(False)
        pool.student_valid = False
        pool.tool_name = "grow_pool"
        pool.discrete = discrete_index("grow_pool")
        self.assertEqual(agreement(Echo(), [hold, pool]), 1.0)

    def test_save_and_load_restore_the_actor(self):
        import tempfile
        torch.manual_seed(0)
        actor = build_actor(ActorConfig("mlp", FEAT_DIM, 1, hidden=8))
        clone(actor, rule_demos(8, seed=3), steps=2, lr=1e-2, batch_size=4, seed=0)
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "actor.pt")
            save_actor(actor, path, "mlp")
            other = build_actor(ActorConfig("mlp", FEAT_DIM, 1, hidden=8))
            blob = load_actor(other, path)
        self.assertEqual(blob["actor_type"], "mlp")
        for left, right in zip(actor.parameters(), other.parameters()):
            self.assertTrue(torch.equal(left, right))


if __name__ == "__main__":
    unittest.main()
