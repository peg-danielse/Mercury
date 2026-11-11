"""Teacher demonstrations from the existing LLM controller log.

A demonstration is read back from the `episode`, `state`, and `tool` records
`llmctl.py` writes. `reconstruct.from_log` groups the same records; this module
keeps the tool timestamp as well. The live learned controller is `--llm rl`.

The student action is one continuous value per process, `cpus / CPU_SCALE`
clipped to [-1, 1], and 0 for hold. `scale_daemon` and `hold` are the calls
that map into that vector. Every other tool is stored, including its arguments,
and marked `student_valid` false so behavior cloning does not pretend the
decentralized actor took a pool or replica action.
"""

from __future__ import annotations

import json
import math
from dataclasses import dataclass, field

import torch

FEAT_DIM = 10
GLOBAL_DIM = 8
CPU_SCALE = 4.0
DEADBAND = 0.5 / CPU_SCALE

DISCRETE_TOOLS = (
    "hold",
    "scale_daemon",
    "grow_pool",
    "shrink_pool",
    "swap_domain",
    "move_daemon",
    "add_replica",
    "remove_replica",
    "shed_route",
    "restore_route",
)
STUDENT_TOOLS = ("hold", "scale_daemon")


def workload_of(path: str) -> str:
    for part in str(path).replace("\\", "/").split("/"):
        if part == "hotel" or part.startswith("hotel-"):
            return "hotel"
        if part == "social" or part.startswith("social-"):
            return "social"
        if part == "media" or part.startswith("media-"):
            return "media"
    return "other"


def _eligible(ident: str) -> bool:
    return not ident.endswith("loadgen") and not ident.endswith("jaeger")


def _f(value, default=0.0) -> float:
    if isinstance(value, bool) or value is None:
        return default
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


def processes(state: dict) -> list[dict]:
    """Running daemons the student represents, in daemon_id order."""
    rows = []
    for daemon in state.get("daemons") or []:
        ident = str(daemon.get("daemon_id") or "")
        if ident and _eligible(ident):
            rows.append(daemon)
    rows.sort(key=lambda daemon: daemon.get("daemon_id") or "")
    return rows


def local_features(daemon: dict) -> list[float]:
    """One process. Nothing in this vector is read from another daemon."""
    cpus = daemon.get("cpus") or []
    cores = float(len(cpus)) if isinstance(cpus, list) else _f(daemon.get("cores"))
    return [
        _f(daemon.get("cpu_use")),
        _f(daemon.get("occupancy")),
        _f(daemon.get("pressure") if daemon.get("pressure") is not None else daemon.get("runnable")),
        cores / 16.0,
        1.0 if daemon.get("in_pool", True) else 0.0,
        1.0 if daemon.get("replica") else 0.0,
        1.0 if daemon.get("throttled") else 0.0,
        _f(daemon.get("cpu_wait")),
        _f(daemon.get("ipc")) / 3.0,
        math.log1p(max(_f(daemon.get("llc_per_ms")), 0.0)) / 8.0,
    ]


def global_features(state: dict) -> list[float]:
    pool = state.get("pool") or {}
    now = ((state.get("latency") or {}).get("now") or {})
    return [
        _f(pool.get("cores")) / 64.0,
        _f(pool.get("max")) / 64.0,
        _f(pool.get("busy")),
        _f(pool.get("pressure")),
        _f(pool.get("allocated")) / 100.0,
        math.log1p(max(_f(now.get("p95_ms")), 0.0)) / 8.0,
        _f(now.get("error_rate")),
        math.log1p(max(_f(now.get("req_per_s")), 0.0)) / 8.0,
    ]


def topology_of(daemons: list[dict]) -> torch.Tensor:
    """Row-normalized shift. An edge means the two cpusets touch the same L3 domain."""
    nodes = len(daemons)
    shift = torch.zeros(nodes, nodes)
    domains = [set(daemon.get("l3") or []) for daemon in daemons]
    for i in range(nodes):
        for j in range(nodes):
            if i != j and domains[i] & domains[j]:
                shift[i, j] = 1.0
        total = float(shift[i].sum())
        if total > 0:
            shift[i] /= total
    return shift


def observe(state: dict, previous: tuple[list[str], torch.Tensor] | None = None):
    """Local rows, history, topology, mask, and the global vector.

    `previous` is `(ids, obs)` from the prior decision of the same run. A
    process that was absent then gets a zero history row.
    """
    daemons = processes(state)
    ids = [str(daemon["daemon_id"]) for daemon in daemons]
    if daemons:
        obs = torch.tensor([local_features(daemon) for daemon in daemons], dtype=torch.float32)
    else:
        obs = torch.zeros(0, FEAT_DIM)
    history = torch.zeros(1, len(ids), FEAT_DIM)
    if previous is not None:
        prev_ids, prev_obs = previous
        locate = {ident: index for index, ident in enumerate(prev_ids)}
        for index, ident in enumerate(ids):
            if ident in locate:
                history[0, index] = prev_obs[locate[ident]]
    mask = torch.ones(len(ids), dtype=torch.bool)
    return ids, obs, history, topology_of(daemons), mask, torch.tensor(global_features(state), dtype=torch.float32)


def discrete_index(name: str) -> int:
    try:
        return DISCRETE_TOOLS.index(name)
    except ValueError:
        return -1


def encode_call(ids: list[str], call: dict | None) -> dict:
    """Map one logged tool call onto the student vector and the discrete label."""
    nodes = len(ids)
    continuous = torch.zeros(nodes, 1)
    if call is None or call.get("name") in (None, "", "none", "hold"):
        return {
            "tool_name": "hold",
            "tool_input": {} if call is None else dict(call.get("input") or {}),
            "discrete": discrete_index("hold"),
            "continuous": continuous,
            "daemon_id": None,
            "cpus": 0,
            "student_valid": True,
            "clipped": False,
            "ok": True if call is None else bool(call.get("ok", True)),
        }
    name = str(call.get("name"))
    raw = dict(call.get("input") or {})
    ok = bool(call.get("ok", True))
    if name != "scale_daemon":
        return {
            "tool_name": name,
            "tool_input": raw,
            "discrete": discrete_index(name),
            "continuous": continuous,
            "daemon_id": raw.get("daemon_id"),
            "cpus": None,
            "student_valid": False,
            "clipped": False,
            "ok": ok,
        }
    daemon_id = str(raw.get("daemon_id") or "")
    try:
        cpus = int(raw.get("cpus"))
    except (TypeError, ValueError):
        cpus = None
    if daemon_id not in ids or cpus is None or cpus == 0:
        return {
            "tool_name": name,
            "tool_input": raw,
            "discrete": discrete_index(name),
            "continuous": continuous,
            "daemon_id": daemon_id or None,
            "cpus": cpus,
            "student_valid": False,
            "clipped": False,
            "ok": ok,
        }
    clipped = abs(cpus) > CPU_SCALE
    continuous[ids.index(daemon_id), 0] = max(-1.0, min(1.0, cpus / CPU_SCALE))
    return {
        "tool_name": name,
        "tool_input": raw,
        "discrete": discrete_index(name),
        "continuous": continuous,
        "daemon_id": daemon_id,
        "cpus": cpus,
        "student_valid": True,
        "clipped": clipped,
        "ok": ok,
    }


def decode_action(action: torch.Tensor, ids: list[str], mask: torch.Tensor | None = None) -> tuple[str, str | None, int]:
    """Invert `encode_call` for hold and scale steps inside ±CPU_SCALE."""
    values = action.detach().reshape(-1).to(dtype=torch.float32)
    present = mask.detach().reshape(-1).to(dtype=torch.bool) if mask is not None else torch.ones(values.shape[0], dtype=torch.bool)
    scored = values.abs()
    scored = torch.where(present, scored, torch.full_like(scored, -1.0))
    if values.numel() == 0 or float(scored.max()) < DEADBAND:
        return "hold", None, 0
    index = int(scored.argmax())
    cpus = int(round(float(values[index]) * CPU_SCALE))
    if cpus == 0:
        return "hold", None, 0
    return "scale_daemon", ids[index], cpus


@dataclass
class Demonstration:
    run: str
    workload: str
    t_s: float
    tool_t: float | None
    trigger: str | None
    tool_name: str
    tool_input: dict
    tools: list
    ok: bool
    student_valid: bool
    clipped: bool
    discrete: int
    daemon_id: str | None
    cpus: int | None
    ids: list
    obs: torch.Tensor
    history: torch.Tensor
    topology: torch.Tensor
    mask: torch.Tensor
    global_state: torch.Tensor
    continuous: torch.Tensor
    applied: torch.Tensor | None
    reward: float | None
    extra: dict = field(default_factory=dict)


def demonstration_from_episode(episode: dict, workload: str | None = None,
                               previous: tuple[list[str], torch.Tensor] | None = None) -> tuple[Demonstration, tuple[list[str], torch.Tensor]]:
    state = episode.get("state") or {}
    ids, obs, history, topology, mask, global_state = observe(state, previous)
    calls = list(episode.get("calls") or [])
    first = calls[0] if calls else None
    mapped = encode_call(ids, first)
    # A mapped call that landed is the student vector. A mapped call that was
    # refused landed nothing, so the applied vector is zeros. A tool with no
    # per-process core delta has no applied vector.
    if mapped["student_valid"] and mapped["ok"]:
        applied = mapped["continuous"].clone()
    elif mapped["student_valid"]:
        applied = torch.zeros_like(mapped["continuous"])
    else:
        applied = None
    reward = episode.get("reward")
    reward = None if reward is None else float(reward)
    tool_t = None if first is None else first.get("t")
    tool_t = float(tool_t) if isinstance(tool_t, (int, float)) else None
    demo = Demonstration(
        run=str(episode.get("run") or ""),
        workload=workload or workload_of(episode.get("run") or ""),
        t_s=_f(episode.get("t_s")),
        tool_t=tool_t,
        trigger=episode.get("trigger"),
        tool_name=mapped["tool_name"],
        tool_input=mapped["tool_input"],
        tools=calls,
        ok=mapped["ok"],
        student_valid=mapped["student_valid"],
        clipped=mapped["clipped"],
        discrete=mapped["discrete"],
        daemon_id=mapped["daemon_id"],
        cpus=mapped["cpus"],
        ids=ids,
        obs=obs,
        history=history,
        topology=topology,
        mask=mask,
        global_state=global_state,
        continuous=mapped["continuous"],
        applied=applied,
        reward=reward,
    )
    return demo, (ids, obs)


def group_episodes(records: list[dict], run: str) -> list[dict]:
    """The `llmctl.jsonl` grouping `reconstruct.from_log` uses, plus each tool's timestamp."""
    episodes: list[dict] = []
    current: dict | None = None
    origin = next((record.get("t") for record in records if isinstance(record.get("t"), (int, float))), None)
    for record in records:
        kind = record.get("event")
        if kind == "episode":
            if current and current.get("state"):
                episodes.append(current)
            stamp = record.get("t")
            current = {
                "run": run,
                "t_s": round(stamp - origin, 2) if isinstance(stamp, (int, float)) and origin is not None else 0.0,
                "trigger": record.get("trigger"),
                "calls": [],
            }
        elif kind == "state" and current is not None:
            current["state"] = record.get("state") or {}
        elif kind == "tool" and current is not None and record.get("name") != "inspect":
            current["calls"].append({
                "name": record.get("name"),
                "input": dict(record.get("input") or {}),
                "ok": bool(record.get("ok")),
                "t": record.get("t"),
            })
    if current and current.get("state"):
        episodes.append(current)
    return episodes


def demonstrations_from_episodes(episodes: list[dict], workload: str | None = None) -> list[Demonstration]:
    previous: dict[str, tuple[list[str], torch.Tensor]] = {}
    demos = []
    for episode in episodes:
        run = str(episode.get("run") or "")
        demo, carried = demonstration_from_episode(episode, workload=workload, previous=previous.get(run))
        previous[run] = carried
        demos.append(demo)
    return demos


def parse_llmctl_jsonl(path: str, workload: str | None = None) -> list[Demonstration]:
    with open(path) as handle:
        records = [json.loads(line) for line in handle if line.strip()]
    return demonstrations_from_episodes(group_episodes(records, path), workload=workload or workload_of(path))


def demonstrations_from_run(run: str, workload: str | None = None) -> list[Demonstration]:
    """Episodes via `reconstruct.from_log`, then the same local features and action map."""
    import os
    import sys
    learn = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "distill")
    if learn not in sys.path:
        sys.path.insert(0, learn)
    from reconstruct import from_log  # noqa: E402

    return demonstrations_from_episodes(from_log(run), workload=workload or workload_of(run))
