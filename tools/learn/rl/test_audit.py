"""Independent audit of the decentralized actors, the replay, and the teacher.

These checks do not reuse the stage tests' assertions. Forbidden inputs are
perturbed by a finite amount and the pre-tanh representation is compared.
A positive control perturbs an input the process is supposed to see, so a
network that ignores every input cannot pass.
"""

from __future__ import annotations

import json
import os
import sys
import tempfile
import time
import unittest

import torch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

from actors import (  # noqa: E402
    ActorConfig,
    GnnActor,
    TransformerActor,
    attention_mask,
    build_actor,
    hop_distances,
)
from critic import TwinCritic  # noqa: E402
from project import constrain_actions  # noqa: E402
from replay import Replay  # noqa: E402
from student import CorrectiveQueue, student_act  # noqa: E402
from td3 import TD3Trainer  # noqa: E402
from teacher import (  # noqa: E402
    CPU_SCALE,
    FEAT_DIM,
    decode_action,
    demonstrations_from_episodes,
    demonstrations_from_run,
    encode_call,
    local_features,
    observe,
    parse_llmctl_jsonl,
)
from train_gnn import LocalGNNDB  # noqa: E402


UNREACHABLE = 10**6


def line_topology(nodes: int) -> torch.Tensor:
    topology = torch.zeros(1, nodes, nodes)
    for i in range(nodes - 1):
        topology[0, i, i + 1] = 1.0
        topology[0, i + 1, i] = 1.0
        degree = topology[0, i].sum()
        if degree > 0:
            pass
    # Row-normalize so the GNN shift matches the teacher topology.
    totals = topology.sum(-1, keepdim=True).clamp(min=1.0)
    return topology / totals


def pack(nodes, ticks, feat, fill=0.05):
    obs = torch.full((1, nodes, feat), fill)
    history = torch.full((1, ticks, nodes, feat), fill)
    topology = line_topology(nodes)
    mask = torch.ones(1, nodes, dtype=torch.bool)
    return obs, history, topology, mask


def pre_tanh(actor, obs, history, topology, mask):
    box = {}
    module = actor.head if hasattr(actor, "head") else actor.net[-1]

    def hook(_module, inputs, output):
        box["value"] = (inputs[0] if hasattr(actor, "head") else output).detach().clone()

    handle = module.register_forward_hook(hook)
    try:
        action = actor(obs, history, topology, mask).detach()
    finally:
        handle.remove()
    return action, box["value"]


def node_slice(value, index):
    return value[0, index]


def live_mlp(feat=1, hidden=8):
    actor = build_actor(ActorConfig("mlp", feat, 1, hidden=hidden))
    with torch.no_grad():
        for module in actor.net:
            if isinstance(module, torch.nn.Linear):
                module.weight.fill_(0.05)
                module.bias.zero_()
    actor.eval()
    return actor


def live_transformer(feat=1, hidden=4, layers=3):
    """Uniform attention over the legal keys, with the feature kept in one channel.

    Query weights are zero, so every legal key gets the same weight. The value
    copies the token, and the token's first channel is the scalar feature.
    """
    actor = TransformerActor(feat, 1, hidden=hidden, layers=layers, heads=1, max_age=8)
    with torch.no_grad():
        actor.inp.weight.zero_()
        actor.inp.bias.zero_()
        actor.inp.weight[0, 0] = 1.0
        actor.age.weight.zero_()
        for block in actor.blocks:
            block.q.weight.zero_()
            block.q.bias.zero_()
            block.kv.weight.zero_()
            block.kv.bias.zero_()
            block.kv.weight[hidden:, :].copy_(torch.eye(hidden))
            block.proj.weight.copy_(torch.eye(hidden))
            block.proj.bias.zero_()
            block.ff[0].weight.zero_()
            block.ff[0].bias.zero_()
            block.ff[2].weight.zero_()
            block.ff[2].bias.zero_()
        actor.ln.bias.zero_()
        actor.ln.weight.fill_(1.0)
        actor.head.weight.zero_()
        actor.head.weight[0, 0] = 1.0
        actor.head.bias.zero_()
    actor.eval()
    return actor


def live_gnn(feat=1, hidden=2, taps=2):
    actor = GnnActor(feat, 1, hidden=hidden, taps=taps)
    eye = torch.eye(hidden)
    with torch.no_grad():
        actor.backbone.g1.W.zero_()
        actor.backbone.g1.b.zero_()
        actor.backbone.g1.W[0, 0, 0] = 1.0
        actor.backbone.g1.W[1, 0, 1] = 1.0
        actor.backbone.g2.W.zero_()
        actor.backbone.g2.b.zero_()
        actor.backbone.g2.W[0].copy_(eye)
        actor.backbone.g2.W[1].copy_(eye)
        actor.backbone.readout.weight.copy_(torch.ones(1, hidden))
        actor.backbone.readout.bias.zero_()
        actor.head.weight.fill_(1.0)
        actor.head.bias.zero_()
    actor.eval()
    return actor


def bfs_distances(topology: torch.Tensor) -> torch.Tensor:
    """Independent shortest paths. topology is [N, N]; an edge is a positive off-diagonal."""
    nodes = topology.shape[0]
    dist = torch.full((nodes, nodes), UNREACHABLE)
    for src in range(nodes):
        dist[src, src] = 0
        queue = [src]
        head = 0
        while head < len(queue):
            node = queue[head]
            head += 1
            for nxt in range(nodes):
                if nxt != node and topology[node, nxt] > 0 and dist[src, nxt] == UNREACHABLE:
                    dist[src, nxt] = dist[src, node] + 1
                    queue.append(nxt)
    return dist


def daemon(ident, cpu_use, l3, **extra):
    row = {
        "daemon_id": ident,
        "cpu_use": cpu_use,
        "occupancy": 0.4,
        "pressure": 0.05,
        "cpus": [1],
        "replica": False,
        "throttled": False,
        "cpu_wait": 0.0,
        "ipc": 1.2,
        "llc_per_ms": 4.0,
        "l3": list(l3),
    }
    row.update(extra)
    return row


def state_of(daemons, p95=25.0, cores=8):
    return {
        "pool": {"cores": cores, "max": 40, "busy": 0.3, "pressure": 0.02, "allocated": 12},
        "latency": {"now": {"p95_ms": p95, "error_rate": 0.0, "req_per_s": 80.0}},
        "daemons": daemons,
    }


class DecentralizationTest(unittest.TestCase):
    def test_forbidden_neighborhood_does_not_change_process_output(self):
        """Process 0 on a line may read itself and a 1-hop neighbor's past.

        Nodes with no path, the neighbor's current features, and the other
        batch row are outside that neighborhood.
        """
        ticks, feat = 3, 1
        # Nodes 0-1-2-3 are a line. Node 4 has no edge.
        obs, history, topology, mask = pack(5, ticks, feat)
        topology[0, 3, 4] = 0
        topology[0, 4, 3] = 0
        obs = obs.repeat(2, 1, 1)
        history = history.repeat(2, 1, 1, 1)
        topology = topology.repeat(2, 1, 1)
        mask = mask.repeat(2, 1)
        topology[1].zero_()
        cases = {"gnn": live_gnn(), "transformer": live_transformer(), "mlp": live_mlp()}
        for name, actor in cases.items():
            base_action, base_rep = pre_tanh(actor, obs, history, topology, mask)

            def check(label, mutated_obs, mutated_history, must_change):
                action, rep = pre_tanh(actor, mutated_obs, mutated_history, topology, mask)
                delta = (node_slice(rep, 0) - node_slice(base_rep, 0)).abs().max().item()
                if must_change:
                    self.assertGreater(delta, 1e-3, f"{name} {label} delta {delta}")
                else:
                    self.assertLess(delta, 1e-5, f"{name} {label} leaked, delta {delta}")
                    self.assertLess(
                        (action[0, 0] - base_action[0, 0]).abs().max().item(),
                        1e-5,
                        name,
                    )

            # Neighbor's current observation has not arrived.
            current = obs.clone()
            current[0, 1] = 3.0
            check("neighbor current", current, history, must_change=False)
            # Own current observation is inside the neighborhood.
            own = obs.clone()
            own[0, 0] = 3.0
            check("own current", own, history, must_change=True)
            # Node 4 is not on the line. None of its frames are in the neighborhood.
            isolated_obs = obs.clone()
            isolated_hist = history.clone()
            isolated_obs[0, 4] = 3.0
            isolated_hist[0, :, 4] = 3.0
            check("isolated process", isolated_obs, isolated_hist, must_change=False)
            # The other batch element is not a neighbor.
            other = obs.clone()
            other[1] = 3.0
            check("other batch row", other, history, must_change=False)
            # MLP additionally ignores the neighbor's arrived history.
            if name == "mlp":
                arrived = history.clone()
                arrived[0, -1, 1] = 3.0
                check("mlp neighbor history", obs, arrived, must_change=False)


class CausalityTest(unittest.TestCase):
    def test_unarrived_frames_do_not_change_the_current_action(self):
        torch.manual_seed(0)
        ticks, feat = 4, 1
        obs, history, topology, mask = pack(4, ticks, feat)
        gnn = live_gnn()
        base_action, base_rep = pre_tanh(gnn, obs, history, topology, mask)

        def delta(obs2, history2):
            _action, rep = pre_tanh(gnn, obs2, history2, topology, mask)
            return (node_slice(rep, 0) - node_slice(base_rep, 0)).abs().max().item()

        # Age of history index t is ticks - t. Hop 2 needs age >= 2.
        too_new = history.clone()
        too_new[0, -1, 2] = 3.0  # age 1
        self.assertLess(delta(obs, too_new), 1e-5, delta(obs, too_new))
        arrived = history.clone()
        arrived[0, -2, 2] = 3.0  # age 2
        self.assertGreater(delta(obs, arrived), 1e-3, delta(obs, arrived))
        # A 1-hop neighbor's previous frame has arrived. The frame before that
        # is not required for the positive control; the most recent history has.
        neighbor = history.clone()
        neighbor[0, -1, 1] = 3.0
        self.assertGreater(delta(obs, neighbor), 1e-3)

        crafted = live_transformer()
        _action, rep0 = pre_tanh(crafted, obs, history, topology, mask)
        _action, rep_new = pre_tanh(crafted, obs, too_new, topology, mask)
        _action, rep_old = pre_tanh(crafted, obs, arrived, topology, mask)
        crafted_leak = (node_slice(rep_new, 0) - node_slice(rep0, 0)).abs().max().item()
        crafted_hit = (node_slice(rep_old, 0) - node_slice(rep0, 0)).abs().max().item()
        self.assertLess(crafted_leak, 1e-5, crafted_leak)
        self.assertGreater(crafted_hit, 1e-3, crafted_hit)
        leaked = []
        for seed in range(4):
            torch.manual_seed(seed)
            actor = TransformerActor(feat, 1, hidden=8, layers=3, heads=2, max_age=8).eval()
            _action, rep0 = pre_tanh(actor, obs, history, topology, mask)
            _action, rep_new = pre_tanh(actor, obs, too_new, topology, mask)
            leaked.append((node_slice(rep_new, 0) - node_slice(rep0, 0)).abs().max().item())
        self.assertLess(max(leaked), 1e-5, leaked)

    def test_attention_mask_matches_an_independent_bfs(self):
        torch.manual_seed(1)
        nodes, history_len = 6, 4
        topology = torch.zeros(2, nodes, nodes)
        # Batch 0: a line. Batch 1: a different directed graph, plus an absent node.
        for i in range(nodes - 1):
            topology[0, i, i + 1] = 1
            topology[0, i + 1, i] = 1
        topology[1, 0, 1] = 0.4
        topology[1, 1, 2] = 0.4
        topology[1, 2, 4] = 0.4
        topology[1, 3, 0] = 0.2
        present = torch.ones(2, nodes, dtype=torch.bool)
        present[1, 5] = False
        got = hop_distances(topology)
        for batch in range(2):
            reference = bfs_distances(topology[batch])
            self.assertTrue(torch.equal(got[batch], reference), batch)
        mask = attention_mask(topology, present, history_len)[:, 0]
        ticks = history_len + 1
        for batch in range(2):
            dist = bfs_distances(topology[batch])
            for nq in range(nodes):
                for tq in range(ticks):
                    q = nq * ticks + tq
                    for nk in range(nodes):
                        for tk in range(ticks):
                            k = nk * ticks + tk
                            age = history_len - tk
                            allowed = (
                                tk <= tq
                                and age >= int(dist[nq, nk])
                                and bool(present[batch, nq])
                                and bool(present[batch, nk])
                            )
                            finite = torch.isfinite(mask[batch, q, k]).item()
                            if bool(present[batch, nq]):
                                self.assertEqual(finite, allowed, (batch, nq, tq, nk, tk))
                            elif finite:
                                # An absent query may fall back to itself so softmax is defined.
                                self.assertEqual(q, k)

    def test_actor_forward_does_not_read_the_next_state(self):
        torch.manual_seed(0)
        actor = build_actor(ActorConfig("transformer", 3, 1, hidden=8, layers=1, heads=2))
        critic = TwinCritic(4, 1, 3, hidden=8)
        trainer = TD3Trainer(actor, critic, policy_delay=1, policy_noise=0.0, noise_clip=0.0)
        obs = torch.randn(2, 3, 3)
        history = torch.randn(2, 2, 3, 3)
        topology = line_topology(3).repeat(2, 1, 1)
        mask = torch.ones(2, 3, dtype=torch.bool)
        batch = {
            "obs": obs, "history": history, "topology": topology, "node_mask": mask,
            "action": torch.zeros(2, 3, 1), "executed": torch.zeros(2, 3, 1),
            "reward": torch.zeros(2), "done": torch.zeros(2),
            "next_obs": obs.clone(), "next_history": history.clone(),
            "next_topology": topology.clone(), "next_mask": mask.clone(),
            "global_state": torch.zeros(2, 4), "next_global": torch.zeros(2, 4),
        }
        seen = []
        real = actor.forward

        def wrapped(obs_in, history_in, topology_in, mask_in):
            seen.append((obs_in.detach().clone(), history_in.detach().clone()))
            return real(obs_in, history_in, topology_in, mask_in)

        actor.forward = wrapped
        trainer.actor_target.forward = lambda *args, **kwargs: torch.zeros(2, 3, 1)
        trainer.update(batch)
        actor.forward = real
        self.assertTrue(torch.equal(seen[0][0], obs))
        scrambled = {key: value.clone() if torch.is_tensor(value) else value for key, value in batch.items()}
        scrambled["next_obs"] = torch.randn_like(obs)
        scrambled["next_history"] = torch.randn_like(history)
        proposal = real(batch["obs"], batch["history"], batch["topology"], batch["node_mask"])
        again = real(scrambled["obs"], scrambled["history"], scrambled["topology"], scrambled["node_mask"])
        self.assertTrue(torch.equal(proposal, again))


class EquivarianceTest(unittest.TestCase):
    def test_process_permutation(self):
        torch.manual_seed(4)
        nodes, feat, ticks = 7, 3, 3
        obs = torch.randn(2, nodes, feat)
        history = torch.randn(2, ticks, nodes, feat)
        topology = torch.rand(2, nodes, nodes)
        topology = topology * (1 - torch.eye(nodes))
        mask = torch.ones(2, nodes, dtype=torch.bool)
        mask[0, 5] = False
        mask[1, 1] = False
        perm = torch.tensor([3, 0, 6, 1, 4, 2, 5])
        errors = {}
        for kind in ("mlp", "gnn", "transformer"):
            actor = build_actor(ActorConfig(kind, feat, 2, hidden=8, taps=2, layers=2, heads=2)).eval()
            action = actor(obs, history, topology, mask)
            moved = actor(
                obs[:, perm],
                history[:, :, perm],
                topology[:, perm][:, :, perm],
                mask[:, perm],
            )
            err = (moved - action[:, perm]).abs().max().item()
            errors[kind] = err
            self.assertLess(err, 1e-4, errors)
        print("PERMUTATION_MAX_ABS", errors)


class VariableSizeTest(unittest.TestCase):
    def test_sizes_and_padding(self):
        torch.manual_seed(5)
        populations = (1, 2, 8, 16, 32)
        errors = {}
        for kind in ("mlp", "gnn", "transformer"):
            actor = build_actor(ActorConfig(kind, 3, 1, hidden=8, taps=2, layers=2, heads=2)).eval()
            for nodes in populations:
                obs = torch.randn(2, nodes, 3)
                history = torch.randn(2, 3, nodes, 3)
                topology = line_topology(nodes).repeat(2, 1, 1)
                mask = torch.ones(2, nodes, dtype=torch.bool)
                action = actor(obs, history, topology, mask)
                self.assertEqual(tuple(action.shape), (2, nodes, 1), (kind, nodes))
                self.assertTrue(torch.isfinite(action).all(), (kind, nodes))
                self.assertTrue(action.abs().max() <= 1 + 1e-5)
                # Hostile padding out to the next width, or to 32.
                width = nodes if nodes == 32 else max(nodes + 3, 4)
                if width == nodes:
                    continue
                pad = width - nodes
                wide_obs = torch.cat([obs, torch.full((2, pad, 3), 40.0)], 1)
                wide_hist = torch.cat([history, torch.full((2, 3, pad, 3), -40.0)], 2)
                wide_top = torch.zeros(2, width, width)
                wide_top[:, :nodes, :nodes] = topology
                wide_top[:, :, nodes:] = 1
                wide_top[:, nodes:, :] = 1
                wide_mask = torch.zeros(2, width, dtype=torch.bool)
                wide_mask[:, :nodes] = True
                wide = actor(wide_obs, wide_hist, wide_top, wide_mask)
                err = (wide[:, :nodes] - action).abs().max().item()
                errors[(kind, nodes, width)] = err
                self.assertLess(err, 1e-4, (kind, nodes, err))
                self.assertTrue(torch.all(wide[:, nodes:] == 0), kind)
        print("PADDING_MAX_ABS", errors)


class GradientTest(unittest.TestCase):
    def test_critic_gradient_reaches_transformer_encoder(self):
        torch.manual_seed(8)
        actor = build_actor(ActorConfig("transformer", 4, 1, hidden=16, layers=2, heads=4))
        critic = TwinCritic(3, 1, 4, hidden=32)
        obs = torch.randn(4, 5, 4)
        history = torch.randn(4, 2, 5, 4)
        topology = line_topology(5).repeat(4, 1, 1)
        mask = torch.ones(4, 5, dtype=torch.bool)
        proposal = actor(obs, history, topology, mask)
        self.assertIsNotNone(proposal.grad_fn)
        q1, q2 = critic(torch.randn(4, 3), proposal, mask, obs)
        (q1 + q2).sum().backward()
        needed = [
            "inp.weight",
            "age.weight",
            "blocks.0.q.weight",
            "blocks.0.kv.weight",
            "blocks.0.proj.weight",
            "blocks.0.ff.0.weight",
            "blocks.1.q.weight",
            "blocks.1.kv.weight",
            "head.weight",
        ]
        norms = {}
        for name, param in actor.named_parameters():
            if param.grad is None:
                norms[name] = None
            else:
                norms[name] = float(param.grad.abs().sum())
        missing = [name for name in needed if not norms.get(name)]
        self.assertFalse(missing, {name: norms.get(name) for name in needed})
        # Ages that this forward actually embeds are 0, 1, and 2.
        age_grad = actor.age.weight.grad[:3].abs().sum().item()
        self.assertGreater(age_grad, 0.0, age_grad)
        print("ENCODER_GRAD_L1", {name: norms[name] for name in needed})


class ReplayTest(unittest.TestCase):
    def test_sampled_fields_are_one_transition(self):
        replay = Replay(4)
        specs = []
        for index, (nodes, nxt, fill) in enumerate(((1, 4, 0.25), (3, 1, 1.5), (2, 2, -0.75), (5, 3, 2.25))):
            obs = torch.full((nodes, 2), fill)
            history = torch.full((2, nodes, 2), fill - 0.1)
            topology = torch.full((nodes, nodes), fill / 10)
            topology.fill_diagonal_(0)
            mask = torch.ones(nodes, dtype=torch.bool)
            action = torch.full((nodes, 1), fill + 0.01)
            executed = torch.full((nodes, 1), fill - 0.02)
            next_obs = torch.full((nxt, 2), fill + 0.3)
            next_history = torch.full((2, nxt, 2), fill + 0.4)
            next_topology = torch.eye(nxt) * (index + 1)
            next_mask = torch.ones(nxt, dtype=torch.bool)
            if nxt > 1:
                next_mask[-1] = False
            reward = 10.0 + index
            fields = dict(
                obs=obs, history=history, topology=topology, node_mask=mask,
                action=action, executed=executed, reward=reward, done=index % 2 == 0,
                next_obs=next_obs, next_history=next_history, next_topology=next_topology,
                next_mask=next_mask,
                global_state=torch.tensor([fill, index], dtype=torch.float32),
                next_global=torch.tensor([fill + 1, index + 3], dtype=torch.float32),
            )
            replay.add(**fields)
            specs.append(fields)
        # Capacity 4 keeps every spec. Sample all of them.
        batch = replay.sample(4, generator=torch.Generator().manual_seed(0))
        self.assertEqual(sorted(batch["reward"].tolist()), [10, 11, 12, 13])
        for row in range(4):
            reward = float(batch["reward"][row])
            source = specs[int(reward) - 10]
            nodes = source["obs"].shape[0]
            nxt = source["next_obs"].shape[0]
            self.assertTrue(torch.equal(batch["obs"][row, :nodes], source["obs"]))
            self.assertTrue(torch.all(batch["obs"][row, nodes:] == 0))
            self.assertTrue(torch.equal(batch["history"][row, :, :nodes], source["history"]))
            self.assertTrue(torch.equal(batch["action"][row, :nodes], source["action"]))
            self.assertTrue(torch.equal(batch["executed"][row, :nodes], source["executed"]))
            self.assertFalse(torch.equal(batch["action"][row, :nodes], batch["executed"][row, :nodes]))
            self.assertTrue(torch.equal(batch["node_mask"][row, :nodes], source["node_mask"]))
            self.assertTrue(torch.all(batch["node_mask"][row, nodes:] == False))  # noqa: E712
            self.assertTrue(torch.equal(batch["topology"][row, :nodes, :nodes], source["topology"]))
            self.assertTrue(torch.all(batch["topology"][row, nodes:] == 0))
            self.assertTrue(torch.equal(batch["next_obs"][row, :nxt], source["next_obs"]))
            self.assertTrue(torch.all(batch["next_obs"][row, nxt:] == 0))
            self.assertTrue(torch.equal(batch["next_mask"][row, :nxt], source["next_mask"]))
            self.assertTrue(torch.all(batch["next_mask"][row, nxt:] == False))  # noqa: E712
            self.assertTrue(torch.equal(batch["next_topology"][row, :nxt, :nxt], source["next_topology"]))
            self.assertTrue(torch.equal(batch["global_state"][row], source["global_state"]))
            self.assertTrue(torch.equal(batch["next_global"][row], source["next_global"]))
            self.assertEqual(float(batch["done"][row]), float(source["done"]))
        # Eviction keeps the newest transitions intact and aligned.
        replay.add(**specs[0])
        kept = replay.sample(4, generator=torch.Generator().manual_seed(1))
        self.assertEqual(sorted(kept["reward"].tolist()), [10, 11, 12, 13])
        self.assertEqual(kept["reward"].tolist().count(10.0), 1)


class LearningTest(unittest.TestCase):
    def test_deterministic_bandit_beats_random(self):
        results = []
        for kind in ("mlp", "gnn", "transformer"):
            for seed in (0, 1, 2):
                results.append(self.train(kind, seed))
        print("BANDIT", json.dumps(results))
        for row in results:
            self.assertTrue(row["finite"], row)
            self.assertGreater(row["actor_delta"], 0.0, row)
            self.assertGreater(row["target_actor_delta"], 0.0, row)
            self.assertGreater(row["target_critic_delta"], 0.0, row)
            self.assertLess(row["final_mse"], row["initial_mse"], row)
            self.assertLess(row["final_mse"], row["random_mse"], row)

    def train(self, kind, seed):
        torch.manual_seed(seed)
        nodes, feat, ticks, gdim = 3, 3, 2, 4
        obs = torch.zeros(nodes, feat)
        obs[:, 0] = torch.tensor([0.2, 0.6, 0.9])
        history = torch.zeros(ticks, nodes, feat)
        topology = torch.zeros(nodes, nodes)
        topology[0, 1] = topology[1, 0] = topology[1, 2] = topology[2, 1] = 1
        mask = torch.ones(nodes, dtype=torch.bool)
        target = 0.5 * obs[:, 0] - 0.2
        global_state = torch.tensor([0.25, 0.0, 0.0, 1.0])
        actor = build_actor(ActorConfig(kind, feat, 1, hidden=32, taps=2, layers=2, heads=4))
        critic = TwinCritic(gdim, 1, feat, hidden=64)
        trainer = TD3Trainer(
            actor, critic, lr=3e-3, policy_delay=10**9, tau=0.05,
            policy_noise=0.0, noise_clip=0.0, gamma=0.0,
        )
        replay = Replay(256)
        finite = True
        for i in range(256):
            generator = torch.Generator().manual_seed(10_000 + seed * 1000 + i)
            proposal = torch.empty(nodes, 1).uniform_(-1.0, 1.0, generator=generator)
            executed = constrain_actions(proposal, limit=1.0, node_mask=mask)
            reward = -float(((executed[:, 0] - target) ** 2).mean())
            replay.add(
                obs=obs, history=history, topology=topology, node_mask=mask,
                action=proposal, executed=executed, reward=reward, done=False,
                next_obs=obs, next_history=history, next_topology=topology, next_mask=mask,
                global_state=global_state, next_global=global_state,
            )
        for step in range(60):
            stats = trainer.update(replay.sample(64, generator=torch.Generator().manual_seed(seed * 100 + step)))
            finite = finite and torch.isfinite(torch.tensor(stats["critic_loss"])).item()

        def mse():
            with torch.no_grad():
                proposal = actor(obs[None], history[None], topology[None], mask[None])[0, :, 0]
            return float(((proposal - target) ** 2).mean())

        initial = mse()
        before_actor = {name: param.detach().clone() for name, param in actor.named_parameters()}
        before_target = {name: param.detach().clone() for name, param in trainer.actor_target.named_parameters()}
        before_critic_target = {
            name: param.detach().clone() for name, param in trainer.critic_target.named_parameters()
        }
        trainer.policy_delay = 1
        trainer.updates = 0
        for step in range(40):
            stats = trainer.update(replay.sample(64, generator=torch.Generator().manual_seed(2_000 + seed * 100 + step)))
            finite = finite and torch.isfinite(torch.tensor(stats["critic_loss"])).item()
            finite = finite and torch.isfinite(torch.tensor(stats["actor_loss"])).item()
        final = mse()
        actor_delta = sum(
            (param - before_actor[name]).abs().sum().item() for name, param in actor.named_parameters()
        )
        target_actor_delta = sum(
            (param - before_target[name]).abs().sum().item()
            for name, param in trainer.actor_target.named_parameters()
        )
        target_critic_delta = sum(
            (param - before_critic_target[name]).abs().sum().item()
            for name, param in trainer.critic_target.named_parameters()
        )
        random_mse = float((1.0 / 3.0) + (target ** 2).mean())
        return {
            "kind": kind,
            "seed": seed,
            "finite": bool(finite),
            "initial_mse": initial,
            "final_mse": final,
            "random_mse": random_mse,
            "actor_delta": actor_delta,
            "target_actor_delta": target_actor_delta,
            "target_critic_delta": target_critic_delta,
        }


class RegressionTest(unittest.TestCase):
    def test_gnn_demand_matches_local_gnn(self):
        torch.manual_seed(9)
        feat, nodes, ticks, hidden, taps = 17, 9, 5, 16, 3
        standalone = LocalGNNDB(feat, hidden, taps)
        actor = GnnActor(feat, action_dim=2, hidden=hidden, taps=taps)
        self.assertIs(type(actor.backbone), LocalGNNDB)
        actor.backbone.load_state_dict(standalone.state_dict())
        features = torch.randn(ticks, nodes, feat)
        shift = torch.rand(ticks, nodes, nodes)
        shift = shift * (1 - torch.eye(nodes))
        left = standalone(features, shift)
        right = actor.demand(features, shift)
        self.assertTrue(torch.equal(left, right))
        (left ** 2).mean().backward()
        (right ** 2).mean().backward()
        for src, dst in zip(standalone.parameters(), actor.backbone.parameters()):
            self.assertTrue(torch.equal(src.grad, dst.grad))
        print("GNN_DEMAND_SUM", float(left.detach().sum()))


class LatencyTest(unittest.TestCase):
    def test_inference_percentiles(self):
        torch.manual_seed(10)
        feat, ticks, action = 10, 4, 1
        populations = (1, 8, 16, 32)
        previous = torch.get_num_threads()
        rows = []
        try:
            torch.set_num_threads(1)
            for kind in ("mlp", "gnn", "transformer"):
                actor = build_actor(
                    ActorConfig(kind, feat, action, hidden=32, taps=3, layers=2, heads=4)
                ).eval()
                for nodes in populations:
                    obs = torch.randn(1, nodes, feat)
                    history = torch.randn(1, ticks, nodes, feat)
                    topology = line_topology(nodes)
                    mask = torch.ones(1, nodes, dtype=torch.bool)
                    with torch.inference_mode():
                        for _ in range(20):
                            actor(obs, history, topology, mask)
                        samples = []
                        for _ in range(200):
                            start = time.perf_counter()
                            action_out = actor(obs, history, topology, mask)
                            samples.append((time.perf_counter() - start) * 1e3)
                    self.assertEqual(tuple(action_out.shape), (1, nodes, action))
                    ordered = sorted(samples)
                    def pct(percent):
                        index = int(round((percent / 100) * (len(ordered) - 1)))
                        return ordered[index]
                    row = {
                        "actor": kind,
                        "N": nodes,
                        "p50_ms": pct(50),
                        "p95_ms": pct(95),
                        "p99_ms": pct(99),
                    }
                    rows.append(row)
                    self.assertLessEqual(row["p50_ms"], row["p95_ms"])
                    self.assertLessEqual(row["p95_ms"], row["p99_ms"])
                    self.assertTrue(all(map(lambda value: value == value, row.values() if False else [
                        row["p50_ms"], row["p95_ms"], row["p99_ms"],
                    ])))
        finally:
            torch.set_num_threads(previous)
        print("LATENCY", json.dumps(rows))


class TeacherAuditTest(unittest.TestCase):
    def test_global_fields_do_not_enter_the_actor_inputs(self):
        one = daemon("a", 0.2, ["d0"])
        other = daemon("b", 0.8, ["d0"], pressure=0.9, ipc=9.0, llc_per_ms=80.0, cpu_wait=0.7)
        ids, obs, history, topology, mask, global_state = observe(state_of([one, other], p95=25, cores=8))
        ids2, obs2, history2, topology2, mask2, global2 = observe(state_of([one, other], p95=400, cores=32))
        self.assertTrue(torch.equal(obs, obs2))
        self.assertTrue(torch.equal(history, history2))
        self.assertTrue(torch.equal(topology, topology2))
        self.assertTrue(torch.equal(mask, mask2))
        self.assertFalse(torch.equal(global_state, global2))
        self.assertTrue(torch.equal(obs[0], torch.tensor(local_features(one))))
        changed = daemon("b", 0.1, ["d9"])
        ids3, obs3, _, topology3, _, _ = observe(state_of([one, changed], p95=1, cores=1))
        self.assertEqual(ids3, ["a", "b"])
        self.assertTrue(torch.equal(obs[0], obs3[0]))
        self.assertEqual(float(topology3.sum()), 0.0)
        # A third daemon on another domain does not change a's row or a's neighbors.
        extra = observe(state_of([one, other, daemon("c", 0.3, ["d9"])]))
        self.assertTrue(torch.equal(extra[1][0], obs[0]))
        self.assertTrue(torch.equal(extra[3][0, :2], topology[0]))

    def test_future_episode_does_not_enter_history(self):
        episodes = []
        for stamp, cpu in ((1.0, 0.15), (2.0, 0.55), (3.0, 0.95)):
            episodes.append({
                "run": "/data/hotel/r1",
                "t_s": stamp,
                "state": state_of([daemon("a", cpu, ["d0"]), daemon("b", 0.2, ["d0"])]),
                "calls": [{"name": "hold", "input": {}, "ok": True, "t": stamp + 0.1}],
            })
        demos = demonstrations_from_episodes(episodes)
        self.assertTrue(torch.equal(demos[0].history, torch.zeros_like(demos[0].history)))
        self.assertTrue(torch.equal(demos[1].history[0], demos[0].obs))
        self.assertFalse(torch.equal(demos[0].obs, demos[2].obs))
        self.assertNotIn(float(demos[2].obs[0, 0]), [float(value) for value in demos[0].obs.reshape(-1)])
        self.assertNotIn(float(demos[2].obs[0, 0]), [float(value) for value in demos[1].history.reshape(-1)])

    def test_a_state_after_the_tool_must_not_replace_the_observation(self):
        before = state_of([daemon("svc", 0.2, ["d0"])])
        after = state_of([daemon("svc", 0.9, ["d0"])])
        records = [
            {"event": "episode", "t": 10.0, "trigger": "short"},
            {"event": "state", "t": 10.1, "state": before},
            {"event": "tool", "t": 10.2, "name": "scale_daemon",
             "input": {"daemon_id": "svc", "cpus": 1, "reason": "grow"}, "ok": True},
            {"event": "state", "t": 12.0, "state": after},
        ]
        with tempfile.TemporaryDirectory() as tmp:
            run = os.path.join(tmp, "hotel", "r1")
            os.makedirs(run)
            path = os.path.join(run, "llmctl.jsonl")
            with open(path, "w") as handle:
                for record in records:
                    handle.write(json.dumps(record) + "\n")
            parsed = parse_llmctl_jsonl(path)
            via_log = demonstrations_from_run(run)
        self.assertEqual(len(parsed), 1)
        expected = torch.tensor(local_features(before["daemons"][0]))
        leaked = torch.tensor(local_features(after["daemons"][0]))
        self.assertTrue(torch.equal(parsed[0].obs[0], expected), parsed[0].obs[0])
        self.assertFalse(torch.equal(parsed[0].obs[0], leaked))
        self.assertTrue(torch.equal(via_log[0].obs[0], expected))

    def test_corrective_label_keeps_the_history_the_student_saw(self):
        previous_ids = ["a", "b"]
        previous_obs = torch.tensor([local_features(daemon("a", 0.66, ["d0"])),
                                     local_features(daemon("b", 0.11, ["d0"]))])
        current = state_of([daemon("b", 0.2, ["d0"]), daemon("a", 0.3, ["d0"])])
        queue = CorrectiveQueue()
        queue.offer("k", current, run="/data/hotel/r1", t_s=5.0)
        demo = queue.label("k", {"name": "scale_daemon", "input": {"daemon_id": "a", "cpus": 2}, "ok": True})
        _ids, _obs, history, _topology, _mask, _global = observe(current, previous=(previous_ids, previous_obs))
        self.assertTrue(torch.equal(demo.history, history), demo.history)

    def test_train_and_eval_forward_match_on_the_same_observation(self):
        torch.manual_seed(0)
        ids, obs, history, topology, mask, _global = observe(
            state_of([daemon("a", 0.4, ["d0"]), daemon("b", 0.2, ["d0"])])
        )
        actor = build_actor(ActorConfig("transformer", FEAT_DIM, 1, hidden=8, layers=1, heads=2))
        actor.train()
        batched = (
            obs.unsqueeze(0), history.unsqueeze(0), topology.unsqueeze(0), mask.unsqueeze(0),
        )
        training = actor(*batched)
        executed = student_act(actor, *batched)
        self.assertTrue(actor.training)
        self.assertTrue(torch.allclose(training, executed))
        mapped = encode_call(ids, {"name": "scale_daemon", "input": {"daemon_id": "a", "cpus": 2}, "ok": True})
        self.assertNotIn("global", actor.forward.__code__.co_varnames)

    def test_decoded_execution_matches_the_action_the_critic_would_score(self):
        raw = torch.tensor([[0.30], [0.05], [-0.62]])
        executed = constrain_actions(raw, limit=1.0)
        name, ident, cpus = decode_action(executed, ["a", "b", "c"])
        posted = torch.zeros_like(executed)
        if name == "scale_daemon":
            posted[executed.abs().argmax()] = cpus / CPU_SCALE
        gap = (executed - posted).abs().max().item()
        self.assertLess(
            gap,
            1e-6,
            f"decode posted {name} {ident} {cpus}; max |executed - reencoded| = {gap}",
        )


if __name__ == "__main__":
    unittest.main()
