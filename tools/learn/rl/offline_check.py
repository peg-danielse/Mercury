#!/usr/bin/env python3
"""Pair consecutive Luna episodes and see whether one accepted action moves the next state.

The next episode's logged state is the outcome. With --timing, a pair is kept when the
accepted tool (or the turn, for hold and idle) is 1–10 s before that next state. The episode
gap includes the model call, so it is not the outcome horizon. Episodes with more than one
accepted call are left out: those calls share one outcome. hold, and an episode with no
accepted call, are the noise.
"""

from __future__ import annotations

import argparse
import json
import os


def accepted(calls: list | None) -> list:
    return [c for c in (calls or []) if c.get("ok", True) and c.get("name") != "inspect"]


def label(episode: dict) -> str:
    calls = accepted(episode.get("calls"))
    if not calls:
        return "idle"
    if len(calls) > 1:
        return "multi"
    name = calls[0]["name"]
    if name == "scale_daemon":
        cpus = (calls[0].get("input") or {}).get("cpus")
        try:
            cpus = int(cpus)
        except (TypeError, ValueError):
            return "scale_other"
        if cpus > 0:
            return "scale_up"
        if cpus < 0:
            return "scale_down"
        return "scale_other"
    if name == "hold":
        return "hold"
    return name


def app_of(run: str) -> str:
    for part in run.split("/"):
        if part == "hotel" or part.startswith("hotel-"):
            return "hotel"
        if part == "social" or "social" in part:
            return "social"
        if part == "media" or part.startswith("media-"):
            return "media"
    return "other"


def num(value):
    if isinstance(value, bool) or value is None:
        return None
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def cpu_count(daemon: dict | None) -> float | None:
    if not daemon:
        return None
    cpus = daemon.get("cpus")
    if isinstance(cpus, list):
        return float(len(cpus))
    return num(daemon.get("cores"))


def measure(episode: dict) -> dict:
    state = episode.get("state") or {}
    now = ((state.get("latency") or {}).get("now") or {})
    pool = state.get("pool") or {}
    daemons = {d.get("daemon_id"): d for d in state.get("daemons") or []}
    return {
        "p95": num(now.get("p95_ms")),
        "req": num(now.get("req_per_s")),
        "err": num(now.get("error_rate")),
        "cores": num(pool.get("cores")),
        "busy": num(pool.get("busy")),
        "daemons": daemons,
    }


def percentile(values: list[float], p: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    index = (len(ordered) - 1) * p
    lo = int(index)
    hi = min(lo + 1, len(ordered) - 1)
    frac = index - lo
    return ordered[lo] * (1 - frac) + ordered[hi] * frac


def fmt(value, digits=2) -> str:
    if value is None:
        return "—"
    return f"{value:.{digits}f}"


def summarize(values: list[float]) -> str:
    if not values:
        return "n=0"
    med = percentile(values, 0.5)
    return (f"n={len(values)} med {fmt(med)}  p10 {fmt(percentile(values, 0.1))}  "
            f"p90 {fmt(percentile(values, 0.9))}")


def mad(values: list[float]) -> float | None:
    if not values:
        return None
    med = percentile(values, 0.5)
    return percentile([abs(v - med) for v in values], 0.5)


def write_timing(root: str, output: str) -> int:
    """One line per episode, in from_log order: seconds from the accepted tool to the next state.

    The episode gap includes the model call. post_s does not. An episode with no accepted tool uses the
    turn time, so hold and idle still have an outcome clock.
    """
    runs = []
    for dirpath, _, files in os.walk(root):
        if "llmctl.jsonl" not in files:
            continue
        path = os.path.join(dirpath, "llmctl.jsonl")
        with open(path) as handle:
            for line in handle:
                if '"event": "turn"' not in line and '"event":"turn"' not in line:
                    continue
                record = json.loads(line)
                if record.get("event") == "turn":
                    if record.get("model") == "openai/gpt-6-luna":
                        runs.append(os.path.dirname(path))
                    break
    written = 0
    with open(output, "w") as out:
        for run in sorted(runs):
            records = [json.loads(line) for line in open(os.path.join(run, "llmctl.jsonl"))]
            t0 = next((r.get("t") for r in records if isinstance(r.get("t"), (int, float))), None)
            episodes = []
            current = None
            for record in records:
                kind = record.get("event")
                if kind == "episode":
                    if current and current.get("state_t") is not None:
                        episodes.append(current)
                    current = {"t": record.get("t"), "state_t": None, "turn_t": None, "tools": []}
                elif kind == "state" and current is not None and current["state_t"] is None:
                    current["state_t"] = record.get("t")
                elif kind == "turn" and current is not None and current["turn_t"] is None:
                    current["turn_t"] = record.get("t")
                elif kind == "tool" and current is not None and record.get("name") != "inspect":
                    current["tools"].append(record)
            if current and current.get("state_t") is not None:
                episodes.append(current)
            for index, episode in enumerate(episodes):
                post_s = None
                if index + 1 < len(episodes):
                    acted = [tool for tool in episode["tools"] if tool.get("ok", True)]
                    anchor = (acted[-1]["t"] if acted else episode["turn_t"] or episode["t"])
                    nxt = episodes[index + 1]["state_t"]
                    if isinstance(anchor, (int, float)) and isinstance(nxt, (int, float)):
                        post_s = round(nxt - anchor, 3)
                t_s = round(episode["t"] - t0, 2) if isinstance(episode["t"], (int, float)) and t0 is not None else 0.0
                out.write(json.dumps({"run": run, "t_s": t_s, "post_s": post_s}) + "\n")
                written += 1
    print(f"wrote {output}: {written} episodes from {len(runs)} luna logs", flush=True)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("episodes", nargs="?")
    parser.add_argument("--timing", help="jsonl from write_timing: post-tool seconds to the next state")
    parser.add_argument("--gap-min", type=float, default=1.0)
    parser.add_argument("--gap-max", type=float, default=10.0)
    parser.add_argument("--write-timing")
    parser.add_argument("--log-root", default="data/dsb/compare")
    args = parser.parse_args()
    if args.write_timing:
        return write_timing(args.log_root, args.write_timing)
    if not args.episodes:
        parser.error("episodes jsonl is required")

    by_run: dict[str, list[dict]] = {}
    with open(args.episodes) as handle:
        for line in handle:
            episode = json.loads(line)
            by_run.setdefault(episode["run"], []).append(episode)
    for episodes in by_run.values():
        episodes.sort(key=lambda e: e.get("t_s") or 0)

    timing: dict[tuple[str, float], float] = {}
    if args.timing:
        with open(args.timing) as handle:
            for line in handle:
                record = json.loads(line)
                if record.get("post_s") is not None:
                    timing[(record["run"], record["t_s"])] = record["post_s"]

    rows = []
    skipped = {"multi": 0, "gap": 0, "edge": 0, "no_timing": 0}
    for run, episodes in by_run.items():
        for index, episode in enumerate(episodes[:-1]):
            kind = label(episode)
            if kind == "multi":
                skipped["multi"] += 1
                continue
            nxt = episodes[index + 1]
            if timing:
                gap = timing.get((run, episode.get("t_s")))
                if gap is None:
                    skipped["no_timing"] += 1
                    continue
            else:
                gap = (nxt.get("t_s") or 0) - (episode.get("t_s") or 0)
            if gap < args.gap_min or gap > args.gap_max:
                skipped["gap"] += 1
                continue
            before, after = measure(episode), measure(nxt)
            if before["p95"] is None or after["p95"] is None:
                skipped["edge"] += 1
                continue
            before_p95 = before["p95"]
            call = (accepted(episode.get("calls")) or [{}])[0]
            daemon_id = (call.get("input") or {}).get("daemon_id")
            requested = (call.get("input") or {}).get("cpus")
            try:
                requested = int(requested)
            except (TypeError, ValueError):
                requested = None
            cpu_before = cpu_count(before["daemons"].get(daemon_id))
            cpu_after = cpu_count(after["daemons"].get(daemon_id))
            cpu_delta = None if cpu_before is None or cpu_after is None else cpu_after - cpu_before
            rows.append({
                "kind": kind,
                "app": app_of(run),
                "gap": gap,
                "dp95": after["p95"] - before["p95"],
                "dreq": (after["req"] - before["req"]) if before["req"] is not None and after["req"] is not None else None,
                "derr": (after["err"] - before["err"]) if before["err"] is not None and after["err"] is not None else None,
                "dcores": (after["cores"] - before["cores"]) if before["cores"] is not None and after["cores"] is not None else None,
                "dbusy": (after["busy"] - before["busy"]) if before["busy"] is not None and after["busy"] is not None else None,
                "dcpu": cpu_delta,
                "requested": requested,
                "p95_before": before_p95,
            })

    clock = "post-tool" if timing else "episode-gap"
    print(f"pairs with one accepted call, {clock} {args.gap_min:g}–{args.gap_max:g}s: {len(rows)}")
    print(f"left out: multi-call {skipped['multi']}, outside window {skipped['gap']}, "
          f"no timing {skipped['no_timing']}, missing p95 {skipped['edge']}")
    counts: dict[str, int] = {}
    for row in rows:
        counts[row["kind"]] = counts.get(row["kind"], 0) + 1
    print("kinds:", dict(sorted(counts.items(), key=lambda kv: -kv[1])))
    print("outcome gap s:", summarize([r["gap"] for r in rows]))
    print()

    # hold is the explicit do-nothing decision. idle is no accepted call. Noise is both.
    noise_kinds = {"hold", "idle"}
    noise = [r for r in rows if r["kind"] in noise_kinds]
    metrics = ("dp95", "dreq", "derr", "dcores", "dbusy")
    print("noise (hold + idle)")
    for metric in metrics:
        values = [r[metric] for r in noise if r[metric] is not None]
        print(f"  {metric:7} {summarize(values)}  mad {fmt(mad(values))}")
    print()

    order = ["hold", "idle", "scale_up", "scale_down", "grow_pool", "shrink_pool", "swap_domain", "move_daemon"]
    present = [k for k in order if any(r["kind"] == k for r in rows)]
    present += sorted({r["kind"] for r in rows} - set(present))

    def values(kind: str, metric: str) -> list[float]:
        return [r[metric] for r in rows if r["kind"] == kind and r[metric] is not None]

    print(f"{'action':14} {'n':>5}  {'p95 ms':>14} {'req/s':>14} {'error':>10} {'pool cores':>12} {'vs noise p95':>12}")
    for kind in present:
        group = [r for r in rows if r["kind"] == kind]
        p95 = values(kind, "dp95")
        req = values(kind, "dreq")
        err = values(kind, "derr")
        cores = values(kind, "dcores")
        noise_p95 = [r["dp95"] for r in noise]
        shift = None
        if p95 and noise_p95 and mad(noise_p95):
            shift = (percentile(p95, 0.5) - percentile(noise_p95, 0.5)) / mad(noise_p95)
        elif p95 and noise_p95:
            shift = None
        flag = "—" if shift is None else f"{shift:+.2f} mad"
        print(f"{kind:14} {len(group):5}  {fmt(percentile(p95, 0.5)):>8} med  "
              f"{fmt(percentile(req, 0.5)):>8} med  {fmt(percentile(err, 0.5), 3):>8}  "
              f"{fmt(percentile(cores, 0.5)):>8} med  {flag:>12}")

    print()
    print("scale: did the named daemon's CPU count change by the request?")
    for kind in ("scale_up", "scale_down"):
        group = [r for r in rows if r["kind"] == kind and r["dcpu"] is not None and r["requested"] is not None]
        if not group:
            print(f"  {kind}: no daemon match")
            continue
        exact = sum(1 for r in group if r["dcpu"] == r["requested"])
        sign = sum(1 for r in group if r["dcpu"] * r["requested"] > 0)
        zero = sum(1 for r in group if r["dcpu"] == 0)
        print(f"  {kind}: n={len(group)} exact {exact} ({exact / len(group):.0%})  "
              f"same sign {sign} ({sign / len(group):.0%})  unchanged {zero} ({zero / len(group):.0%})  "
              f"med requested {fmt(percentile([r['requested'] for r in group], 0.5), 0)}  "
              f"med delta {fmt(percentile([r['dcpu'] for r in group], 0.5), 0)}")

    print()
    print("by application, median p95 delta and median req/s delta")
    print(f"{'app':8} {'action':14} {'n':>5} {'p95':>8} {'req/s':>8} {'cores':>8} {'cpu':>8}")
    for app in ("hotel", "social", "media"):
        for kind in ("hold", "idle", "scale_up", "scale_down", "grow_pool"):
            group = [r for r in rows if r["app"] == app and r["kind"] == kind]
            if not group:
                continue
            def med(metric, group=group):
                vals = [r[metric] for r in group if r[metric] is not None]
                return percentile(vals, 0.5)
            print(f"{app:8} {kind:14} {len(group):5} {fmt(med('dp95')):>8} {fmt(med('dreq')):>8} "
                  f"{fmt(med('dcores')):>8} {fmt(med('dcpu'), 0):>8}")

    print()
    print("absolute movement, median |delta|. This is the noise floor, not a directional effect.")
    print(f"{'action':14} {'|p95|':>8} {'|req/s|':>8} {'|error|':>8}")
    for kind in present:
        def amed(metric, kind=kind):
            return percentile([abs(v) for v in values(kind, metric)], 0.5)
        print(f"{kind:14} {fmt(amed('dp95')):>8} {fmt(amed('dreq')):>8} {fmt(amed('derr'), 3):>8}")

    print()
    print("scale_up by the p95 it acted on. A useful horizon moves the slow seconds down.")
    print(f"{'before p95':14} {'n':>5} {'d p95':>8} {'d req/s':>8}")
    for lo, hi, title in ((0, 50, "<50 ms"), (50, 200, "50–200 ms"), (200, 1e12, ">=200 ms")):
        group = [r for r in rows if r["kind"] == "scale_up" and lo <= r["p95_before"] < hi]
        base = [r for r in noise if lo <= r["p95_before"] < hi]
        if not group:
            continue
        gp = percentile([r["dp95"] for r in group], 0.5)
        bp = percentile([r["dp95"] for r in base], 0.5) if base else None
        gr = percentile([r["dreq"] for r in group if r["dreq"] is not None], 0.5)
        print(f"{title:14} {len(group):5} {fmt(gp):>8} {fmt(gr):>8}   idle/hold n={len(base)} med dp95 {fmt(bp)}")

    print()
    print("grow_pool is the placement check: pool.cores should rise. scale should move the daemon, not the pool.")
    for kind in ("grow_pool", "shrink_pool", "hold", "scale_up"):
        cores = values(kind, "dcores")
        if not cores:
            continue
        rose = sum(1 for v in cores if v > 0)
        print(f"  {kind}: pool cores {summarize(cores)}  rose {rose}/{len(cores)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
