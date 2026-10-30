#!/usr/bin/env python3
"""Reduce compare_systems.sh results to one compact JSON for the paper figures (plot_runs.py).

  extract_runs.py OUT [OUT ...] -o runs.json [--bin-s 5]

Every OUT is a compare_systems.sh directory (OUT/<system>/r<n>/DONE names the run). Per run the JSON holds, with all
times in seconds from the run's first load window (the load generator's, the antagonist driver's and Mercury's
timestamps share CLOCK_MONOTONIC):
  routes    per route: requests sent, errors, and latency histograms ({upper bound ns: count}) for the whole run and
            for the windows inside and outside antagonist phases
  phases    the antagonist phases: index, kind, level, victim, on/off times, and per route the histogram and
            counts of the windows inside the phase
  series    per route, bin_s-second bins: start time, sent, errors, histogram
  pool      [time, CPUs] steps of the pool's size (from Mercury's timestamped pool lines); swaps and degrade steps
  cpu       core-seconds allocated to and used by the application's daemons (alloc_vs_use.txt), load generator excluded
  llm_*     the llm controller's episodes and the episodes its model API refused (llmctl.out "... API error")
Stdlib only: it runs on the experiment host, which has no matplotlib.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys


def merge(into: dict, hist) -> None:
    for bound, count in hist or []:
        into[bound] = into.get(bound, 0) + count


def extract(run: str, bin_s: float) -> dict:
    windows = [json.loads(l) for l in open(os.path.join(run, "loadgen.hist.jsonl"))]
    t0 = min(w["t0_ns"] for w in windows)
    sec = lambda ns: (ns - t0) / 1e9
    phases, on = [], None
    for line in open(os.path.join(run, "antagonists.jsonl")):
        e = json.loads(line)
        if e.get("event") == "on":
            on = e
        elif e.get("event") == "off" and on is not None:
            phases.append({"phase": on["phase"], "kind": on["kind"], "level": on["level"], "victim": on["victim"],
                           "on_s": sec(on["t_ns"]), "off_s": sec(e["t_ns"]), "routes": {}})
            on = None
    routes, series = {}, {}
    for w in windows:
        route, mid = w["route"], (w["t0_ns"] + w["t1_ns"]) / 2
        r = routes.setdefault(route, {"sent": 0, "errors": 0, "hist": {}, "on": {"sent": 0, "errors": 0, "hist": {}},
                                      "off": {"sent": 0, "errors": 0, "hist": {}}})
        r["sent"] += w["sent"]; r["errors"] += w["errors"]; merge(r["hist"], w.get("hist"))
        phase = next((p for p in phases if p["on_s"] <= sec(mid) < p["off_s"]), None)
        part = r["on" if phase else "off"]
        part["sent"] += w["sent"]; part["errors"] += w["errors"]; merge(part["hist"], w.get("hist"))
        if phase:
            pr = phase["routes"].setdefault(route, {"sent": 0, "errors": 0, "hist": {}})
            pr["sent"] += w["sent"]; pr["errors"] += w["errors"]; merge(pr["hist"], w.get("hist"))
        b = int(sec(w["t0_ns"]) // bin_s)
        s = series.setdefault(route, {}).setdefault(b, {"sent": 0, "errors": 0, "hist": {}})
        s["sent"] += w["sent"]; s["errors"] += w["errors"]; merge(s["hist"], w.get("hist"))
    log = open(os.path.join(run, "mercury.log"), errors="replace").read()
    pool = sorted((sec(int(t)), int(c)) for c, t in
                  re.findall(r"^pool (?:l3|fixed)\b.* cores=(\d+)\b.* t_ns=(\d+)", log, re.M))
    swaps = [sec(int(t)) for t in re.findall(r"^pool swap .* t_ns=(\d+)", log, re.M)]
    degrade = [(sec(int(t)), len([x for x in d.split(",") if x]))
               for d, t in re.findall(r"^degrade daemon_id=\S+ (?:disabled|enabled) \S+ load=\S+ disabled=\[([^\]]*)\] t_ns=(\d+)",
                                      log, re.M)]
    alloc = used = 0.0
    window = None
    path = os.path.join(run, "alloc_vs_use.txt")
    if os.path.exists(path):
        for line in open(path):
            if line.startswith("window "):  # "window 674.6 s, 16 daemons": the span alloc and used cover
                window = float(line.split()[1])
            p = line.split()
            if len(p) > 3 and re.match(r"^(hotel|social|media)-", p[0]) and not p[0].endswith("loadgen"):
                try:
                    alloc += float(p[1]); used += float(p[2])
                except ValueError:
                    pass
    episodes = api_errors = 0
    ctl = os.path.join(run, "llmctl.jsonl")
    if os.path.exists(ctl):
        episodes = sum(1 for l in open(ctl) if '"event": "episode"' in l)
    out = os.path.join(run, "llmctl.out")
    if os.path.exists(out):  # episodes the model API refused (429: quota): the llm system did nothing in them
        api_errors = sum(1 for l in open(out, errors="replace") if "API error" in l and "episode stopped" in l)
    ser = {route: [{"t_s": b * bin_s, **v} for b, v in sorted(bins.items())] for route, bins in series.items()}
    return {"path": run, "duration_s": sec(max(w["t1_ns"] for w in windows)), "routes": routes, "phases": phases,
            "series": ser, "pool": pool, "swaps": swaps, "degrade": degrade,
            "cpu": {"alloc_core_s": alloc, "used_core_s": used, "window_s": window or sec(max(w["t1_ns"] for w in windows))},
            "llm_episodes": episodes,
            "llm_api_errors": api_errors}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("out", nargs="+")
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--bin-s", type=float, default=5.0)
    ap.add_argument("--map", default=None, metavar="FROM=TO",
                    help="rewrite the run paths in DONE files (a copy made elsewhere: FROM=the experiment host's OUT parent)")
    a = ap.parse_args()
    result = {"bin_s": a.bin_s, "comparisons": []}
    for out in a.out:
        meta = open(os.path.join(out, "meta.txt")).read().strip().splitlines()
        comp = {"dir": out, "meta": next((l for l in reversed(meta) if l.startswith("started")), meta[0]), "runs": []}
        for system in sorted(os.listdir(out)):
            base = os.path.join(out, system)
            if not os.path.isdir(base):
                continue
            for rep in sorted(os.listdir(base)):
                done = os.path.join(base, rep, "DONE")
                if os.path.isfile(done):
                    run = open(done).read().strip()
                    if a.map:
                        src, dst = a.map.split("=", 1)
                        run = run.replace(src, dst, 1)
                    print(f"{system} {rep}: {run}", file=sys.stderr)
                    comp["runs"].append({"system": system, "repeat": rep, **extract(run, a.bin_s)})
        result["comparisons"].append(comp)
    with open(a.output, "w") as f:
        json.dump(result, f, separators=(",", ":"))
    print(f"wrote {a.output}: {sum(len(c['runs']) for c in result['comparisons'])} runs", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
