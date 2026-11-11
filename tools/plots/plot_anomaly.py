#!/usr/bin/env python3
"""Sensitivity of the social and hotel benchmarks to each antagonist.

Drawn systems are Fixed 8, Fixed 40, ZeroCTL, and GRU, scale_ind+r_move (the Shift+L3
policy on the GRU autoscaler, the runs previously labelled FIRM Shift+L3).

The bar is the pooled p95 of the critical routes during that antagonist, divided by the pooled
p95 of the same routes in the windows with no antagonist. Critical routes are compose-post and
read-home-timeline on social, hotels and reservation on hotel. Both intensities of a kind are
pooled. A ratio above 1 is a slower tail than the quiet windows.

Social Fixed 8, ZeroCTL, and the saturate-autoscaler runs are social-eval-20261029; Fixed 40 is
the -p40 arm; GRU, scale_ind+r_move is l3shift in social-eval-20261029-gnn. Hotel Fixed 8 and GRU, scale_ind+r_move are
hotel-final-r5-20261023-0019 (l3shift there ran on the GRU autoscaler). Hotel Fixed 40 is
hotel-fixed40-20261027 repeat 1, which has no load antagonist. Hotel ZeroCTL is
hotel-llms-20261027 llm25-luna, repeats 1–3.
"""

from __future__ import annotations

import json
import os
import sys

import matplotlib.pyplot as plt
from matplotlib.patches import Patch
from matplotlib.ticker import NullFormatter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ieee_style import FULL, apply

# The comparison never crosses type and place: a CPU hog is only on the victim core or its
# SMT sibling, the cache thrash only on the rest of that L3, and the stream only on the
# same NUMA node outside that L3. The first line is the stressor, the second where it sits.
KINDS = [
    ("same", "CPU\nsame core"),
    ("smt", "CPU\nSMT sibling"),
    ("l3", "cache\nsame L3"),
    ("membw", "bandwidth\nother L3"),
    ("load", "requests\n×1.5–2"),
]
OUTLINE = "#1a1a1a"
SYSTEMS = [
    ("fixed8", "Fixed 8", "#b0b0b0", True),
    ("fixed40", "Fixed 40", "#333333", False),
    ("luna", "ZeroCTL", "#D55E00", True),
    ("gru", "GRU, scale_ind+r_move", "#882255", True),
]
SOCIAL_ROUTES = ["compose-post", "read-home-timeline"]
HOTEL_ROUTES = ["hotels", "reservation"]


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


def blank() -> dict:
    return {"sent": 0, "errors": 0, "hist": {}}


def add(dst: dict, src: dict) -> None:
    dst["sent"] += src["sent"]
    dst["errors"] += src["errors"]
    for bound, count in src["hist"].items():
        dst["hist"][bound] = dst["hist"].get(bound, 0) + count


def load(specs: list[str], dir_renames: dict[str, dict[str, str]]) -> list[dict]:
    runs = []
    for spec in specs:
        path, _, renames = spec.partition(":")
        mapping = dict(pair.split("=", 1) for pair in renames.split(",") if pair)
        for comp in json.load(open(path))["comparisons"]:
            by_dir = dir_renames.get(os.path.basename(str(comp.get("dir", "")).rstrip("/")), {})
            for run in comp["runs"]:
                system = by_dir.get(run["system"], run["system"])
                run = dict(run)
                run["system"] = mapping.get(system, system)
                runs.append(run)
    return runs


def _one(run: dict, routes: list[str], kind: str | None) -> dict:
    """Quiet windows, or every phase of one kind, for the critical routes of one repeat."""
    part = blank()
    if kind is None:
        for route in routes:
            piece = run["routes"].get(route, {}).get("off")
            if piece:
                add(part, piece)
        return part
    for phase in run["phases"]:
        if phase["kind"] != kind:
            continue
        for route in routes:
            piece = phase["routes"].get(route)
            if piece:
                add(part, piece)
    return part


def ratios(runs: list[dict], routes: list[str]) -> dict[str, dict]:
    """Pooled p95(on) / p95(off) over every repeat, plus the min and max of the per-repeat ratios.

    The bar uses every request from every repeat. The whisker is that repeat's own ratio, so a
    repeat with no phase of that kind is left out of the whisker and out of the pool."""
    kept = [r for r in runs if r["system"] in {s for s, *_ in SYSTEMS}]
    off = {s: blank() for s, *_ in SYSTEMS}
    on = {(s, kind): blank() for s, *_ in SYSTEMS for kind, _ in KINDS}
    per: dict[tuple[str, str], list[float]] = {}
    for run in kept:
        quiet = _one(run, routes, None)
        add(off[run["system"]], quiet)
        quiet_p = pct(quiet["hist"])
        for kind, _ in KINDS:
            part = _one(run, routes, kind)
            if part["sent"] == 0:
                continue
            add(on[(run["system"], kind)], part)
            if quiet_p == quiet_p and quiet_p > 0:
                per.setdefault((run["system"], kind), []).append(pct(part["hist"]) / quiet_p)
    out = {}
    for system, *_ in SYSTEMS:
        base = pct(off[system]["hist"])
        out[system] = {"none_ms": base, "n_runs": sum(1 for r in kept if r["system"] == system)}
        for kind, _ in KINDS:
            part = on[(system, kind)]
            ratios_rep = per.get((system, kind), [])
            if part["sent"] == 0 or not (base == base) or base <= 0 or not ratios_rep:
                continue
            pooled = pct(part["hist"]) / base
            out[system][kind] = pooled
            out[system][kind + "_lo"] = min(ratios_rep)
            out[system][kind + "_hi"] = max(ratios_rep)
            out[system][kind + "_n"] = len(ratios_rep)
            out[system][kind + "_ms"] = pct(part["hist"])
            out[system][kind + "_err"] = 100 * part["errors"] / part["sent"]
    return out


def _panel(ax, values: dict[str, dict[str, float]]) -> None:
    n = len(SYSTEMS)
    width = 0.8 / n
    for i, (kind, label) in enumerate(KINDS):
        for j, (system, _name, color, filled) in enumerate(SYSTEMS):
            ratio = values[system].get(kind)
            if ratio is None:
                continue
            x = i + (j - (n - 1) / 2) * width
            ax.bar(x, ratio, width=width * 0.88,
                   color=color if filled else "white", edgecolor=OUTLINE, linewidth=1.0, zorder=3)
            lo = min(values[system][kind + "_lo"], ratio)
            hi = max(values[system][kind + "_hi"], ratio)
            if values[system][kind + "_n"] > 1 and hi > lo:
                ax.errorbar(x, ratio, yerr=[[ratio - lo], [hi - ratio]], fmt="none",
                            ecolor=OUTLINE, elinewidth=1.0, capsize=1.6, capthick=1.0, zorder=4)
    ax.axhline(1.0, color="#333333", lw=0.8, zorder=2)
    ax.set_xticks(range(len(KINDS)), [label for _, label in KINDS])
    ax.set_yscale("log")
    ax.yaxis.set_major_formatter(plt.FuncFormatter(lambda v, _p: f"{v:g}"))
    ax.yaxis.set_minor_formatter(NullFormatter())
    ax.set_ylim(0.015, 800)
    ax.grid(axis="y", color="#e6e6e6", linewidth=0.4)
    ax.grid(axis="x", visible=False)


def main() -> None:
    face = apply()
    print(f"figure font: {face}")
    social = load(
        ["data/dsb/compare/social-eval-20261029-runs.json:llm40-luna=luna"],
        {"social-eval-20261029": {"saturate": "fixed8"},
         "social-eval-20261029-p40": {"saturate": "fixed40"},
         "social-eval-20261029-gnn": {"l3shift": "gru"}},
    )
    hotel = load(
        ["data/paper/figs/all-policies-and-models/runs.json:saturate=fixed8,l3shift=gru",
         "data/dsb/compare/hotel-fixed40-20261027-runs.json:saturate=fixed40",
         "data/dsb/compare/hotel-llms-20261027-luna.json:llm25-luna=luna"],
        {},
    )
    panels = [("Hotel", ratios(hotel, HOTEL_ROUTES)), ("Social", ratios(social, SOCIAL_ROUTES))]
    for name, values in panels:
        for system, label, *_ in SYSTEMS:
            row = values[system]
            bits = [f"{label} n={row['n_runs']} quiet {row['none_ms']:.1f} ms"]
            for kind, pretty in KINDS:
                if kind in row:
                    bits.append(f"{pretty} {row[kind]:.2f}x [{row[kind + '_lo']:.2f}–{row[kind + '_hi']:.2f}] "
                                f"n={row[kind + '_n']}")
            print(name, "; ".join(bits))
    fig, axes = plt.subplots(1, 2, figsize=(FULL, 1.93), sharey=True, constrained_layout=True)
    for ax, (name, values) in zip(axes, panels):
        _panel(ax, values)
        ax.set_title(name, loc="left", pad=2)
    axes[0].set_ylabel("Critical p95 / no antagonist")
    handles = [Patch(facecolor=color if filled else "white", edgecolor=OUTLINE, linewidth=1.0, label=label)
               for _s, label, color, filled in SYSTEMS]
    fig.legend(handles=handles, ncol=4, loc="outside upper center", frameon=False, handlelength=1.4)
    out = sys.argv[1] if len(sys.argv) > 1 else "anomaly.pdf"
    from ieee_style import dress
    dress(fig)
    fig.savefig(out, bbox_inches=None)
    stem, ext = os.path.splitext(out)
    fig.savefig(stem + (".pdf" if ext.lower() == ".png" else ".png"), bbox_inches=None)
    plt.close(fig)


if __name__ == "__main__":
    main()
