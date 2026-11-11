"""An action model can post every teacher tool, or hold when the call would be illegal."""

from __future__ import annotations

import os
import sys
import tempfile
import unittest

import torch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))

from actions import (  # noqa: E402
    CPU_VALUES, ROUTES, TOOLS, Scene, action_loss, build_action_model, choose, decode_choice, legal_logits,
    load_action_model, refusal_rate, save_action_model, scene_logits, targets, would_refuse,
)
from train import examples_from, fit_action  # noqa: E402


def machine():
    return {
        "pool": {
            "cores": 8, "max": 40, "busy": 0.2, "pressure": 0.0, "allocated": 4,
            "free_domains": ["l3.1"],
        },
        "latency": {"now": {"p95_ms": 20.0, "error_rate": 0.0, "req_per_s": 10.0}},
        "l3": [
            {"name": "l3.0", "in_pool": True, "load": 0.8, "cpus": [0, 1], "free": [], "daemons": ["hotel-frontend"]},
            {"name": "l3.1", "in_pool": False, "load": 0.1, "cpus": [2, 3], "free": [2, 3], "daemons": []},
        ],
        "degrade": {"sheddable": ["recommendations"], "shed": []},
        "daemons": [
            {"daemon_id": "hotel-frontend", "cpu_use": 0.9, "occupancy": 0.9, "pressure": 0.4,
             "cpus": [0, 1], "l3": ["l3.0"]},
            {"daemon_id": "hotel-rate-r2", "cpu_use": 0.0, "occupancy": 0.1, "pressure": 0.0,
             "cpus": [1], "l3": ["l3.0"], "replica": True},
        ],
    }


def hot(length, index):
    scores = torch.full((length,), -10.0)
    scores[index] = 10.0
    return scores


def choice(tool, service=0, cpu=2, domain=1, domain_to=1, auto=-20.0, route=0, state=None):
    scene = Scene(state or machine())
    tool_s = hot(len(TOOLS), TOOLS.index(tool))
    service_s = hot(len(scene.ids), service)
    cpu_s = torch.full((len(scene.ids), len(CPU_VALUES)), -10.0)
    cpu_s[service, CPU_VALUES.index(cpu)] = 10.0
    name, arguments, daemon_id, cpus = decode_choice(
        tool_s, service_s, cpu_s, hot(len(scene.domain_ids), domain), torch.tensor([auto]),
        hot(len(scene.domain_ids), domain_to), hot(len(ROUTES), route), scene,
    )
    return name, arguments, daemon_id, cpus


class DecodeTest(unittest.TestCase):
    def test_every_tool_decodes_to_the_call_execute_posts(self):
        self.assertEqual(choice("hold")[0], "hold")
        name, arguments, daemon, cpus = choice("scale_daemon", service=0, cpu=2)
        self.assertEqual((name, daemon, cpus), ("scale_daemon", "hotel-frontend", 2))
        self.assertEqual(arguments["cpus"], 2)
        name, arguments, daemon, _ = choice("add_replica", service=0)
        self.assertEqual((name, daemon), ("add_replica", "hotel-frontend"))
        name, arguments, daemon, _ = choice("remove_replica", service=1)
        self.assertEqual((name, arguments["daemon_id"]), ("remove_replica", "hotel-rate-r2"))
        name, arguments, _, _ = choice("grow_pool", domain=1, auto=-20)
        self.assertEqual(arguments["l3"], "l3.1")
        name, arguments, _, _ = choice("grow_pool", domain=1, auto=30)
        self.assertEqual(arguments["l3"], "auto")
        name, arguments, _, _ = choice("shrink_pool", domain=0, auto=-20)
        self.assertEqual((name, arguments["l3"]), ("shrink_pool", "l3.0"))
        name, arguments, _, _ = choice("swap_domain", domain=0, domain_to=1)
        self.assertEqual((arguments["from_l3"], arguments["to_l3"]), ("l3.0", "l3.1"))
        self.assertEqual(choice("move_daemon", service=0, domain=0, domain_to=1)[0], "hold")
        pooled = machine()
        pooled["l3"][1]["in_pool"] = True
        pooled["pool"]["free_domains"] = []
        name, arguments, daemon, _ = choice("move_daemon", service=0, domain=0, domain_to=1, state=pooled)
        self.assertEqual(name, "move_daemon")
        self.assertEqual((daemon, arguments["from_l3"], arguments["to_l3"]), ("hotel-frontend", "l3.0", "l3.1"))
        name, arguments, _, _ = choice("shed_route", route=0)
        self.assertEqual((name, arguments["route"]), ("shed_route", "recommendations"))
        state = machine()
        state["degrade"]["shed"] = ["recommendations"]
        name, arguments, _, _ = choice("restore_route", route=0, state=state)
        self.assertEqual((name, arguments["route"]), ("restore_route", "recommendations"))

    def test_illegal_calls_hold(self):
        self.assertEqual(choice("scale_daemon", service=1, cpu=-2)[0], "hold")
        only_replica = machine()
        only_replica["daemons"] = [only_replica["daemons"][1]]
        self.assertEqual(choice("add_replica", service=0, state=only_replica)[0], "hold")
        only_original = machine()
        only_original["daemons"] = [only_original["daemons"][0]]
        self.assertEqual(choice("remove_replica", service=0, state=only_original)[0], "hold")
        no_free = machine()
        no_free["pool"]["free_domains"] = []
        self.assertEqual(choice("swap_domain", domain=0, domain_to=1, state=no_free)[0], "hold")
        name, arguments, _, _ = choice("shed_route", route=1)
        self.assertEqual(arguments["route"], "recommendations")
        closed = machine()
        closed["degrade"] = {"sheddable": [], "shed": []}
        self.assertEqual(choice("shed_route", state=closed)[0], "hold")
        self.assertEqual(choice("restore_route", route=0)[0], "hold")


def full():
    """hotel-frontend holds every pool CPU, the pool is at its start size, and grow fits the budget."""
    state = machine()
    state["pool"].update(cores=2, start=2, max=4)
    state["l3"][0]["cpus"] = [0, 1]
    state["daemons"][0]["cpus"] = [0, 1]
    return state


class LegalTest(unittest.TestCase):
    def test_masks_follow_mercury_refusals(self):
        scene = Scene(full())
        self.assertTrue(would_refuse(scene, "scale_daemon", {"daemon_id": "hotel-frontend", "cpus": 2}))
        self.assertFalse(would_refuse(scene, "scale_daemon", {"daemon_id": "hotel-frontend", "cpus": -1}))
        self.assertTrue(would_refuse(scene, "shrink_pool", {"l3": "auto"}))
        self.assertFalse(would_refuse(scene, "grow_pool", {"l3": "l3.1"}))
        self.assertFalse(bool(scene.tool_legal[TOOLS.index("shrink_pool")]))
        self.assertTrue(bool(scene.tool_legal[TOOLS.index("grow_pool")]))
        front = scene.ids.index("hotel-frontend")
        self.assertFalse(bool(scene.cpu_legal[front, CPU_VALUES.index(1)]))
        self.assertTrue(bool(scene.cpu_legal[front, CPU_VALUES.index(-1)]))
        self.assertFalse(bool(scene.cpu_legal[front, CPU_VALUES.index(-2)]))
        budget = full()
        budget["pool"]["max"] = 3
        self.assertTrue(would_refuse(Scene(budget), "grow_pool", {"l3": "auto"}))

    def test_data_stores_are_not_replicated_and_moves_stay_in_the_pool(self):
        from actions import replicable
        self.assertFalse(replicable("hotel-mongod"))
        self.assertFalse(replicable("hotel-11213"))
        self.assertFalse(replicable("social-graph-redis"))
        self.assertTrue(replicable("hotel-frontend"))
        self.assertTrue(replicable("hotel-rate-r2"))
        state = machine()
        state["daemons"].append({"daemon_id": "hotel-mongod", "cpu_use": 0.1, "occupancy": 0.1, "pressure": 0.0,
                                 "cpus": [0], "l3": ["l3.0"]})
        scene = Scene(state)
        self.assertTrue(would_refuse(scene, "add_replica", {"daemon_id": "hotel-mongod"}))
        self.assertFalse(would_refuse(scene, "add_replica", {"daemon_id": "hotel-frontend"}))
        self.assertTrue(would_refuse(scene, "move_daemon",
                                     {"daemon_id": "hotel-frontend", "from_l3": "l3.0", "to_l3": "l3.1"}))
        self.assertFalse(bool(scene.tool_legal[TOOLS.index("move_daemon")]))
        name, arguments, _d, _c = choice("add_replica", service=2, state=state)
        self.assertNotEqual(arguments.get("daemon_id"), "hotel-mongod")

    def test_choose_never_posts_a_refused_call(self):
        model = build_action_model("mlp", hidden=8)
        scene = Scene(full())
        with torch.no_grad():
            model.heads_out.tool.bias.fill_(0.0)
            model.heads_out.tool.bias[TOOLS.index("shrink_pool")] = 50.0
        decision, _ = choose(model, full(), None)
        self.assertNotEqual(decision.name, "shrink_pool")
        self.assertFalse(would_refuse(scene, decision.name, decision.arguments))
        report = refusal_rate(model, [scene], masked=False)
        self.assertEqual(report["by_tool"]["shrink_pool"]["refused"], 1)
        self.assertEqual(refusal_rate(model, [scene], masked=True)["refused"], 0)

    def test_used_services_and_pool_tools_are_not_repeated(self):
        model = build_action_model("mlp", hidden=8)
        with torch.no_grad():
            model.heads_out.tool.bias.fill_(0.0)
            model.heads_out.tool.bias[TOOLS.index("grow_pool")] = 50.0
        first, _ = choose(model, machine(), None)
        self.assertEqual(first.name, "grow_pool")
        second, _ = choose(model, machine(), None, {"services": set(), "tools": {"grow_pool"}})
        self.assertNotEqual(second.name, "grow_pool")

    def test_the_target_stays_legal_and_weights_and_avoid_change_the_loss(self):
        model = build_action_model("mlp", hidden=8)
        scene = Scene(full())
        logits = scene_logits(model, scene)
        refused = targets(scene, {"name": "scale_daemon", "input": {"daemon_id": "hotel-frontend", "cpus": 2}})
        masked = legal_logits(logits, scene, refused)
        self.assertTrue(bool(torch.isfinite(masked["tool"][0, refused["tool"]])))
        plain = action_loss(logits, refused, scene)
        heavy = action_loss(logits, {**refused, "weight": 3.0}, scene)
        self.assertAlmostEqual(float(heavy), 3.0 * float(plain), places=4)
        grow = targets(scene, {"name": "grow_pool", "input": {"l3": "l3.1"}})
        avoid = action_loss(logits, {**grow, "avoid": True}, scene)
        avoid.backward()
        self.assertGreater(float(avoid), 0.0)

    def test_avoiding_one_scale_call_leaves_the_other_service_alone(self):
        model = build_action_model("mlp", hidden=8)
        scene = Scene(machine())
        bad = targets(scene, {"name": "scale_daemon", "input": {"daemon_id": "hotel-frontend", "cpus": 1}})
        other = targets(scene, {"name": "scale_daemon", "input": {"daemon_id": "hotel-rate-r2", "cpus": 1}})
        optimizer = torch.optim.SGD(model.parameters(), lr=0.5)
        before = {}
        from actions import call_log_prob

        def measure():
            with torch.no_grad():
                logits = legal_logits(scene_logits(model, scene), scene)
                service = torch.softmax(logits["service"][0], dim=-1)
                return float(call_log_prob(logits, bad)), float(service[other["service"]])

        bad_before, other_before = measure()
        for _ in range(20):
            optimizer.zero_grad()
            action_loss(scene_logits(model, scene), {**bad, "avoid": True}, scene).backward()
            optimizer.step()
        bad_after, other_after = measure()
        self.assertLess(bad_after, bad_before)
        self.assertGreater(other_after, other_before)


class ContextTest(unittest.TestCase):
    def test_the_tool_head_sees_latency_and_the_pool_budget(self):
        torch.manual_seed(0)
        model = build_action_model("mlp", hidden=8)
        calm, slow = machine(), machine()
        calm["latency"]["reference_p95_ms"] = 20.0
        slow["latency"] = {"now": {"p95_ms": 900.0, "error_rate": 0.2, "req_per_s": 10.0}, "reference_p95_ms": 20.0}
        left, right = Scene(calm), Scene(slow)
        self.assertTrue(torch.equal(left.obs, right.obs))
        self.assertFalse(torch.equal(left.context, right.context))
        with torch.no_grad():
            self.assertFalse(torch.allclose(scene_logits(model, left)["tool"], scene_logits(model, right)["tool"]))
        budget = machine()
        budget["pool"]["max"] = 8
        self.assertFalse(torch.equal(Scene(budget).context, left.context))

    def test_a_checkpoint_without_context_still_loads_and_runs(self):
        model = build_action_model("mlp", hidden=8, context=False)
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "old.pt")
            save_action_model(model, path)
            blob = torch.load(path, weights_only=False)
            blob.pop("context")
            torch.save(blob, path)
            loaded = load_action_model(path)
        self.assertFalse(loaded.context)
        decision, _ = choose(loaded, machine(), None)
        self.assertIn(decision.name, TOOLS)


class ModelTest(unittest.TestCase):
    def test_each_encoder_scores_every_head(self):
        scene = Scene(machine())
        for kind in ("mlp", "gnn", "transformer"):
            model = build_action_model(kind, hidden=8, taps=2, layers=1, heads=2)
            logits = scene_logits(model, scene)
            self.assertEqual(tuple(logits["tool"].shape), (1, len(TOOLS)))
            self.assertEqual(tuple(logits["service"].shape), (1, 2))
            self.assertEqual(tuple(logits["cpu"].shape), (1, 2, len(CPU_VALUES)))
            self.assertEqual(tuple(logits["route"].shape), (1, len(ROUTES)))
            model.eval()
            decision, carry = choose(model, machine(), None)
            self.assertIn(decision.name, TOOLS)
            self.assertEqual(carry[0], ["hotel-frontend", "hotel-rate-r2"])
            self.assertFalse(model.training)

    def test_checkpoint_roundtrip(self):
        model = build_action_model("mlp", hidden=8)
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "action.pt")
            save_action_model(model, path)
            loaded = load_action_model(path)
        self.assertEqual(loaded.actor_type, "mlp")
        for left, right in zip(model.parameters(), loaded.parameters()):
            self.assertTrue(torch.equal(left, right))


class TrainTest(unittest.TestCase):
    def test_a_refused_call_is_not_a_label_and_hold_is(self):
        state = machine()
        refused = examples_from([{"run": "a", "state": state, "calls": [
            {"name": "grow_pool", "input": {"l3": "l3.1"}, "ok": False},
        ]}])
        held = examples_from([{"run": "a", "state": state, "calls": []}])
        self.assertEqual(refused, [])
        self.assertEqual(held[0][1]["tool"], TOOLS.index("hold"))
        scene = Scene(state)
        grown = targets(scene, {"name": "add_replica", "input": {"daemon_id": "hotel-frontend"}, "ok": True})
        self.assertEqual(grown["service"], 0)
        self.assertIsNone(targets(scene, {"name": "add_replica", "input": {"daemon_id": "hotel-rate-r2"}, "ok": True}))

    def test_every_accepted_call_in_the_turn_is_a_label(self):
        rows = examples_from([{"run": "a", "state": machine(), "calls": [
            {"name": "grow_pool", "input": {"l3": "l3.1"}, "ok": False},
            {"name": "grow_pool", "input": {"l3": "l3.1"}, "ok": True},
            {"name": "scale_daemon", "input": {"daemon_id": "hotel-frontend", "cpus": 2}, "ok": True},
        ]}])
        self.assertEqual([TOOLS[row[1]["tool"]] for row in rows], ["grow_pool", "scale_daemon"])
        self.assertEqual(rows[1][1]["service"], 0)
        self.assertEqual(rows[1][1]["cpu"], CPU_VALUES.index(2))
        self.assertIs(rows[0][0], rows[1][0])

    def test_cloning_a_grow_makes_choose_post_it(self):
        episode = {"run": "a", "state": machine(), "calls": [
            {"name": "grow_pool", "input": {"l3": "l3.1"}, "ok": True},
        ]}
        model, losses = fit_action("mlp-action", [episode], steps=40, seed=0, batch_size=1, hidden=8, lr=1e-2)
        self.assertLess(losses[-1], losses[0])
        decision, _ = choose(model, machine(), None)
        self.assertEqual(decision.name, "grow_pool")
        self.assertEqual(decision.arguments["l3"], "l3.1")


class LiveTest(unittest.TestCase):
    def test_rl_episode_posts_an_action_model_call(self):
        tools = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
        sys.path.insert(0, tools)
        import llmctl

        llmctl.RL.clear()
        model = build_action_model("mlp", hidden=8)
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "action.pt")
            save_action_model(model, path)
            import argparse
            args = argparse.Namespace(model=path, current=[], max_actions=6)
            mercury = llmctl.Mercury("http://127.0.0.1:9", 50, True)
            log = llmctl.Log(os.path.join(directory, "llmctl.jsonl"))
            try:
                text = llmctl.rl_episode(args, mercury, log, "rl period", machine())
            finally:
                log.file.close()
        self.assertTrue(text.startswith("rl mlp-action:"))
        self.assertEqual(llmctl.RL["kind"], "action")
        llmctl.RL.clear()

    def test_an_action_episode_posts_several_calls_on_fresh_states(self):
        tools = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
        sys.path.insert(0, tools)
        import argparse
        import json

        import llmctl

        llmctl.RL.clear()
        model = build_action_model("mlp", hidden=8)
        with torch.no_grad():
            model.heads_out.tool.bias.fill_(0.0)
            model.heads_out.tool.bias[TOOLS.index("scale_daemon")] = 50.0
            model.heads_out.cpus.bias.fill_(0.0)
            model.heads_out.cpus.bias[CPU_VALUES.index(1)] = 50.0

        class Fresh(llmctl.Mercury):
            def state(self, since=None):
                return machine()

        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "action.pt")
            save_action_model(model, path)
            args = argparse.Namespace(model=path, current=[], max_actions=6)
            mercury = Fresh("http://127.0.0.1:9", 50, True)
            log = llmctl.Log(os.path.join(directory, "llmctl.jsonl"))
            try:
                llmctl.rl_episode(args, mercury, log, "rl period", machine())
            finally:
                log.file.close()
            with open(os.path.join(directory, "llmctl.jsonl")) as handle:
                calls = [json.loads(line) for line in handle if '"event": "tool"' in line]
        scaled = [call["input"]["daemon_id"] for call in calls if call["name"] == "scale_daemon"]
        self.assertEqual(sorted(scaled), ["hotel-frontend", "hotel-rate-r2"])
        self.assertEqual(calls[-1]["name"], "hold")
        llmctl.RL.clear()


if __name__ == "__main__":
    unittest.main()
