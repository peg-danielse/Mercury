#!/usr/bin/env python3
"""Two comparisons of the Luna teacher holds.

The holds are data/dsb/compare/luna-rl-20261105, low, mid, and high, for hotel,
social, and media. Each hold was run once with antagonists and once clean.

stressor.pdf is the benchmark's tail under each stressor, in milliseconds.
The first bar of a group is the clean hold, warmup dropped. The others are the
critical-route p95 while that stressor is on. Both intensities of a kind are
pooled. A stressor that matters is a bar taller than the clean hold beside it.

luna.pdf is the effect of adding Luna: the fixed pool's p95 divided by Luna's,
for the clean hold and for each stressor. A bar above 1 is Luna faster than
that pool. Repeats are paired, r1 with r1.

The bar is the median of the two repeats. The line is their min and max.
The run is the directory named in DONE.
"""

from __future__ import annotations

import glob
import json
import os
import statistics
import sys

import matplotlib.pyplot as plt
from matplotlib.patches import Patch
from matplotlib.ticker import NullFormatter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ieee_style import FULL, apply, dress

ROOT = "data/dsb/compare/luna-rl-20261105"
ORANGE = "#D55E00"
BLACK = "#000000"
APPS = ["hotel", "social", "media"]
APP_NAME = {"hotel": "Hotel", "social": "Social", "media": "Media"}
ROUTES = {
    "hotel": ["hotels", "reservation"],
    "social": ["compose-post", "read-home-timeline"],
    "media": ["read-page", "compose-review"],
}
LEVELS = ["low", "mid", "high"]
ARMS = [
    ("luna", "Luna", ORANGE, True),
    ("fixed8", "Fixed 8", BLACK, True),
    ("fixed40", "Fixed 40", BLACK, False),
]
# Both intensities of a kind are one bar. The names stay short enough for one panel.
STRESS = [
    ("same", "core"),
    ("smt", "SMT"),
    ("l3", "L3"),
    ("membw", "bus"),
]
LUNA_GROUPS = [("clean", "none")] + STRESS
BASELINES = [
    ("fixed8", "versus Fixed 8", BLACK, True),
    ("fixed40", "versus Fixed 40", BLACK, False),
]


def pct(hist: dict, q: float = 0.95) -> float:
    items = sorted((int(b), c) for b, c in hist.items())
    total = sum(c for _, c in items)
    if not total:
        return float("nan")
    acc = 0
    for bound, count in items:
        acc += count
        if acc >= q * total:
            return bound / 1e6
    return items[-1][0] / 1e6


def done_run(app: str, sched: str, rep: str) -> str:
    matches = glob.glob(os.path.join(ROOT, app, sched, rep, "**", "DONE"), recursive=True)
    if len(matches) != 1:
        raise FileNotFoundError(f"{app}/{sched}/{rep}: {len(matches)} DONE files")
    path = open(matches[0]).read().strip()
    if not os.path.isdir(path):
        raise FileNotFoundError(path)
    return path


def plan_value(app: str, sched: str, rep: str, key: str) -> str:
    text = open(os.path.join(ROOT, app, sched, rep, "plan.txt")).read()
    for tok in text.split():
        if tok.startswith(key + "="):
            return tok.split("=", 1)[1]
    raise KeyError(key)


def _windows(run: str) -> tuple[list[dict], int]:
    windows = [json.loads(line) for line in open(os.path.join(run, "loadgen.hist.jsonl"))]
    t0 = min(w["t0_ns"] for w in windows)
    return windows, t0


def _add(hist: dict, sent: list[int], w: dict) -> None:
    sent[0] += w["sent"]
    sent[1] += w["errors"]
    for bound, count in w.get("hist") or []:
        hist[int(bound)] = hist.get(int(bound), 0) + count


def phase_hist(run: str, routes: list[str], kind: str) -> tuple[dict, int, int]:
    """Critical-route histogram of the windows inside this kind, both intensities."""
    windows, t0 = _windows(run)
    spans = []
    on = None
    for line in open(os.path.join(run, "antagonists.jsonl")):
        event = json.loads(line)
        if event.get("event") == "on" and event.get("kind") == kind:
            on = event
        elif event.get("event") == "off" and on is not None and on.get("phase") == event.get("phase"):
            spans.append((on["t_ns"], event["t_ns"]))
            on = None
    hist: dict = {}
    sent = [0, 0]
    for w in windows:
        if w["route"] not in routes:
            continue
        mid = (w["t0_ns"] + w["t1_ns"]) / 2
        if any(a <= mid < b for a, b in spans):
            _add(hist, sent, w)
    return hist, sent[0], sent[1]


def hold_ms(run: str, routes: list[str], drop_s: float) -> float:
    """Critical-route p95 of the clean hold after its warmup."""
    windows, t0 = _windows(run)
    hist: dict = {}
    for w in windows:
        if w["route"] not in routes:
            continue
        if (w["t0_ns"] - t0) / 1e9 < drop_s:
            continue
        for bound, count in w.get("hist") or []:
            hist[int(bound)] = hist.get(int(bound), 0) + count
    return pct(hist)


def measure() -> dict:
    """Per app, hold, arm, and repeat: the clean-hold p95 and each stressor's p95."""
    out: dict = {}
    for app in APPS:
        routes = ROUTES[app]
        out[app] = {}
        for level in LEVELS:
            hold = plan_value(app, f"{level}-ant-luna", "r1", "levels")
            arms = {}
            for arm, *_ in ARMS:
                reps = {}
                for rep in ("r1", "r2"):
                    drop = float(plan_value(app, f"{level}-clean-{arm}", rep, "warmup"))
                    clean = hold_ms(done_run(app, f"{level}-clean-{arm}", rep), routes, drop)
                    kinds = {}
                    ant = done_run(app, f"{level}-ant-{arm}", rep)
                    for kind, _ in STRESS:
                        hist, nsent, _err = phase_hist(ant, routes, kind)
                        if nsent:
                            kinds[kind] = pct(hist)
                    reps[rep] = {"clean": clean, "kinds": kinds}
                arms[arm] = reps
            out[app][level] = {"hold": hold, "arms": arms}
    return out


def _ratios(pairs: list[float]) -> dict | None:
    got = [r for r in pairs if r == r and r > 0]
    if not got:
        return None
    return {"ratios": got}


def stressor_of(measured: dict) -> dict:
    """Critical-route p95 in milliseconds: the clean hold, then each stressor."""
    out: dict = {}
    for app in APPS:
        out[app] = {}
        for level in LEVELS:
            panel = measured[app][level]
            arms = {}
            for arm, *_ in ARMS:
                kinds = {}
                clean = _ratios([rep["clean"] for rep in panel["arms"][arm].values()])
                if clean:
                    kinds["clean"] = clean
                for kind, _ in STRESS:
                    cell = _ratios([rep["kinds"][kind] for rep in panel["arms"][arm].values()
                                    if kind in rep["kinds"]])
                    if cell:
                        kinds[kind] = cell
                arms[arm] = {"kinds": kinds}
            out[app][level] = {"hold": panel["hold"], "arms": arms}
    return out


def luna_of(measured: dict) -> dict:
    """Fixed-pool p95 divided by Luna's, paired by repeat. Above 1, Luna is faster."""
    out: dict = {}
    for app in APPS:
        out[app] = {}
        for level in LEVELS:
            panel = measured[app][level]
            arms = {}
            for base, *_ in BASELINES:
                kinds = {}
                for kind, _ in LUNA_GROUPS:
                    ratios = []
                    for rep in ("r1", "r2"):
                        luna = panel["arms"]["luna"][rep]
                        other = panel["arms"][base][rep]
                        if kind == "clean":
                            num, den = other["clean"], luna["clean"]
                        else:
                            num, den = other["kinds"].get(kind), luna["kinds"].get(kind)
                        if num is None or den is None or not (num > 0) or not (den > 0):
                            continue
                        ratios.append(num / den)
                    cell = _ratios(ratios)
                    if cell:
                        kinds[kind] = cell
                arms[base] = {"kinds": kinds}
            out[app][level] = {"hold": panel["hold"], "arms": arms}
    return out


def _report(title: str, values: dict, groups: list[tuple[str, str]], series: list, unit: str) -> None:
    print(f"\n{title}")
    for app in APPS:
        for level in LEVELS:
            hold = values[app][level]["hold"]
            for key, label, *_ in series:
                bits = []
                for kind, name in groups:
                    got = values[app][level]["arms"][key]["kinds"].get(kind)
                    if not got:
                        bits.append(f"{name} —")
                        continue
                    rs = got["ratios"]
                    bits.append(f"{name} {statistics.median(rs):.1f}{unit} [{min(rs):.1f}–{max(rs):.1f}]")
                print(f"{app} {hold} {label}: " + "; ".join(bits))


def _ylim(values: dict, groups: list[tuple[str, str]], series: list) -> tuple[float, float]:
    seen = []
    for app in APPS:
        for level in LEVELS:
            for key, *_ in series:
                for kind, _ in groups:
                    got = values[app][level]["arms"][key]["kinds"].get(kind)
                    if got:
                        seen.extend(got["ratios"])
    lo = min(seen) * 0.55
    hi = max(seen) * 1.45
    return lo, hi


def draw(values: dict, groups: list[tuple[str, str]], series: list, ylabel: str, path: str,
         guide: bool = True) -> None:
    fig, axes = plt.subplots(3, 3, figsize=(FULL, 5.55), sharey=True, constrained_layout=True)
    n = len(series)
    width = 0.8 / n
    ymin, ymax = _ylim(values, groups, series)
    for row, level in enumerate(LEVELS):
        for col, app in enumerate(APPS):
            ax = axes[row][col]
            panel = values[app][level]
            for i, (kind, _label) in enumerate(groups):
                for j, (key, _name, color, filled) in enumerate(series):
                    got = panel["arms"][key]["kinds"].get(kind)
                    if not got:
                        continue
                    rs = got["ratios"]
                    ratio = statistics.median(rs)
                    x = i + (j - (n - 1) / 2) * width
                    ax.bar(x, ratio, width=width * 0.88,
                           color=color if filled else "white", edgecolor=BLACK,
                           linewidth=1.0, zorder=3)
                    lo, hi = min(rs), max(rs)
                    if len(rs) > 1 and hi > lo:
                        ax.errorbar(x, ratio, yerr=[[ratio - lo], [hi - ratio]], fmt="none",
                                    ecolor=BLACK, elinewidth=1.0, capsize=1.4, capthick=1.0, zorder=4)
            if guide:
                ax.axhline(1.0, color="#333333", lw=0.8, zorder=2)
            ax.set_yscale("log")
            ax.set_ylim(ymin, ymax)
            ax.yaxis.set_major_formatter(plt.FuncFormatter(lambda v, _p: f"{v:g}"))
            ax.yaxis.set_minor_formatter(NullFormatter())
            ax.grid(axis="y", color="#e6e6e6", linewidth=0.4)
            ax.grid(axis="x", visible=False)
            ax.set_title(f"{APP_NAME[app]} {panel['hold']}", loc="left", pad=2)
            if row == len(LEVELS) - 1:
                ax.set_xticks(range(len(groups)), [label for _, label in groups])
            else:
                ax.set_xticks(range(len(groups)), [])
    axes[1][0].set_ylabel(ylabel)
    handles = [Patch(facecolor=color if filled else "white", edgecolor=BLACK, linewidth=1.0, label=label)
               for _key, label, color, filled in series]
    fig.legend(handles=handles, ncol=len(series), loc="outside upper center", frameon=False,
               handlelength=1.4, borderaxespad=0.0)
    dress(fig)
    fig.savefig(path, bbox_inches=None)
    stem, ext = os.path.splitext(path)
    fig.savefig(stem + (".pdf" if ext.lower() == ".png" else ".png"), bbox_inches=None)
    plt.close(fig)


def main() -> None:
    face = apply()
    print(f"figure font: {face}")
    measured = measure()
    stress = stressor_of(measured)
    luna = luna_of(measured)
    tails = [("clean", "none")] + STRESS
    _report("stressor: critical p95, ms", stress, tails, ARMS, " ms")
    _report("luna: fixed p95 / Luna p95", luna, LUNA_GROUPS, BASELINES, "x")
    figs = os.path.join(ROOT, "figs")
    draw(stress, tails, ARMS, "Critical p95 (ms)", os.path.join(figs, "stressor.pdf"), guide=False)
    draw(luna, LUNA_GROUPS, BASELINES, "Fixed p95 / Luna p95", os.path.join(figs, "luna.pdf"))
    print("wrote", os.path.join(figs, "stressor.pdf"))
    print("wrote", os.path.join(figs, "luna.pdf"))


if __name__ == "__main__":
    main()
