"""Run an action checkpoint on Mercury, learn from what each decision did, and run it again.

Each round is one Mercury run. Every logged call gets its own advantage: the
mean `step_reward` over the `outcome` of the checks in the horizon after the
call, minus the mean over the horizon before it, minus the mean advantage of
the round's calls in the same load phase (`phase_baseline`), because a ramp
moves the reward whatever the call was. A call above that baseline is cloned
and one below it is avoided, each weighted by its standardized size. A refused
call is avoided too. Every update starts from the best round's weights. Half of
the draws are teacher rows; the student rows of the last `keep` rounds are
drawn evenly across tools (`mix_probs`). The loop stops when the critical
routes' p95 and error rate are both within the bar.

`cycle_loop` does not start Mercury. `run_mercury` is the compare_systems.sh
call the command line uses: the teacher's 0.5 s check and 1 s window, on the
teacher's trouble gate (`llmctl.py --gate`).
"""

from __future__ import annotations

import json
import math
import os
import subprocess

import torch

from actions import Scene, build_action_model, load_action_model, save_action_model, targets
from reward import step_reward
from teacher import group_episodes
from train import episodes_from, examples_from, fit_action, tool_sampling

HORIZON_S = 5.0
DELAY_S = 1.0
MAX_WEIGHT = 3.0
DEADZONE = 0.25
RAMP_S = 10.0
MIN_BUCKET = 5
TEACHER_SHARE = 0.5


def percentile_ms(hist: dict, q: float) -> float:
    items = sorted((int(bound), count) for bound, count in hist.items())
    total = sum(count for _, count in items)
    if not total:
        return float("nan")
    acc = 0
    for bound, count in items:
        acc += count
        if acc >= q * total:
            return bound / 1e6
    return items[-1][0] / 1e6


def _summary(sent: int, errors: int, hist: dict) -> dict:
    return {"sent": sent, "errors": errors, "p95_ms": percentile_ms(hist, 0.95),
            "error_pct": 100.0 * errors / sent if sent else float("nan")}


def score_run(run: str, routes: list[str], levels: list[int] | None = None, primary: str | None = None) -> dict:
    """Critical-route p95 and error rate from `loadgen.hist.jsonl`.

    With `levels`, `by_level` splits the same windows by the offered rate of the
    `primary` route in that window, rounded to the nearest level.
    """
    want = set(routes)
    windows = []
    with open(os.path.join(run, "loadgen.hist.jsonl")) as handle:
        for line in handle:
            if line.strip():
                windows.append(json.loads(line))
    level_at: dict[int, int] = {}
    if levels and primary:
        for window in windows:
            if window.get("route") != primary:
                continue
            span = (window["t1_ns"] - window["t0_ns"]) / 1e9
            if span > 0:
                rate = int(window.get("sent") or 0) / span
                level_at[window["t0_ns"]] = min(levels, key=lambda level: abs(level - rate))
    total = [0, 0, {}]
    parts: dict[int, list] = {}
    for window in windows:
        if window.get("route") not in want:
            continue
        rows = [total]
        if window.get("t0_ns") in level_at:
            rows.append(parts.setdefault(level_at[window["t0_ns"]], [0, 0, {}]))
        for row in rows:
            row[0] += int(window.get("sent") or 0)
            row[1] += int(window.get("errors") or 0)
            for bound, count in window.get("hist") or []:
                row[2][int(bound)] = row[2].get(int(bound), 0) + int(count)
    out = _summary(*total)
    if parts:
        out["by_level"] = {str(level): _summary(*parts[level]) for level in sorted(parts)}
    return out


def better(score: dict, best: dict) -> bool:
    """True when `score` has a lower error rate, or the same error and a lower p95."""
    if score["error_pct"] != best["error_pct"]:
        return score["error_pct"] < best["error_pct"]
    return score["p95_ms"] < best["p95_ms"]


def meets(score: dict, max_p95_ms: float, max_error_pct: float) -> bool:
    return score["p95_ms"] <= max_p95_ms and score["error_pct"] <= max_error_pct


def mix_probs(teacher: list, student: list, share: float = TEACHER_SHARE) -> tuple[list, torch.Tensor]:
    """All rows and their draw probabilities: `share` to the teacher, the rest to the student.

    Teacher rows are drawn by `tool_sampling`. Student rows are drawn evenly
    across (tool, avoid) groups, so a thousand scale calls do not drown a few grows.
    """
    rows = list(teacher) + list(student)
    if not student:
        return rows, tool_sampling(rows)
    if not teacher:
        share = 0.0
    groups: dict[tuple, int] = {}
    for _scene, target in student:
        key = (target["tool"], bool(target.get("avoid")))
        groups[key] = groups.get(key, 0) + 1
    student_p = torch.tensor([1.0 / (len(groups) * groups[(target["tool"], bool(target.get("avoid")))])
                              for _scene, target in student], dtype=torch.float64)
    parts = [student_p * (1.0 - share)]
    if teacher:
        parts.insert(0, tool_sampling(teacher) * share)
    probs = torch.cat(parts)
    return rows, probs / probs.sum()


def outcome_reward(outcome: dict, slo_ms: float) -> float | None:
    """`step_reward` of one check's `outcome`. None when the latency is not known yet."""
    error = outcome.get("error_rate")
    if error is None:
        return None
    p95 = outcome.get("p95_ms") or 0.0
    value, _terms = step_reward(
        done=1.0 - float(error), sent=1.0, p95_ms=float(p95), p95_slo_ms=slo_ms, errors=float(error),
        busy=float(outcome.get("busy") or 0.0), cores=float(outcome.get("cores") or 0.0),
        pool_max=float(outcome.get("max") or 0.0), action_cost=0.0,
    )
    return value


def state_outcome(state: dict) -> dict:
    latency = state.get("latency") or {}
    now = latency.get("critical") or latency.get("now") or {}
    pool = state.get("pool") or {}
    return {"p95_ms": now.get("p95_ms"), "error_rate": now.get("error_rate"),
            "cores": pool.get("cores"), "max": pool.get("max"), "busy": pool.get("busy")}


def reward_series(records: list[dict], slo_ms: float) -> list[tuple[float, float]]:
    """(wall time, reward) from every check `outcome` and every logged state."""
    series = []
    for record in records:
        if record.get("event") == "check" and record.get("outcome"):
            outcome = record["outcome"]
        elif record.get("event") == "state" and record.get("state"):
            outcome = state_outcome(record["state"])
        else:
            continue
        stamp = record.get("t")
        value = outcome_reward(outcome, slo_ms)
        if isinstance(stamp, (int, float)) and value is not None:
            series.append((float(stamp), value))
    series.sort()
    return series


def advantage(series: list[tuple[float, float]], at: float, horizon: float = HORIZON_S,
              delay: float = DELAY_S) -> float | None:
    """Mean reward in (at + delay, at + delay + horizon] minus the mean in [at - horizon, at]."""
    before = [value for stamp, value in series if at - horizon <= stamp <= at]
    after = [value for stamp, value in series if at + delay < stamp <= at + delay + horizon]
    if not before or not after:
        return None
    return sum(after) / len(after) - sum(before) / len(before)


def phase_series(records: list[dict]) -> list[tuple[float, object]]:
    """(wall time, load phase) from the check outcomes that carry `phase` (llmctl `Latency.phase`)."""
    series = []
    for record in records:
        outcome = record.get("outcome") if record.get("event") == "check" else None
        if outcome and outcome.get("phase") is not None and isinstance(record.get("t"), (int, float)):
            series.append((float(record["t"]), outcome["phase"]))
    series.sort(key=lambda item: item[0])
    return series


def phase_at(series: list[tuple[float, object]], at: float) -> tuple:
    """(phase, ramp): the newest phase at `at`, and whether it began less than `RAMP_S` before."""
    phase, start = None, None
    for stamp, value in series:
        if stamp > at:
            break
        if value != phase:
            phase, start = value, stamp
    if phase is None:
        return (None, False)
    return (phase, at - start < RAMP_S)


def phase_baseline(gains: list[float], buckets: list[tuple]) -> list[float]:
    """Each gain's baseline: the mean gain of its bucket, or of all gains when the bucket is small."""
    overall = sum(gains) / len(gains)
    sums: dict[tuple, list[float]] = {}
    for gain, bucket in zip(gains, buckets):
        sums.setdefault(bucket, []).append(gain)
    means = {bucket: sum(values) / len(values) for bucket, values in sums.items() if len(values) >= MIN_BUCKET}
    return [means.get(bucket, overall) for bucket in buckets]


def decision_rows(records: list[dict], run: str, slo_ms: float) -> tuple[list, dict]:
    """Clone rows (better than the phase baseline) and avoid rows (worse, or refused) from one student run."""
    series = reward_series(records, slo_ms)
    phases = phase_series(records)
    previous: dict[str, tuple] = {}
    accepted = []
    avoid = []
    for episode in group_episodes(records, run):
        key = str(episode.get("run") or "")
        scene = Scene(episode.get("state") or {}, previous.get(key))
        previous[key] = (scene.ids, scene.obs)
        for call in episode.get("calls") or []:
            stamp = call.get("t")
            if not isinstance(stamp, (int, float)):
                continue
            if not call.get("ok", True):
                target = targets(scene, {**call, "ok": True})
                if target is not None:
                    avoid.append((scene, {**target, "avoid": True}))
                continue
            gain = advantage(series, float(stamp))
            target = targets(scene, call)
            if gain is None or target is None:
                continue
            accepted.append((scene, target, gain, phase_at(phases, float(stamp))))
    rows = []
    stats = {"accepted": len(accepted), "refused": len(avoid), "imitate": 0, "worse": 0,
             "advantage_mean": None, "advantage_std": None, "by_tool": {}}
    if accepted:
        from actions import TOOLS
        gains = [gain for _scene, _target, gain, _bucket in accepted]
        base = phase_baseline(gains, [bucket for *_rest, bucket in accepted])
        relative = [gain - baseline for gain, baseline in zip(gains, base)]
        std = math.sqrt(sum(value ** 2 for value in relative) / len(relative)) or 1.0
        for (scene, target, _gain, _bucket), value in zip(accepted, relative):
            z = value / std
            name = TOOLS[target["tool"]]
            tally = stats["by_tool"].setdefault(name, {"n": 0, "imitate": 0, "worse": 0, "z_sum": 0.0})
            tally["n"] += 1
            tally["z_sum"] += z
            if abs(z) < DEADZONE:
                continue
            if z > 0:
                rows.append((scene, {**target, "weight": min(z, MAX_WEIGHT)}))
                stats["imitate"] += 1
                tally["imitate"] += 1
            else:
                rows.append((scene, {**target, "avoid": True, "weight": min(-z, MAX_WEIGHT)}))
                stats["worse"] += 1
                tally["worse"] += 1
        for tally in stats["by_tool"].values():
            tally["z_mean"] = round(tally.pop("z_sum") / tally["n"], 3)
        stats.update(advantage_mean=sum(gains) / len(gains), advantage_std=std)
    return rows + avoid, stats


class _Rounds(list):
    """The round records, also appended to a JSON-lines file when one is named."""

    def __init__(self, path: str | None):
        super().__init__()
        self.path = path

    def append(self, record: dict) -> None:
        super().append(record)
        if self.path:
            with open(self.path, "a") as handle:
                handle.write(json.dumps(record) + "\n")


def cycle_loop(model, setting: str, teacher_rows: list, rounds: int, collect, steps: int,
               max_p95_ms: float, max_error_pct: float, seed: int = 0, out: str | None = None,
               batch_size: int = 16, keep: int = 3, teacher_share: float = TEACHER_SHARE) -> list[dict]:
    """`collect(checkpoint, round) -> (rows, score, stats)`. Returns one record per round.

    With `out`, each record is appended to `out/cycle.jsonl` as the round ends.
    `teacher_share` 0 trains on the student's own decisions only.
    """
    history = _Rounds(None if out is None else os.path.join(out, "cycle.jsonl"))
    if rounds < 1:
        raise ValueError("rounds must be positive")
    if teacher_share > 0 and not teacher_rows:
        raise ValueError("a teacher share needs teacher rows")
    if teacher_share <= 0:
        teacher_rows = []
    recent: list[list] = []
    best_score = None
    best_state = None
    for rnd in range(1, rounds + 1):
        path = None if out is None else os.path.join(out, f"round-{rnd}.pt")
        if path:
            os.makedirs(out, exist_ok=True)
            save_action_model(model, path, setting)
        rows, score, stats = collect(path, rnd)
        best = best_score is None or better(score, best_score)
        if best:
            best_score = dict(score)
            best_state = {key: value.detach().clone() for key, value in model.state_dict().items()}
            if path:
                save_action_model(model, os.path.join(out, "best.pt"), setting)
        done = meets(score, max_p95_ms, max_error_pct)
        record = {"round": rnd, "score": score, "best": best, "stopped": done, **stats}
        if done:
            history.append(record)
            if path:
                save_action_model(model, os.path.join(out, "winner.pt"), setting)
            return history
        if rows:
            recent = (recent + [rows])[-keep:]
        student = [row for part in recent for row in part]
        model.load_state_dict(best_state)
        if not teacher_rows and not student:
            record.update(student_rows=0, trained_from="best", skipped="no rows")
            history.append(record)
            continue
        mixed, probs = mix_probs(teacher_rows, student, teacher_share)
        model, losses = fit_action(
            setting, [], steps, out=None, seed=seed + rnd, batch_size=batch_size, model=model,
            rows=mixed, probs=probs,
        )
        tail = losses[-max(1, len(losses) // 5):]
        record.update(loss_first=losses[0], loss_last=sum(tail) / len(tail), student_rows=len(student),
                      trained_from="best")
        history.append(record)
    return history


def teacher_runs(root: str) -> list[str]:
    """Luna repeat-1 run directories under `root`."""
    found = []
    for dirpath, _dirs, files in os.walk(root):
        if "llmctl.jsonl" not in files:
            continue
        parts = os.path.relpath(dirpath, root).split(os.sep)
        if len(parts) >= 3 and "luna" in parts[1] and parts[2] == "r1":
            found.append(dirpath)
    return sorted(found)


CRITICAL = {"hotel": ["hotels", "reservation"], "social": ["compose-post", "read-home-timeline"],
            "media": ["read-page", "compose-review"]}
PRIMARY = {"hotel": "hotels", "social": "read-home-timeline", "media": "compose-review"}


def mercury_env(checkpoint: str, base: dict | None = None) -> dict:
    """The compare_systems.sh environment of one round: provider rl at the teacher's cadence and gate."""
    if "@" in checkpoint:
        raise ValueError(f"checkpoint path must not contain @: {checkpoint}")
    env = dict(os.environ if base is None else base)
    env["SYSTEMS"] = "llm40-cycle"
    env["LLM_MODELS"] = f"cycle={os.path.abspath(checkpoint)}@rl"
    env.setdefault("LLMCTL_PYTHON", "/home/paul/opt/torchenv/bin/python")
    env["SKIP_SNAPSHOT"] = "1"
    env.setdefault("INTERFERE", "0")
    env.setdefault("DB", "seed")
    env.setdefault("POOL_START", "8")
    env.setdefault("EVALSET_EVERY", "0.5")
    env.setdefault("EVALSET_WINDOW_MS", "1000")
    if "--gate" not in env.get("LLM_ARGS", "").split():
        env["LLM_ARGS"] = (env.get("LLM_ARGS", "") + " --gate").strip()
    return env


def run_mercury(checkpoint: str, round_dir: str, app: str, slo_ms: float):
    """One `compare_systems.sh` round. Returns `(rows, score, stats)`."""
    env = mercury_env(checkpoint)
    repo = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
    script = os.path.join(repo, "src", "test", "compare_systems.sh")
    subprocess.run([script, round_dir, app], env=env, check=True)
    done = os.path.join(round_dir, "llm40-cycle", "r1", "DONE")
    if not os.path.isfile(done):
        raise RuntimeError(f"no DONE after {round_dir}")
    with open(done) as handle:
        run = handle.read().strip()
    routes = [item for item in env.get("CRITICAL", "").split(",") if item] or CRITICAL[app]
    levels = [int(item) for item in env.get("LEVELS", "").split(",") if item.strip().isdigit()]
    score = score_run(run, routes, levels or None, PRIMARY[app])
    with open(os.path.join(run, "llmctl.jsonl")) as handle:
        records = [json.loads(line) for line in handle if line.strip()]
    rows, stats = decision_rows(records, run, slo_ms)
    return rows, score, stats


def main() -> int:
    import argparse
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--init", required=True,
                        help="action checkpoint to run first, or scratch-mlp / scratch-gnn / scratch-tf for random weights")
    parser.add_argument("--teacher", default="", help="directory of Luna repeat-1 runs (unused with --teacher-share 0)")
    parser.add_argument("--teacher-share", type=float, default=TEACHER_SHARE,
                        help="share of each update drawn from teacher rows; 0 learns from Mercury only")
    parser.add_argument("--out", required=True)
    parser.add_argument("--app", default="hotel", choices=("hotel", "social", "media"))
    parser.add_argument("--rounds", type=int, default=8)
    parser.add_argument("--steps", type=int, default=200)
    parser.add_argument("--keep", type=int, default=3, help="student rounds kept for training")
    parser.add_argument("--max-p95-ms", type=float, default=100.0)
    parser.add_argument("--max-error-pct", type=float, default=1.0)
    parser.add_argument("--seed", type=int, default=0)
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)
    scratch = {"scratch-mlp": "mlp", "scratch-gnn": "gnn", "scratch-tf": "transformer"}
    if args.init in scratch:
        torch.manual_seed(args.seed)
        model = build_action_model(scratch[args.init])
        setting = {"mlp": "mlp-action", "gnn": "gnn-action", "transformer": "tf-action"}[model.actor_type]
    else:
        blob = torch.load(args.init, map_location="cpu", weights_only=False)
        if blob.get("kind") != "action":
            raise SystemExit(f"{args.init} is not an action checkpoint")
        model = load_action_model(args.init, blob)
        setting = blob.get("setting") or f"{model.actor_type}-action"
    rows = []
    if args.teacher_share > 0:
        if not args.teacher:
            raise SystemExit("--teacher is needed when --teacher-share is above 0")
        rows = examples_from(episodes_from([], teacher_runs(args.teacher)))
        if not rows:
            raise SystemExit(f"no teacher labels under {args.teacher}")
    def collect(checkpoint: str, rnd: int):
        result = run_mercury(checkpoint, os.path.join(args.out, f"mercury-{rnd}"), args.app, args.max_p95_ms)
        score = result[1]
        print(f"round {rnd} p95 {score['p95_ms']:.1f} ms error {score['error_pct']:.2f}%", flush=True)
        return result

    history = cycle_loop(
        model, setting, rows, args.rounds, collect, args.steps,
        args.max_p95_ms, args.max_error_pct, args.seed, args.out, keep=args.keep,
        teacher_share=args.teacher_share,
    )
    return 0 if history[-1]["stopped"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
