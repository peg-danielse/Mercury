#!/usr/bin/env python3
"""Rebuild, for every episode of an llm run, the machine state the model decided on and the calls it made: the
training data for a small local model that imitates it (train.py).

  reconstruct.py RUN [RUN ...] -o episodes.jsonl [--window-s 1]

RUN is a dsb_ab.sh run directory of an llm system (llmctl.jsonl, mercury.log, rings/, loadgen.hist.jsonl). Sources:
  rings/placement.jsonl   every cpuset change (t_ns, daemon, cpus): the services' placement at any time
  rings/<daemon>.mring    per service every 10 ms: task-clock and pressure (ns), cores
  rings/cpu-N.mring       per CPU every 50 ms: busy and run_delay (ns), cycles, instructions, llc_misses
  mercury.log             the pool: "pool fixed ... max= cpus=" at start, then every pool step with its cpus and t_ns
  loadgen.hist.jsonl      end-to-end latency, summarized as llmctl's Latency does (time slices)
  llmctl.jsonl            episodes (wall clock), their tool calls and results; a "state" record when llmctl logged
                          the exact state (from 2026-10-23), which is then used as it is
llmctl's wall clock maps to Mercury's CLOCK_MONOTONIC through the pool steps both record (the llm's grow, shrink and
swap calls), else through the sidecar's start. The topology is dtn-01's: 64 CPUs, L3 domain i = CPUs 2i, 2i+1,
2i+32, 2i+33, CPU 0 Mercury's. One output line per episode: run, system, t_s, budget, the state as entity lists
(domains, services, global), the previous episode's calls and this episode's calls.
"""

from __future__ import annotations

import argparse
import bisect
import json
import os
import re
import statistics
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import llmctl  # noqa: E402  (Latency: the same latency summary the model saw)

NCPU, NDOM = 64, 16


def domain_cpus(i: int) -> list[int]:
    return [c for c in (2 * i, 2 * i + 1, 2 * i + 32, 2 * i + 33) if c != 0]


DOMAIN_OF = {c: i for i in range(NDOM) for c in domain_cpus(i)}


def cpu_list(spec: str) -> list[int]:
    out: list[int] = []
    for part in spec.split(","):
        if "-" in part:
            a, b = part.split("-")
            out += range(int(a), int(b) + 1)
        elif part:
            out.append(int(part))
    return out


def read_mring(path: str) -> tuple[np.ndarray, dict[str, np.ndarray]]:
    """Mercury's ring archive: header, counter names, then records of [t_ns, 3 words, counters...] (uint64)."""
    data = open(path, "rb").read()
    _, count, _, record = struct.unpack_from("<4I", data, 8)
    committed, offset = struct.unpack_from("<2Q", data, 24)
    names, at = [], 40
    for _ in range(count):
        (size,) = struct.unpack_from("<I", data, at)
        names.append(data[at + 4:at + 4 + size].decode())
        at += 4 + size
    committed = min(committed, (len(data) - offset) // record)
    rows = np.frombuffer(data, dtype=np.uint64, count=committed * record // 8, offset=offset).reshape(committed, record // 8)
    return rows[:, 0].astype(np.int64), {n: rows[:, 4 + i].astype(np.float64) for i, n in enumerate(names)}


class Rings:
    """Window sums over a ring: sum(col over samples with t in (t0, t1]) and the time covered."""

    def __init__(self, path: str):
        self.t, self.cols = read_mring(path)
        self.cum = {k: np.concatenate([[0.0], np.cumsum(v)]) for k, v in self.cols.items()}

    def window(self, t0: int, t1: int) -> tuple[dict[str, float], float, int]:
        a, b = np.searchsorted(self.t, t0, "right"), np.searchsorted(self.t, t1, "right")
        if b <= a:
            return {}, 0.0, b
        span = (self.t[b - 1] - self.t[a - 1]) / 1e9 if a > 0 else (self.t[b - 1] - self.t[a]) / 1e9
        return {k: c[b] - c[a] for k, c in self.cum.items()}, max(span, 1e-3), b


def pool_timeline(log: str) -> tuple[list[tuple[int, set[int], str]], dict[str, int]]:
    """(t_ns, pool CPUs, kind) for the pool's start and every step; and the pool's start and max. Lines:
    "pool fixed start=8 max=25 up=... cpus=2-5,34-37 t_ns=" (start), "pool l3 llm cores=12 cpus=... t_ns=" (grow),
    "pool l3 shrink llm cores=.. cpus=... t_ns=", "pool swap llm swapped ..., pool 2-7,... t_ns=" (swap)."""
    steps, meta = [], {}
    for line in log.splitlines():
        if not line.startswith("pool ") or "t_ns=" not in line:
            continue
        t = int(re.search(r"t_ns=(\d+)", line).group(1))
        m = re.search(r"\bcpus=([0-9][0-9,-]*)", line)
        if not m and ", pool " in line:
            m = re.match(r"([0-9][0-9,-]*)", line.rsplit(", pool ", 1)[1])
        if not m:
            continue
        if " up=" in line:
            kind = "start"
            meta["start"] = int(re.search(r"start=(\d+)", line).group(1))
            meta["max"] = int(m2.group(1)) if (m2 := re.search(r"\bmax=(\d+)", line)) else 0
        else:
            kind = "swap" if line.startswith("pool swap") else ("shrink" if " shrink " in line else "grow")
        steps.append((t, set(cpu_list(m.group(1))), kind))
    return sorted(steps, key=lambda s: s[0]), meta


def placement(path: str) -> dict[str, list[tuple[int, list[int]]]]:
    out: dict[str, list[tuple[int, list[int]]]] = {}
    for line in open(path):
        d = json.loads(line)
        out.setdefault(d["daemon"], []).append((d["t_ns"], d["cpus"]))
    return out


def at(series: list[tuple[int, object]], t: int):
    ts = [s[0] for s in series]
    i = bisect.bisect_right(ts, t) - 1
    return series[i][1] if i >= 0 else None


def wall_to_mono(run: str, tools: list[dict], steps: list[tuple[int, set[int], str]]) -> tuple[float, str]:
    """Offset (s) with mono = wall - offset. The llm's ok pool calls and Mercury's llm pool steps happen in the same
    order and kind; the call's record (wall) is written just after the step (mono). Pair them by order within each
    kind; else the sidecar's start against llmctl's first record."""
    diffs = []
    for kind, tool in (("grow", "grow_pool"), ("shrink", "shrink_pool"), ("swap", "swap_domain")):
        calls = [c["t"] for c in tools if c.get("name") == tool and c.get("ok")]
        marks = [t / 1e9 for t, _, k in steps if k == kind]
        if calls and len(calls) == len(marks):
            diffs += [c - m for c, m in zip(calls, marks)]
    if diffs:
        off = statistics.median(diffs)
        spread = max(abs(d - off) for d in diffs)
        if spread < 2.0:
            return off, f"pool steps (n={len(diffs)}, max spread {spread * 1000:.0f} ms)"
    start = next(json.loads(l) for l in open(os.path.join(run, "system.jsonl")) if '"start"' in l)["t_ns"] / 1e9
    first = json.loads(open(os.path.join(run, "llmctl.jsonl")).readline())["t"]
    return first - start - 0.5, "sidecar start"


def state_at(T: int, window_s: float, rings_d: dict[str, Rings], rings_c: dict[int, Rings],
             place: dict[str, list], pool: set[int], meta: dict[str, int], lat: dict | None, shed: list[str]) -> dict:
    t0 = T - int(window_s * 1e9)
    cpu = {}
    for c, r in rings_c.items():
        s, span, _ = r.window(t0, T)
        if not s:
            cpu[c] = {"busy": 0.0, "wait": 0.0, "ipc": 0.0, "llc_per_ms": 0.0}
            continue
        cpu[c] = {"busy": min(1.0, s["busy"] / 1e9 / span), "wait": s["run_delay"] / 1e9 / span,
                  "ipc": s["instructions"] / s["cycles"] if s["cycles"] else 0.0,
                  "llc_per_ms": s["llc_misses"] / (span * 1000)}
    daemons = []
    on: dict[int, list[str]] = {}
    for d, r in rings_d.items():
        cpus = at(place.get(d, []), T) or []
        s, span, _ = r.window(t0, T)
        use = s.get("task-clock", 0.0) / 1e9 / span if s else 0.0
        pressure = s.get("pressure", 0.0) / 1e9 / span if s else 0.0
        daemons.append({"daemon_id": d, "cpus": cpus, "cores": len(cpus), "cpu_use": round(use, 3),
                        "occupancy": round(use / len(cpus), 3) if cpus else 0.0, "pressure": round(min(pressure, 1.0), 3),
                        "in_pool": bool(cpus) and all(c in pool for c in cpus), "loadgen": d.endswith("loadgen")})
        for c in cpus:
            on.setdefault(c, []).append(d)
    domains = []
    for i in range(NDOM):
        cs = domain_cpus(i)
        names = sorted({d for c in cs for d in on.get(c, [])})
        own = sum(dm["cpu_use"] * sum(c in cs for c in dm["cpus"]) / len(dm["cpus"])
                  for dm in daemons if dm["cpus"] and dm["daemon_id"] in names)
        busy = sum(cpu[c]["busy"] for c in cs)
        cyc_ipc = [cpu[c]["ipc"] for c in cs if cpu[c]["busy"] > 0.05]
        domains.append({"name": f"l3.{i}", "in_pool": all(c in pool for c in cs) or any(c in pool for c in cs),
                        "load": round(busy / len(cs), 3), "cpus": cs, "free": [c for c in cs if not on.get(c)],
                        "daemons": names, "foreign_cpus": round(max(0.0, busy - own), 3),
                        "ipc": round(statistics.mean(cyc_ipc), 3) if cyc_ipc else 0.0,
                        "llc_per_ms": round(sum(cpu[c]["llc_per_ms"] for c in cs), 1),
                        "cpu_wait": round(statistics.mean(cpu[c]["wait"] for c in cs), 3)})
    in_pool = [dm for dm in daemons if dm["in_pool"] and not dm["loadgen"]]
    pool_cpus = sorted(pool)
    pool_state = {"start": meta.get("start", 8), "max": meta.get("max", 0), "cores": len(pool_cpus), "cpus": pool_cpus,
                  "busy": round(statistics.mean(cpu[c]["busy"] for c in pool_cpus), 3) if pool_cpus else 0.0,
                  "pressure": round(statistics.mean(dm["pressure"] for dm in in_pool), 3) if in_pool else 0.0,
                  "allocated": sum(dm["cores"] for dm in in_pool),
                  "free_domains": [d["name"] for d in domains if not d["in_pool"] and not d["daemons"]]}
    out = {"pool": pool_state, "l3": domains, "daemons": daemons}
    if lat:
        out["latency"] = lat
    if shed is not None:
        out["degrade"] = {"shed": list(shed)}
    return out


def reconstruct(run: str, window_s: float) -> list[dict]:
    recs = [json.loads(l) for l in open(os.path.join(run, "llmctl.jsonl"))]
    log = open(os.path.join(run, "mercury.log"), errors="replace").read()
    steps, meta = pool_timeline(log)
    tools = [r for r in recs if r.get("event") == "tool"]
    off, how = wall_to_mono(run, tools, steps)
    rd = os.path.join(run, "rings")
    rings_d = {f[:-6]: Rings(os.path.join(rd, f)) for f in os.listdir(rd)
               if f.endswith(".mring") and not f.startswith("cpu-")}
    rings_c = {int(f[4:-6]): Rings(os.path.join(rd, f)) for f in os.listdir(rd) if f.startswith("cpu-")}
    place = placement(os.path.join(rd, "placement.jsonl"))
    hist = [json.loads(l) for l in open(os.path.join(run, "loadgen.hist.jsonl"))]
    hist.sort(key=lambda w: w["t1_ns"])
    hist_t = [w["t1_ns"] for w in hist]
    sysmeta = next((json.loads(l) for l in open(os.path.join(run, "system.jsonl")) if '"start"' in l), {})
    shedding = bool(sysmeta.get("llm_shed"))
    critical = (sysmeta.get("llm_shed") or [None] * 6)[5]
    critical = critical.split(",") if critical else None
    episodes, cur, prev_calls, shed = [], None, [], []
    for r in recs + [{"event": "end"}]:
        if r.get("event") in ("episode", "end"):
            if cur:
                episodes.append(cur)
                prev_calls = cur["calls"]
                for c in cur["calls"]:
                    if c["ok"] and c["name"] == "shed_route":
                        shed.append(c["input"].get("route"))
                    if c["ok"] and c["name"] == "restore_route" and c["input"].get("route") in shed:
                        shed.remove(c["input"].get("route"))
            if r.get("event") == "end":
                break
            T = int((r["t"] - off) * 1e9)
            pool = next((p for t, p, _ in reversed(steps) if t <= T), steps[0][1] if steps else set())
            lat_obj = llmctl.Latency(None, critical if shedding else None)
            for w in hist[:bisect.bisect_right(hist_t, T)][-7 * 240:]:
                lat_obj.slices.setdefault(w["t0_ns"], []).append(w)
            lat_obj.path = "-"
            lat_obj._read = lambda: None
            cur = {"run": run, "t_s": round((T - hist[0]["t0_ns"]) / 1e9, 2) if hist else None, "trigger": r.get("trigger"),
                   "budget": meta.get("max", 0), "shedding": shedding, "align": how,
                   "state": state_at(T, window_s, rings_d, rings_c, place, pool, meta, lat_obj.summary(),
                                     list(shed) if shedding else None),
                   "previous": prev_calls, "calls": []}
        elif r.get("event") == "state" and cur is not None:
            cur["state"] = r["state"]  # the exact state, when llmctl logged it
            cur["exact"] = True
        elif r.get("event") == "tool" and cur is not None and r.get("name") != "inspect":
            cur["calls"].append({"name": r["name"], "input": {k: v for k, v in (r.get("input") or {}).items() if k != "reason"},
                                 "ok": bool(r.get("ok"))})
    return episodes


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("runs", nargs="+")
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--window-s", type=float, default=1.0)
    a = ap.parse_args()
    n = 0
    with open(a.output, "w") as out:
        for run in a.runs:
            try:
                eps = reconstruct(run, a.window_s)
            except Exception as error:  # one broken run must not stop the rest
                print(f"skip {run}: {error!r}", file=sys.stderr)
                continue
            for e in eps:
                out.write(json.dumps(e, separators=(",", ":")) + "\n")
            n += len(eps)
            print(f"{run}: {len(eps)} episodes ({eps[0]['align'] if eps else '-'})", file=sys.stderr)
    print(f"wrote {a.output}: {n} episodes", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
