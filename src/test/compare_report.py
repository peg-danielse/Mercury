#!/usr/bin/env python3
"""Summarize a compare_systems.sh run: one row per system with end-to-end latency (all windows, and the
windows inside and outside antagonist ON phases), errors, throughput, CPU cost of the application's daemons,
and how many placement changes the system and the autoscaler made. With repeats (OUT/<system>/r<n>), each
number is the median over the repeats and the n column says how many; a second table gives the p95 and p99
of every repeat, so a system's spread is visible next to its median.

  compare_report.py OUT

Latency comes from the load generator's 500 ms histograms (loadgen.hist.jsonl: [upper bound ns, count]),
counted from one OFF period before the first ON phase. CPU comes from alloc_vs_use.txt (core-seconds over the
run, the load generator left out). Placement changes are counted from mercury.log.
"""

from __future__ import annotations

import json
import os
import re
import sys


def percentile(hist: dict[int, int], q: float) -> float | None:
    total = sum(hist.values())
    if total == 0:
        return None
    need, seen = q * total, 0
    for bound in sorted(hist):
        seen += hist[bound]
        if seen >= need:
            return bound / 1e6
    return None


def phases(run: str) -> tuple[list[tuple[int, int]], int | None, list[dict]]:
    """ON intervals (t_ns), the measurement start (one OFF period before the first ON), and per phase its
    index, kind, level, victim and interval."""
    on, intervals, start, off_s, info = None, [], None, 20.0, []
    for line in open(os.path.join(run, "antagonists.jsonl")):
        event = json.loads(line)
        if event.get("event") == "start":
            off_s = float(event.get("args", {}).get("off", off_s))
        elif event.get("event") == "on":
            on = event
            if start is None:
                start = event["t_ns"] - int(off_s * 1e9)
        elif event.get("event") == "off" and on is not None:
            intervals.append((on["t_ns"], event["t_ns"]))
            info.append({"phase": on["phase"], "kind": on["kind"], "level": on["level"],
                         "victim": on["victim"], "span": (on["t_ns"], event["t_ns"])})
            on = None
    return intervals, start, info


def gnn_resizes(log: str) -> int:
    """Cpuset size changes by the gnn/gru autoscaler: its log lines whose cores differ from the daemon's last line."""
    last: dict[str, str] = {}
    changes = 0
    for daemon, cores in re.findall(r"^gnn daemon_id=(\S+) cores=(\d+)", log, re.M):
        if daemon in last and last[daemon] != cores:
            changes += 1
        last[daemon] = cores
    return changes


def summarize(name: str, run: str) -> dict:
    intervals, start, info = phases(run)
    per_phase: dict[int, dict[int, int]] = {p["phase"]: {} for p in info}
    critical = [r for r in os.environ.get("CRITICAL", "").split(",") if r]
    # Per route group (critical / optional), inside and outside antagonist phases: histograms and counts.
    groups = {g: {"on": {}, "off": {}, "sent": 0, "errors": 0} for g in ("critical", "optional")}
    all_h: dict[int, int] = {}
    on_h: dict[int, int] = {}
    off_h: dict[int, int] = {}
    sent = done = errors = 0
    t_first = t_last = None
    for line in open(os.path.join(run, "loadgen.hist.jsonl")):
        w = json.loads(line)
        if start is not None and w["t0_ns"] < start:
            continue
        mid = (w["t0_ns"] + w["t1_ns"]) // 2
        inside = any(a <= mid < b for a, b in intervals)
        if critical:
            g = groups["critical" if w.get("route") in critical else "optional"]
            target_h = g["on" if inside else "off"]
            for bound, count in w.get("hist", []):
                target_h[bound] = target_h.get(bound, 0) + count
            g["sent"] += w.get("sent", 0)
            g["errors"] += w.get("errors", 0)
        for p in info:
            if critical and w.get("route") not in critical:
                continue
            if p["span"][0] <= mid < p["span"][1]:
                h = per_phase[p["phase"]]
                for bound, count in w.get("hist", []):
                    h[bound] = h.get(bound, 0) + count
        for bound, count in w.get("hist", []):
            all_h[bound] = all_h.get(bound, 0) + count
            target = on_h if inside else off_h
            target[bound] = target.get(bound, 0) + count
        sent += w.get("sent", 0)
        done += w.get("done", 0)
        errors += w.get("errors", 0)
        t_first = w["t0_ns"] if t_first is None else t_first
        t_last = w["t1_ns"]
    alloc = used = 0.0
    path = os.path.join(run, "alloc_vs_use.txt")
    if os.path.exists(path):
        for line in open(path):
            parts = line.split()
            if len(parts) > 3 and parts[0].startswith(("hotel-", "social-")) and not parts[0].endswith("loadgen"):
                try:
                    alloc += float(parts[1])
                    used += float(parts[2])
                except ValueError:
                    pass
    log = open(os.path.join(run, "mercury.log")).read() if os.path.exists(os.path.join(run, "mercury.log")) else ""
    changes = {
        "saturate": len(re.findall(r"^scale daemon_id=\S+ cores=\d+ cpus=", log, re.M)),
        "pool": len(re.findall(r"^pool l3 load=.* cores=", log, re.M)),
        "swap": len(re.findall(r"^pool swap ", log, re.M)),
        "shrink": len(re.findall(r"^pool l3 shrink ", log, re.M)),
        "degrade": len(re.findall(r"^degrade daemon_id=\S+ (?:disabled|enabled) ", log, re.M)),
        "shift": len(re.findall(r"^shift daemon_id=\S+ from=\S+ load=\S+ to=", log, re.M)),
        "throttle": len(re.findall(r"^throttle daemon_id=\S+ system_load=\S+ scope=\S+ cut", log, re.M)),
        "llm": len(re.findall(r"^llm daemon_id=", log, re.M)),
        "gru": gnn_resizes(log),
    }
    pool_cores = [int(c) for c in re.findall(r"^pool (?:l3|fixed)\b.* cores=(\d+)", log, re.M)]
    # Pool core-seconds over the measured windows: the pool's size held from each timestamped pool line to the next.
    steps = sorted((int(t), int(c)) for c, t in re.findall(r"^pool (?:l3|fixed)\b.* cores=(\d+)\b.* t_ns=(\d+)", log, re.M))
    pool_core_s = None
    if steps and t_first and t_last:
        pool_core_s = 0.0
        for i, (t, cores) in enumerate(steps):
            a = max(t, t_first)
            b = min(steps[i + 1][0] if i + 1 < len(steps) else t_last, t_last)
            if b > a:
                pool_core_s += cores * (b - a) / 1e9
        if steps[0][0] > t_first:
            pool_core_s += steps[0][1] * (steps[0][0] - t_first) / 1e9
    episodes, latencies = 0, []
    ctl = os.path.join(run, "llmctl.jsonl")
    if os.path.exists(ctl):
        begun = None
        for line in open(ctl):
            r = json.loads(line)
            if r["event"] == "episode":
                episodes, begun = episodes + 1, r["t"]
            elif r["event"] == "turn" and begun is not None:
                latencies.append(r["t"] - begun)
    seconds = (t_last - t_first) / 1e9 if t_first and t_last else 0
    return {
        "system": name, "p50": percentile(all_h, 0.50), "p95": percentile(all_h, 0.95), "p99": percentile(all_h, 0.99),
        "p95_on": percentile(on_h, 0.95), "p95_off": percentile(off_h, 0.95), "p99_on": percentile(on_h, 0.99),
        "rps": done / seconds if seconds else 0, "err": errors / sent if sent else 0,
        "alloc": alloc, "used": used, "changes": changes, "episodes": episodes,
        "llm_s": sorted(latencies)[len(latencies) // 2] if latencies else None, "phases": len(intervals),
        "per_phase": {p["phase"]: {"label": f"{p['kind']}{p['level']} {p['victim'].split('-', 1)[-1]}",
                                   "p95": percentile(per_phase[p["phase"]], 0.95)} for p in info},
        "pool_max": max(pool_cores) if pool_cores else None,
        "pool_core_s": pool_core_s,
        "groups": {name: {"p95_off": percentile(g["off"], 0.95), "p95_on": percentile(g["on"], 0.95),
                          "p99_on": percentile(g["on"], 0.99),
                          "err": g["errors"] / g["sent"] if g["sent"] else None} for name, g in groups.items()}
        if critical else None,
    }


def paired(row: dict, base: dict) -> str:
    """Phases where row's p95 is at or below base's, of the phases both have, and the geometric mean of the ratio."""
    import math
    pairs = [(row["per_phase"][k]["p95"], base["per_phase"][k]["p95"]) for k in row["per_phase"]
             if k in base["per_phase"] and row["per_phase"][k]["p95"] and base["per_phase"][k]["p95"]]
    if not pairs:
        return "-"
    wins = sum(a <= b for a, b in pairs)
    gmean = math.exp(sum(math.log(a / b) for a, b in pairs) / len(pairs))
    return f"{wins}/{len(pairs)} phases, x{gmean:.2f}"


def fmt(value: float | None, spec: str = ".1f") -> str:
    return "-" if value is None else format(value, spec)


def median(values: list) -> float | None:
    values = sorted(v for v in values if v is not None)
    if not values:
        return None
    mid = len(values) // 2
    return values[mid] if len(values) % 2 else (values[mid - 1] + values[mid]) / 2


def combine(name: str, runs: list[dict]) -> dict:
    """The median of every number over a system's repeats; changes summed into medians per mechanism."""
    row = {"system": name, "n": len(runs), "runs": runs}
    for key in ("p50", "p95", "p99", "p95_on", "p95_off", "p99_on", "rps", "err", "alloc", "used", "llm_s"):
        row[key] = median([r[key] for r in runs])
    row["changes"] = {k: median([r["changes"][k] for r in runs]) for k in runs[0]["changes"]}
    row["episodes"] = median([r["episodes"] for r in runs])
    row["pool_max"] = median([r["pool_max"] for r in runs])
    row["pool_core_s"] = median([r["pool_core_s"] for r in runs])
    row["groups"] = None
    if runs[0]["groups"]:
        row["groups"] = {g: {k: median([r["groups"][g][k] for r in runs]) for k in runs[0]["groups"][g]}
                         for g in runs[0]["groups"]}
    phases_seen = sorted({k for r in runs for k in r["per_phase"]})
    row["per_phase"] = {k: {"label": next(r["per_phase"][k]["label"] for r in runs if k in r["per_phase"]),
                            "p95": median([r["per_phase"][k]["p95"] for r in runs if k in r["per_phase"]])}
                        for k in phases_seen}
    return row


def main() -> int:
    out = sys.argv[1]
    rows = []
    for name in sorted(os.listdir(out)):
        base = os.path.join(out, name)
        if not os.path.isdir(base):
            continue
        dones = [os.path.join(base, "DONE")] + [os.path.join(base, rep, "DONE") for rep in sorted(os.listdir(base))]
        runs = [summarize(name, open(d).read().strip()) for d in dones if os.path.isfile(d)]
        if runs:
            rows.append(combine(name, runs))
    order = ["saturate", "l3", "shift", "l3shift", "throttle", "degrade", "llm"]
    base = lambda s: s.rstrip("0123456789") if s.rstrip("0123456789") in order else s  # llm15, llm25: llm budgets
    rows.sort(key=lambda r: (order.index(base(r["system"])) if base(r["system"]) in order else len(order),
                             int(r["system"][len(base(r["system"])):] or 0)))
    meta = open(os.path.join(out, "meta.txt")).read().strip().splitlines()
    print(next((l for l in reversed(meta) if l.startswith("started")), meta[-1]))
    for line in meta:
        if " pool=" in line and not line.startswith("started"):
            print("  " + line)
    print()
    if any(r["groups"] for r in rows):
        print(f"{'system':<9} {'pool':>4} {'pool core-s':>11} | {'critical p95 off':>16} {'p95 on':>7} {'p99 on':>7} {'err%':>6} | "
              f"{'optional p95 off':>16} {'p95 on':>7} {'err%':>6}")
        for r in rows:
            c, o = r["groups"]["critical"], r["groups"]["optional"]
            pct = lambda v: "-" if v is None else f"{100 * v:.2f}"
            print(f"{r['system']:<9} {fmt(r['pool_max'], '.0f'):>4} {fmt(r['pool_core_s'], '.0f'):>11} | {fmt(c['p95_off']):>16} {fmt(c['p95_on']):>7} "
                  f"{fmt(c['p99_on']):>7} {pct(c['err']):>6} | {fmt(o['p95_off']):>16} {fmt(o['p95_on']):>7} {pct(o['err']):>6}")
        print("  critical = the CRITICAL routes; pool = the largest pool (CPUs) in the run; pool core-s = the pool's size")
        print("  integrated over the measured windows (its cost); off/on = outside/inside antagonist phases")
        print()
    print(f"{'system':<9} {'n':>2} {'p50':>6} {'p95':>6} {'p99':>6} | {'p95 off':>7} {'p95 on':>7} {'p99 on':>7} | "
          f"{'req/s':>7} {'err%':>6} | {'alloc':>7} {'used':>6} {'util':>5} | changes")
    for r in rows:
        ch = r["changes"]
        made = ", ".join(f"{k} {v:g}" for k, v in ch.items() if v)
        if r["episodes"]:
            made += f" (llm episodes {r['episodes']:g}, median call {fmt(r['llm_s'], '.1f')}s)"
        util = r["used"] / r["alloc"] if r["alloc"] else None
        print(f"{r['system']:<9} {r['n']:>2} {fmt(r['p50']):>6} {fmt(r['p95']):>6} {fmt(r['p99']):>6} | {fmt(r['p95_off']):>7} "
              f"{fmt(r['p95_on']):>7} {fmt(r['p99_on']):>7} | {r['rps']:>7.0f} {100 * r['err']:>6.2f} | "
              f"{r['alloc']:>7.0f} {r['used']:>6.0f} {fmt(None if util is None else 100 * util, '.0f'):>4}% | {made}")
    # Paired by phase: the same antagonist (kind, level, victim) at the same time in every system.
    phase_ids = sorted({k for r in rows for k in r["per_phase"]})
    if phase_ids:
        print()
        print("p95 (ms) during each antagonist phase, paired across systems (median over repeats; critical routes only"
              " when CRITICAL is set):")
        print(f"  {'phase':<22}" + "".join(f"{r['system']:>10}" for r in rows))
        for k in phase_ids:
            label = next((r["per_phase"][k]["label"] for r in rows if k in r["per_phase"]), "")
            print(f"  {k:>2} {label:<19}" + "".join(f"{fmt(r['per_phase'].get(k, {}).get('p95')):>10}" for r in rows))
        base = next((r for r in rows if r["system"] == "saturate"), None)
        if base:
            print("  against saturate, phase by phase (at or below its p95 / compared; geometric mean of the p95 ratio):")
            for r in rows:
                if r is not base:
                    print(f"    {r['system']:<9} {paired(r, base)}")
    if any(r["n"] > 1 for r in rows):
        print()
        print("per repeat, p95 / p99 (ms):")
        for r in rows:
            print(f"  {r['system']:<9} " + "  ".join(f"{fmt(x['p95'])}/{fmt(x['p99'])}" for x in r["runs"]))
    print()
    print("medians over n repeats; latency in ms (histogram bucket upper bounds); on/off = windows inside/outside antagonist ON phases;")
    print("alloc/used = core-seconds of the application's daemons (load generator excluded); changes = cpuset or cpu.max")
    print("changes from mercury.log by mechanism (saturate = one CPU added or removed by the autoscaler; gru = a resize")
    print("pool = one L3 domain added to the l3 pool).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
