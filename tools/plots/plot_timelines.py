#!/usr/bin/env python3
"""Every repeat of a comparison as its own timeline, on one shared logarithmic p95 scale.

  plot_timelines.py OUT [OUT ...] -d figs/
  plot_timelines.py runs.json[:system=newname,...] -d figs/

This is the grid in data/dsb/compare/social-cycle-gemini-20261028/figs/timelines-all.png: one column per
system, one row per repeat, antagonist-on windows shaded, and the repeat's critical p95 and error rate in the
corner. A directory is read with extract_runs.py (a run with DONE). A JSON file is that script's output.
The suffix :system=newname renames systems in that input, so two fixed pools can share a figure:

  plot_timelines.py social-cycle-20261028 social-cycle-20261028-p60:saturate=fixed60 -d figs/

The distilled-transformer repeats of the same social cycle:

  plot_timelines.py data/dsb/compare/social-cycle-distill-20261029 -d data/dsb/compare/social-cycle-distill-20261029/figs

Needs matplotlib (on this machine, /home/paul/opt/plotenv/bin/python3). Failed requests are excluded from the
percentile. --critical defaults to the social routes compose-post,read-home-timeline.
"""

from __future__ import annotations

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import extract_runs  # noqa: E402

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
from ieee_style import FULL, apply as apply_ieee  # noqa: E402

# Column order for the social-cycle grid. Anything else follows, sorted.
ORDER = ["fixed8", "saturate", "fixed40", "fixed60", "shift", "gnn-shift", "l3", "gnn-l3", "l3shift", "gnn-l3shift",
         "degrade", "gnn-degrade", "llm25-gemini", "llm25-distill", "llm25-luna", "llm40-luna", "llm40-gemini38"]
NAMES = {
    "saturate": "Fixed",
    "fixed8": "Fixed 8",
    "fixed40": "Fixed 40",
    "fixed60": "Fixed 60",
    "shift": "Shift",
    "degrade": "Degrade",
    "l3": "L3",
    "l3shift": "L3 + shift",
    "gnn-shift": "FIRM Shift",
    "gnn-l3": "FIRM L3",
    "gnn-l3shift": "FIRM L3+shift",
    "gnn-degrade": "FIRM Degrade",
    "llm25-gemini": "Gemini",
    "llm25-distill": "Distill",
    "llm25-luna": "ZeroCTL",
    "llm40-luna": "ZeroCTL",
    "llm40-gemini38": "Gemini 3.8",
}
INK, MUTED, BAND = "#1f2937", "#78716c", "#94a3b8"


def pct(hist: dict, q: float) -> float:
    """q-quantile in ms of a {upper bound ns: count} histogram. Empty is NaN."""
    items = sorted((int(b), c) for b, c in (hist or {}).items())
    total = sum(c for _, c in items)
    if not total:
        return float("nan")
    acc = 0
    for bound, count in items:
        acc += count
        if acc >= q * total:
            return bound / 1e6
    return items[-1][0] / 1e6


def merged(parts: list[dict]) -> dict:
    out = {"sent": 0, "errors": 0, "hist": {}}
    for p in parts:
        out["sent"] += p.get("sent", 0)
        out["errors"] += p.get("errors", 0)
        for b, c in (p.get("hist") or {}).items():
            out["hist"][b] = out["hist"].get(b, 0) + c
    return out


def split_spec(spec: str) -> tuple[str, dict[str, str]]:
    path, _, renames = spec.partition(":")
    mapping = dict(r.split("=", 1) for r in renames.split(",") if "=" in r)
    return path, mapping


def runs_from(spec: str, bin_s: float) -> list[dict]:
    path, mapping = split_spec(spec)
    if os.path.isdir(path):
        runs = []
        for system in sorted(os.listdir(path)):
            base = os.path.join(path, system)
            if not os.path.isdir(base):
                continue
            for rep in sorted(os.listdir(base)):
                done = os.path.join(base, rep, "DONE")
                if not os.path.isfile(done):
                    continue
                run = open(done).read().strip()
                print(f"{system} {rep}: {run}", file=sys.stderr)
                runs.append({"system": system, "repeat": rep, **extract_runs.extract(run, bin_s)})
    else:
        runs = [run for comp in json.load(open(path))["comparisons"] for run in comp["runs"]]
    for run in runs:
        run["system"] = mapping.get(run["system"], run["system"])
    return runs


def systems_of(runs: list[dict]) -> list[str]:
    seen = {r["system"] for r in runs}
    return [s for s in ORDER if s in seen] + sorted(seen - set(ORDER))


def display(system: str, systems: list[str]) -> str:
    """saturate is the fixed pool of 8 when a second fixed pool is in the same figure."""
    if system == "saturate" and ({"fixed40", "fixed60"} & set(systems)):
        return "Fixed 8"
    return NAMES.get(system, system)


def critical_of(run: dict, routes: list[str]) -> tuple[float, float]:
    part = merged([run["routes"][r] for r in routes if r in run["routes"]])
    err = 100 * part["errors"] / part["sent"] if part["sent"] else 0.0
    return pct(part["hist"], 0.95), err


def bins_of(run: dict, routes: list[str]) -> list[tuple[float, float]]:
    acc: dict[float, list] = {}
    for route in routes:
        for b in run["series"].get(route, []):
            acc.setdefault(b["t_s"], []).append(b)
    return [(t, pct(merged(acc[t])["hist"], 0.95)) for t in sorted(acc)]


def plot(runs: list[dict], routes: list[str], out_dir: str) -> None:
    print(f"figure font: {apply_ieee()}", file=sys.stderr)
    by: dict[str, dict[int, dict]] = {}
    for run in runs:
        by.setdefault(run["system"], {})[int(run["repeat"][1:])] = run
    systems = systems_of(runs)
    rows = max(max(reps) for reps in by.values())
    # One inch per system so the column titles fit, and at least the two-column width.
    # Taller panels keep 8 pt type; the page is this size, not a wider drawing scaled down.
    fig, axes = plt.subplots(rows, len(systems), figsize=(max(FULL, 1.05 * len(systems)), 1.15 * rows + 0.7),
                             sharex=True, sharey=True, squeeze=False)
    ys = []
    xmax = 0.0
    for col, system in enumerate(systems):
        for row in range(rows):
            ax = axes[row][col]
            run = by[system].get(row + 1)
            if run is None:
                ax.text(0.5, 0.5, "not run", transform=ax.transAxes, ha="center", va="center", color=MUTED, fontsize=8)
                ax.set_xticks([])
                ax.set_yticks([])
                continue
            for phase in run["phases"]:
                ax.axvspan(phase["on_s"], phase["off_s"], color=BAND, alpha=0.35, lw=0, zorder=0)
            pts = bins_of(run, routes)
            if pts:
                xs, bin_ys = zip(*pts)
                ax.plot(xs, bin_ys, color=INK, lw=1.0, zorder=2)
                ys.extend(y for y in bin_ys if y == y and y > 0)
            p95, err = critical_of(run, routes)
            ax.text(0.02, 0.95, f"r{row + 1}   {p95:.0f} ms   {err:.0f}% err", transform=ax.transAxes,
                    va="top", fontsize=8, color=INK)
            xmax = max(xmax, run.get("duration_s") or 0)
            if row == 0:
                ax.set_title(display(system, systems), fontsize=8)
            if col == 0:
                ax.set_ylabel("p95", fontsize=8)
            if row == rows - 1:
                ax.set_xlabel("time (s)", fontsize=8)
    finite = ys or [1.0]
    for ax in axes.ravel():
        ax.set_yscale("log")
        ax.set_ylim(min(finite) / 1.8, max(finite) * 1.6)
        ax.set_xlim(0, xmax or 1)
    fig.suptitle("Critical p95, every repeat. Gray bands are antagonist on. Shared logarithmic scale.", fontsize=8)
    fig.tight_layout(rect=(0, 0, 1, 0.98))
    os.makedirs(out_dir, exist_ok=True)
    from ieee_style import dress
    dress(fig)
    for ext in ("png", "pdf"):
        path = os.path.join(out_dir, f"timelines-all.{ext}")
        fig.savefig(path)
        print(f"wrote {path}", file=sys.stderr)
    plt.close(fig)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("inputs", nargs="+", help="compare OUT directory, or runs.json[:system=newname,...]")
    ap.add_argument("-d", "--dir", required=True, help="directory for timelines-all.png and .pdf")
    ap.add_argument("--critical", default="compose-post,read-home-timeline",
                    help="routes merged into the p95 (default: the social critical routes)")
    ap.add_argument("--bin-s", type=float, default=5.0, help="bin width when reading an OUT directory")
    a = ap.parse_args()
    runs = [run for spec in a.inputs for run in runs_from(spec, a.bin_s)]
    if not runs:
        print("no runs with DONE", file=sys.stderr)
        return 1
    plot(runs, a.critical.split(","), a.dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
