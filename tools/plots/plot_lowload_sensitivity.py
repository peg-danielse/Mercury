#!/usr/bin/env python3
"""Primary-route p95 minus the baseline, divided by that repeat's core level 2.

The runs are data/dsb/compare/ant-sensitivity-20261110. Each repeat's bar
value is that phase's p95 minus the same repeat's baseline, divided by the
same difference for the core hog at level 2. That value is 1 and the baseline
is 0. The bar is the median across repeats. The line is their minimum and
maximum. One repeat has no line. The applications do not share a denominator.
"""

from __future__ import annotations

import glob
import os
import statistics
import sys

import matplotlib.pyplot as plt
from matplotlib.patches import Patch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "src", "test")))
from ant_sensitivity_report import (  # noqa: E402
    PRIMARY,
    accumulate,
    baseline_span,
    events_of,
    percentile,
    phases_of,
    windows_of,
)
from ieee_style import COLUMN, apply, dress

ROOT = "data/dsb/compare/ant-sensitivity-20261110"
# tab20 pairs. The light tint is level 1 and the saturated colour is level 2.
_TAB20 = plt.get_cmap("tab20").colors
KIND_SHADE = {
    "same": (_TAB20[1], _TAB20[0]),
    "smt": (_TAB20[3], _TAB20[2]),
    "l3": (_TAB20[5], _TAB20[4]),
    "membw": (_TAB20[7], _TAB20[6]),
}
APPS = ["hotel", "social", "media"]
APP_LABEL = {"hotel": "Hotel", "social": "Social", "media": "Media"}
# same is a core hog, smt its sibling, l3 the other cores of that cache, membw the memory bus.
GROUPS = [
    ("baseline", "none"),
    ("same", "core"),
    ("smt", "SMT"),
    ("l3", "L3"),
    ("membw", "bus"),
]


def measure_run(app: str, run: str) -> dict:
    events = events_of(run)
    windows = windows_of(run)
    span = baseline_span(events)
    if span is None:
        raise FileNotFoundError(f"{app}: no baseline in {run}")
    routes = [PRIMARY[app]]
    base = accumulate(windows, routes, span[0], span[1])
    cell = {"baseline": percentile(base["hist"], 0.95)}
    for phase in phases_of(events):
        stat = accumulate(windows, routes, phase["t_ns"], phase["t1_ns"])
        cell[(phase["kind"], int(phase["level"]))] = percentile(stat["hist"], 0.95)
    return cell


def measure(root: str) -> dict:
    out = {}
    for app in APPS:
        dones = sorted(glob.glob(os.path.join(root, app, "*", "r*", "DONE")))
        if not dones:
            raise FileNotFoundError(f"{app}: no DONE files")
        out[app] = [measure_run(app, open(done).read().strip()) for done in dones]
    return out


def shares(cells: list[dict]) -> dict:
    """Each repeat divided by its own core level 2. Values are that share."""
    out = {}
    for cell in cells:
        diff = differences(cell)
        scale = diff.get(("same", 2))
        if not scale:
            continue
        for key, value in diff.items():
            out.setdefault(key, []).append(value / scale)
    return out


def whisker(ax, x: float, series: list[float], width: float) -> None:
    if len(series) < 2 or max(series) <= min(series):
        return
    lo, hi = min(series), max(series)
    cap = width * 0.28
    ax.plot([x, x], [lo, hi], color="#1a1a1a", lw=0.8, zorder=4, solid_capstyle="butt")
    ax.plot([x - cap, x + cap], [lo, lo], color="#1a1a1a", lw=0.8, zorder=4)
    ax.plot([x - cap, x + cap], [hi, hi], color="#1a1a1a", lw=0.8, zorder=4)


def differences(cell: dict) -> dict:
    """Primary-route p95 minus that run's baseline, in milliseconds."""
    base = cell["baseline"]
    out = {}
    for kind, _label in GROUPS:
        if kind == "baseline":
            continue
        for level in (1, 2):
            value = cell.get((kind, level))
            if value is not None and base is not None:
                out[(kind, level)] = value - base
    return out


def draw(values: dict, path: str) -> None:
    kinds = [(kind, label) for kind, label in GROUPS if kind != "baseline"]
    # One axis. Hotel is the left group. Within an application, level 1 sits
    # left of level 2 for each antagonist, and left to right is core, SMT, L3, bus.
    fig, ax = plt.subplots(figsize=(COLUMN, 1.81), constrained_layout=True)
    bar_w = 0.36
    within = 0.04
    kind_gap = 0.12
    app_gap = 0.9
    scaled = {app: shares(values[app]) for app in APPS}
    for app in APPS:
        if not scaled[app].get(("same", 2)):
            raise ValueError(f"{app} has no repeat whose core level 2 differs from its baseline")
    pos = {}
    x = 0.0
    for app in APPS:
        first = True
        for kind, _label in kinds:
            present = []
            for level in (1, 2):
                series = scaled[app].get((kind, level), [])
                if series and any(value != 0 for value in series):
                    present.append(level)
            if not present:
                continue
            if not first:
                x += kind_gap
            first = False
            for level in present:
                pos[(app, kind, level)] = x
                x += bar_w + within
            x -= within
        x += app_gap
    lowest = 0.0
    highest = 1.0
    for app in APPS:
        for kind, _label in kinds:
            for level in (1, 2):
                series = scaled[app].get((kind, level), [])
                if not series or not any(value != 0 for value in series):
                    continue
                lowest = min(lowest, min(series))
                highest = max(highest, max(series))
                color = KIND_SHADE[kind][level - 1]
                ax.bar(pos[(app, kind, level)], statistics.median(series), width=bar_w,
                       color=color, edgecolor="black", linewidth=0.4, zorder=3)
                whisker(ax, pos[(app, kind, level)], series, bar_w)
    centers = []
    for app in APPS:
        xs = [pos[(app, kind, level)] for kind, _label in kinds for level in (1, 2)
              if (app, kind, level) in pos]
        centers.append(sum(xs) / len(xs))
    right = max(pos.values())
    ax.set_xticks(centers, [APP_LABEL[app] for app in APPS])
    ax.set_xlim(-bar_w / 2 - 0.2, right + bar_w / 2 + 0.2)
    ax.axhline(0.0, color="#333333", lw=0.8, zorder=5)
    ax.set_ylim(lowest, highest)
    ax.set_yticks([0, 0.5, 1])
    ax.set_ylabel("Impact on p95 latency")
    ax.grid(axis="y", color="#e6e6e6", linewidth=0.4)
    ax.grid(axis="x", visible=False)
    handles = [Patch(facecolor=KIND_SHADE[kind][1], edgecolor="black", linewidth=0.4, label=label)
               for kind, label in kinds]
    fig.legend(handles=handles, ncol=4, loc="outside upper center", frameon=False,
               handlelength=1.4, borderaxespad=0.0)
    dress(fig)
    fig.savefig(path, bbox_inches=None)
    stem, ext = os.path.splitext(path)
    fig.savefig(stem + (".pdf" if ext.lower() == ".png" else ".png"), bbox_inches=None)
    plt.close(fig)


def main() -> None:
    root = sys.argv[1] if len(sys.argv) > 1 else ROOT
    face = apply()
    print(f"figure font: {face}")
    values = measure(root)
    for app in APPS:
        print(f"{app} {PRIMARY[app]}: {len(values[app])} repeats")
        for cell in values[app]:
            base = cell["baseline"]
            bits = [f"baseline {base:.2f}"]
            for kind, label in GROUPS[1:]:
                for level in (1, 2):
                    value = cell.get((kind, level))
                    if value is None:
                        bits.append(f"{label}{level} —")
                    else:
                        bits.append(f"{label}{level} {value - base:+.2f}")
            print("  " + ", ".join(bits))
        scaled = shares(values[app])
        for kind, label in GROUPS[1:]:
            for level in (1, 2):
                series = scaled.get((kind, level), [])
                if not series:
                    continue
                span = f" [{min(series):.2f}, {max(series):.2f}]" if len(series) > 1 else ""
                print(f"  {label}{level} median {statistics.median(series):.2f}{span}")
    figs = os.path.join(root, "figs")
    os.makedirs(figs, exist_ok=True)
    path = os.path.join(figs, "sensitivity.pdf")
    draw(values, path)
    print("wrote", path)


if __name__ == "__main__":
    main()
