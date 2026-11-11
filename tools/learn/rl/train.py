"""Fit one evaluation setting and write the checkpoint `--llm rl` loads.

The setting name is the whole choice. `llm` is the teacher and has no
checkpoint. The scale names train the per-process actor. `mlp-action`,
`gnn-action`, and `tf-action` clone every accepted call in a teacher turn:
the tool and whichever arguments that call uses.

Imitation reads the teacher's `llmctl.jsonl` (or a run directory). RL reads a
replay file written by `save_replay`: the transitions `Replay.add` stores,
with the measured reward already filled in. `bc_then_rl` does both, cloning
first. The action settings only clone. This does not post to Mercury. The live
controller is `llmctl.py --llm rl`.

  train.py --setting gnn-imitation --jsonl teacher.jsonl --steps 40 --out gnn-imitation.pt
  train.py --setting gnn-action --run path/to/run --steps 40 --out gnn-action.pt
  train.py --setting tf-rl --replay transitions.pt --rl-steps 40 --out tf-rl.pt
  train.py --setting gnn-imitation-rl --jsonl teacher.jsonl --steps 40 \\
      --replay transitions.pt --rl-steps 40 --out gnn-imitation-rl.pt
"""

from __future__ import annotations

import argparse

import torch

from actions import Scene, action_loss, build_action_model, save_action_model, scene_logits, targets
from replay import Replay
from settings import SETTINGS, get, save_controller
from student import StudentTrainer


def demos_from(jsonl: list[str], runs: list[str]):
    from teacher import demonstrations_from_run, parse_llmctl_jsonl

    found = []
    for path in jsonl:
        found.extend(parse_llmctl_jsonl(path))
    for path in runs:
        found.extend(demonstrations_from_run(path))
    return found


def episodes_from(jsonl: list[str], runs: list[str]) -> list[dict]:
    """Teacher episodes that still carry the state an action model reads."""
    import json

    from teacher import group_episodes

    found = []
    for path in jsonl:
        with open(path) as handle:
            records = [json.loads(line) for line in handle if line.strip()]
        found.extend(group_episodes(records, path))
    if runs:
        import os
        import sys
        learn = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "distill")
        if learn not in sys.path:
            sys.path.insert(0, learn)
        from reconstruct import from_log
        for path in runs:
            found.extend(from_log(path))
    return found


def examples_from(episodes: list[dict]) -> list[tuple[Scene, dict]]:
    """One row per accepted call in the turn, from the state logged at its start.

    A refused call is skipped, and a later accepted call in that turn is kept.
    A turn with no call is hold. History carries forward once per turn.
    """
    previous: dict[str, tuple] = {}
    rows = []
    for episode in episodes:
        run = str(episode.get("run") or "")
        state = episode.get("state") or {}
        calls = [call for call in (episode.get("calls") or []) if call.get("name") != "inspect"]
        scene = Scene(state, previous.get(run))
        previous[run] = (scene.ids, scene.obs)
        if not calls:
            target = targets(scene, None)
            if target is not None:
                rows.append((scene, target))
            continue
        for call in calls:
            if not call.get("ok", True):
                continue
            target = targets(scene, call)
            if target is None:
                continue
            rows.append((scene, target))
    return rows


def tool_sampling(rows: list) -> torch.Tensor:
    """Row probabilities proportional to 1 / sqrt(rows with the same tool), so rare tools are drawn more often."""
    counts: dict[int, int] = {}
    for _scene, target in rows:
        counts[target["tool"]] = counts.get(target["tool"], 0) + 1
    weights = torch.tensor([counts[target["tool"]] ** -0.5 for _scene, target in rows], dtype=torch.float64)
    return weights / weights.sum()


def fit_action(name: str, episodes: list[dict], steps: int, out: str | None = None, seed: int = 0,
               batch_size: int = 16, lr: float = 1e-3, hidden: int = 32, model=None, rows=None,
               balance: bool = False, probs: torch.Tensor | None = None):
    """Clone every teacher tool. Returns `(model, losses)`.

    `rows` replaces `examples_from(episodes)`. `model` continues from those weights.
    Each row is `(scene, target)`. Tools and CPU steps Mercury would refuse in
    that scene are masked. `balance` draws rows by `tool_sampling`; `probs`
    gives each row's draw probability instead.
    """
    setting = get(name)
    if setting.family != "action":
        raise ValueError(f"{name} is a scale setting")
    if rows is None:
        rows = examples_from(episodes)
    if not rows:
        raise ValueError(f"{name} needs teacher episodes")
    if steps < 1:
        raise ValueError(f"{name} needs --steps above 0")
    if model is None:
        model = build_action_model(setting.actor_type, hidden=hidden)
    model.train()
    optimizer = torch.optim.Adam(model.parameters(), lr=lr)
    generator = torch.Generator().manual_seed(seed)
    losses = []
    if probs is None and balance:
        probs = tool_sampling(rows)
    for _ in range(steps):
        width = min(batch_size, len(rows))
        if probs is None:
            pick = torch.randint(0, len(rows), (width,), generator=generator)
        else:
            pick = torch.multinomial(probs, width, replacement=True, generator=generator)
        optimizer.zero_grad(set_to_none=True)
        loss = None
        for index in pick.tolist():
            scene, target = rows[int(index)]
            term = action_loss(scene_logits(model, scene), target, scene)
            loss = term if loss is None else loss + term
        loss = loss / len(pick)
        loss.backward()
        optimizer.step()
        losses.append(float(loss.detach()))
    if out:
        save_action_model(model, out, setting.name)
    return model, losses


def save_replay(replay: Replay, path: str) -> None:
    torch.save({"items": replay.items}, path)


def load_replay(path: str) -> Replay:
    blob = torch.load(path, map_location="cpu", weights_only=False)
    items = blob["items"]
    replay = Replay(max(len(items), 1))
    for item in items:
        replay.add(**item)
    return replay


def fit(name: str, demos=None, replay: Replay | None = None, steps: int = 0, rl_steps: int = 0,
        out: str | None = None, seed: int = 0, batch_size: int = 16) -> StudentTrainer:
    """Run the procedure the setting names. Writes `out` when it is set."""
    setting = get(name)
    config = setting.train_config()
    trainer = StudentTrainer(config)
    clones = config.training_mode in ("bc", "bc_then_rl")
    reinforces = config.training_mode in ("rl", "bc_then_rl")
    if clones:
        usable = [demo for demo in (demos or []) if demo.student_valid]
        if not usable:
            raise ValueError(f"{name} needs teacher demonstrations")
        if steps < 1:
            raise ValueError(f"{name} needs --steps above 0")
        trainer.add_demonstrations(usable)
        trainer.pretrain(steps, batch_size=batch_size, seed=seed)
    if reinforces:
        if replay is None or len(replay) < 1 or rl_steps < 1:
            raise ValueError(f"{name} needs a replay and --rl-steps above 0")
        generator = torch.Generator().manual_seed(seed)
        width = min(batch_size, len(replay))
        for step in range(rl_steps):
            trainer.update(replay.sample(width, generator), seed=seed + step)
    if out:
        save_controller(trainer.actor, out, setting, config)
    return trainer


def main() -> int:
    learned = [item.name for item in SETTINGS if item.learned]
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--setting", required=True, choices=learned)
    parser.add_argument("--jsonl", action="append", default=[], help="a teacher llmctl.jsonl; repeatable")
    parser.add_argument("--run", action="append", default=[], help="a run directory with llmctl.jsonl; repeatable")
    parser.add_argument("--replay", help="transitions saved by save_replay")
    parser.add_argument("--steps", type=int, default=0, help="behavior-cloning steps")
    parser.add_argument("--rl-steps", type=int, default=0, help="TD3 updates")
    parser.add_argument("--batch-size", type=int, default=16)
    parser.add_argument("--seed", type=int, default=0)
    parser.add_argument("--balance", action="store_true", help="action settings: draw rare tools more often")
    parser.add_argument("--out", required=True, help="checkpoint for llmctl.py --llm rl --model")
    args = parser.parse_args()
    if get(args.setting).family == "action":
        if args.replay or args.rl_steps:
            parser.error(f"{args.setting} clones the teacher and does not take a replay")
        if not args.jsonl and not args.run:
            parser.error(f"{args.setting} needs --jsonl or --run")
        fit_action(args.setting, episodes_from(args.jsonl, args.run), args.steps, args.out, args.seed, args.batch_size,
                   balance=args.balance)
    else:
        demos = demos_from(args.jsonl, args.run) if args.jsonl or args.run else None
        replay = load_replay(args.replay) if args.replay else None
        fit(args.setting, demos, replay, args.steps, args.rl_steps, args.out, args.seed, args.batch_size)
    print(f"{args.setting} -> {args.out}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
