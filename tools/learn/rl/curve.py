"""Learning curve for the RL settings on one closed-loop plant.

Each round samples a load, posts the actor's continuous action, and stores the
`step_reward` of the outcome. `imitation + RL` clones the teacher first and
keeps that term on. `RL from scratch` starts from the same random actor with
the teacher term off. Every `--eval-every` updates, the greedy policy is scored
on a fixed set of loads. That score is the performance. The reward of the
transitions collected since the previous score is the returned reward.

This plant is not Mercury. A hot process (load 0.9) needs action 0.75. A cold
process (load 0.2) needs 0. Falling short cuts completions and raises the tail
and the error count. Scaling a cold process spends cores and pays `action_cost`.
The action that is scored is the continuous vector. It is not `decode_action`.

  curve.py --out data/learn/rl-curve --steps 200 --eval-every 25
"""

from __future__ import annotations

import argparse
import json
import os

import torch

from reward import step_reward
from settings import get
from student import StudentTrainer, student_act
from teacher import FEAT_DIM, GLOBAL_DIM, Demonstration, discrete_index


SLO_MS = 100.0
POOL_MAX = 8.0
LEVELS = (0.2, 0.9)
HOT = 0.75
SETTINGS = ("gnn-rl", "gnn-imitation-rl", "tf-rl", "tf-imitation-rl")


def teacher_action(load: torch.Tensor) -> torch.Tensor:
    needed = torch.where(load.reshape(-1) > 0.5, torch.full_like(load.reshape(-1), HOT), torch.zeros_like(load.reshape(-1)))
    return needed.unsqueeze(-1)


def pack(load: torch.Tensor):
    nodes = load.reshape(-1).shape[0]
    obs = torch.zeros(nodes, FEAT_DIM)
    obs[:, 0] = load.reshape(-1)
    history = torch.zeros(1, nodes, FEAT_DIM)
    topology = torch.zeros(nodes, nodes)
    if nodes == 2:
        topology[0, 1] = topology[1, 0] = 1.0
    mask = torch.ones(nodes, dtype=torch.bool)
    globe = torch.zeros(GLOBAL_DIM)
    globe[0] = load.reshape(-1).mean()
    globe[1] = load.reshape(-1).max()
    globe[2] = POOL_MAX / 64.0
    return obs, history, topology, mask, globe


def outcome(load: torch.Tensor, action: torch.Tensor) -> tuple[float, dict]:
    """One joint step. Shortfall loses completions. Excess spends cores."""
    load = load.reshape(-1).to(dtype=torch.float32)
    action = action.reshape(-1).to(dtype=torch.float32).clamp(-1.0, 1.0)
    needed = teacher_action(load).reshape(-1)
    shortfall = (needed - action).clamp(min=0)
    excess = (action - needed).clamp(min=0)
    sent = 100.0 * load.numel()
    done = float((100.0 * (1.0 - 0.8 * shortfall)).sum())
    p95 = float((SLO_MS * (0.6 + 3.0 * shortfall)).max())
    errors = float((100.0 * 0.2 * shortfall).sum())
    busy = float((0.25 + 0.7 * load * (1.0 - action.clamp(min=0))).clamp(0, 1).mean())
    cores = float((1.0 + 3.0 * action.clamp(min=0)).sum())
    value, terms = step_reward(done, sent, p95, SLO_MS, errors, busy, cores, POOL_MAX, float(excess.sum()))
    return value, terms


def eval_loads() -> list[torch.Tensor]:
    return [torch.tensor([left, right], dtype=torch.float32) for left in LEVELS for right in LEVELS]


def teacher_demos(count: int, seed: int) -> list[Demonstration]:
    generator = torch.Generator().manual_seed(seed)
    demos = []
    for ident, load in enumerate(eval_loads()):
        demos.append(_demo(load, ident))
    while len(demos) < count:
        pick = torch.randint(0, len(LEVELS), (2,), generator=generator)
        load = torch.tensor([LEVELS[int(pick[0])], LEVELS[int(pick[1])]], dtype=torch.float32)
        demos.append(_demo(load, len(demos)))
    return demos[:count]


def _demo(load: torch.Tensor, ident: int) -> Demonstration:
    obs, history, topology, mask, globe = pack(load)
    action = teacher_action(load)
    name = "scale_daemon" if float(action.abs().max()) > 0 else "hold"
    return Demonstration(
        run="plant", workload="hotel", t_s=float(ident), tool_t=None, trigger="plant",
        tool_name=name, tool_input={}, tools=[], ok=True, student_valid=True, clipped=False,
        discrete=discrete_index(name), daemon_id="p0" if name == "scale_daemon" else None,
        cpus=None, ids=["p0", "p1"], obs=obs, history=history, topology=topology, mask=mask,
        global_state=globe, continuous=action, applied=action.clone(), reward=None,
    )


def policy_action(actor, load: torch.Tensor, generator: torch.Generator | None, noise: float) -> torch.Tensor:
    obs, history, topology, mask, _ = pack(load)
    action = student_act(
        actor, obs.unsqueeze(0), history.unsqueeze(0), topology.unsqueeze(0), mask.unsqueeze(0),
    )[0]
    if noise <= 0 or generator is None:
        return action
    if noise >= 1:
        draw = torch.rand(action.shape, generator=generator) * 2 - 1
    else:
        draw = action + torch.randn(action.shape, generator=generator) * noise
    return draw.clamp(-1.0, 1.0) * mask.unsqueeze(-1).to(dtype=action.dtype)


def collect(actor, load: torch.Tensor, generator: torch.Generator | None, noise: float) -> tuple[dict, float, dict]:
    action = policy_action(actor, load, generator, noise).detach()
    reward, terms = outcome(load, action)
    obs, history, topology, mask, globe = pack(load)
    fields = {
        "obs": obs, "history": history, "topology": topology, "node_mask": mask,
        "action": action, "executed": action.clone(), "reward": reward, "done": True,
        "next_obs": obs.clone(), "next_history": history.clone(),
        "next_topology": topology.clone(), "next_mask": mask.clone(),
        "global_state": globe, "next_global": globe.clone(),
    }
    return fields, reward, terms


def sample_load(generator: torch.Generator) -> torch.Tensor:
    pick = torch.randint(0, len(LEVELS), (2,), generator=generator)
    return torch.tensor([LEVELS[int(pick[0])], LEVELS[int(pick[1])]], dtype=torch.float32)


def evaluate_actor(actor) -> dict:
    rewards = []
    terms = []
    agreed = []
    for load in eval_loads():
        action = policy_action(actor, load, None, 0.0)
        reward, term = outcome(load, action)
        rewards.append(reward)
        terms.append(term)
        target = teacher_action(load).reshape(-1)
        agreed.append(float((action.reshape(-1) - target).abs().le(0.25).float().mean()))
    def mean(key):
        return sum(item[key] for item in terms) / len(terms)
    return {
        "reward": sum(rewards) / len(rewards),
        "goodput_fraction": mean("goodput_fraction"),
        "latency_violation": mean("latency_violation"),
        "error_fraction": mean("error_fraction"),
        "agreement": sum(agreed) / len(agreed),
    }


def teacher_reference() -> dict:
    """The teacher's own action, scored on the same loads as the policies."""
    rewards = []
    terms = []
    for load in eval_loads():
        reward, term = outcome(load, teacher_action(load))
        rewards.append(reward)
        terms.append(term)
    def mean(key):
        return sum(item[key] for item in terms) / len(terms)
    return {
        "reward": sum(rewards) / len(rewards),
        "goodput_fraction": mean("goodput_fraction"),
        "latency_violation": mean("latency_violation"),
        "error_fraction": mean("error_fraction"),
        "agreement": 1.0,
    }


def run_setting(name: str, steps: int, eval_every: int, seed: int, warmup: int = 64,
                pretrain_steps: int = 60, batch_size: int = 32, noise: float = 0.25) -> list[dict]:
    """One learning curve. Step 0 is the policy before any TD3 update."""
    config = get(name).train_config()
    torch.manual_seed(seed)
    trainer = StudentTrainer(config, online_capacity=max(8000, warmup + steps + 1))
    generator = torch.Generator().manual_seed(seed)
    if config.training_mode in ("bc", "bc_then_rl"):
        trainer.add_demonstrations(teacher_demos(64, seed))
        trainer.pretrain(pretrain_steps, batch_size=min(16, 64), seed=seed)
    reference = teacher_reference()
    collected: list[float] = []
    for _ in range(warmup):
        fields, reward, _ = collect(trainer.actor, sample_load(generator), generator, noise=1.0)
        trainer.online.add(**fields)
        collected.append(reward)
    rows = []

    def mark(step: int) -> None:
        scored = evaluate_actor(trainer.actor)
        scored.update({
            "step": step,
            "setting": name,
            "actor": config.actor_type,
            "training_mode": config.training_mode,
            "train_reward": sum(collected) / len(collected) if collected else None,
            "teacher_reward": reference["reward"],
            "teacher_goodput": reference["goodput_fraction"],
        })
        rows.append(scored)

    mark(0)
    collected.clear()
    for step in range(1, steps + 1):
        fields, reward, _ = collect(trainer.actor, sample_load(generator), generator, noise)
        trainer.online.add(**fields)
        collected.append(reward)
        trainer.update(trainer.online.sample(min(batch_size, len(trainer.online)), generator), seed=seed + step)
        if step % eval_every == 0 or step == steps:
            mark(step)
            collected.clear()
    return rows


def steps_to_teacher(rows: list[dict], fraction: float = 0.9) -> int | None:
    target = rows[0]["teacher_reward"] * fraction
    for row in rows:
        if row["reward"] >= target:
            return int(row["step"])
    return None


def write_curve(path: str, rows: list[dict]) -> None:
    with open(path, "w") as handle:
        for row in rows:
            handle.write(json.dumps(row) + "\n")


def summarize(rows: list[dict]) -> list[dict]:
    by_name: dict[str, list[dict]] = {}
    for row in rows:
        by_name.setdefault(row["setting"], []).append(row)
    summary = []
    for name, group in by_name.items():
        best = max(group, key=lambda row: row["reward"])
        summary.append({
            "setting": name,
            "actor": group[0]["actor"],
            "training_mode": group[0]["training_mode"],
            "start_reward": group[0]["reward"],
            "end_reward": group[-1]["reward"],
            "best_reward": best["reward"],
            "best_step": best["step"],
            "end_goodput": group[-1]["goodput_fraction"],
            "end_violation": group[-1]["latency_violation"],
            "end_error": group[-1]["error_fraction"],
            "end_agreement": group[-1]["agreement"],
            "teacher_reward": group[0]["teacher_reward"],
            "steps_to_90pct_teacher": steps_to_teacher(group),
        })
    return summary


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", required=True, help="directory for curve.jsonl and summary.json")
    parser.add_argument("--steps", type=int, default=200)
    parser.add_argument("--eval-every", type=int, default=25)
    parser.add_argument("--seed", type=int, default=0)
    parser.add_argument("--settings", default=",".join(SETTINGS))
    args = parser.parse_args()
    if args.steps < 1 or args.eval_every < 1:
        parser.error("--steps and --eval-every must be positive")
    os.makedirs(args.out, exist_ok=True)
    rows = []
    for name in [item for item in args.settings.split(",") if item]:
        rows.extend(run_setting(name, args.steps, args.eval_every, args.seed))
    write_curve(os.path.join(args.out, "curve.jsonl"), rows)
    summary = summarize(rows)
    with open(os.path.join(args.out, "summary.json"), "w") as handle:
        json.dump({"seed": args.seed, "steps": args.steps, "plant": "load-scale", "settings": summary}, handle, indent=2)
        handle.write("\n")
    header = f"{'setting':<20} {'start':>8} {'end':>8} {'best':>8} {'goodput':>8} {'viol':>8} {'err':>8} {'agree':>8} {'to90':>6}"
    print(header)
    for row in summary:
        reached = "-" if row["steps_to_90pct_teacher"] is None else str(row["steps_to_90pct_teacher"])
        print(f"{row['setting']:<20} {row['start_reward']:8.3f} {row['end_reward']:8.3f} {row['best_reward']:8.3f} "
              f"{row['end_goodput']:8.3f} {row['end_violation']:8.3f} {row['end_error']:8.3f} "
              f"{row['end_agreement']:8.3f} {reached:>6}")
    print(f"teacher reward {summary[0]['teacher_reward']:.3f}")
    print(os.path.join(args.out, "curve.jsonl"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
