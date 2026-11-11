#!/usr/bin/env python3
"""Grouped bars of L3 use for the social and hotel runs behind the speedup figure.

Each bar is the median across repeats. A domain's busy fraction is the mean, over the
whole run, of the four logical CPUs that share that L3. The hottest bar is the busiest
of the 16 domains. Misses are host-wide LLC misses per 1,000 instructions.

Social is repeats 1–5 of social-eval-20261029 (Fixed 40 from the -p40 arm, FIRM from
the -gnn arm). Hotel is repeats 1–3 of hotel-final-r5-20261023-0019, ZeroCTL from
hotel-llms-20261027 llm25-luna. Hotel Fixed 40 is hotel-fixed40-20261027 repeat 1 only.
Hotel has no plain Shift+L3 or Degrade in that comparison, so those slots stay empty.
"""

from __future__ import annotations

import os
import sys

import matplotlib.pyplot as plt
from matplotlib.patches import Patch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ieee_style import FULL, apply

# Hottest L3 busy percent, then LLC misses per 1,000 instructions. None is a system
# that comparison did not run.
SYSTEMS = [
    ("Fixed 8", "#333333", True),
    ("Fixed 40", "#333333", False),
    ("ZeroCTL", "#D55E00", True),
    ("Shift+L3", "#882255", True),
    ("FIRM Shift+L3", "#882255", False),
    ("Degrade", "#CC79A7", True),
    ("FIRM Degrade", "#CC79A7", False),
]
HOTEL = {
    "Fixed 8": (88.5, 17.9),
    "Fixed 40": (37.3, 8.7),
    "ZeroCTL": (55.5, 8.5),
    "FIRM Shift+L3": (80.9, 12.4),
    "FIRM Degrade": (88.9, 12.5),
}
SOCIAL = {
    "Fixed 8": (35.7, 24.5),
    "Fixed 40": (25.8, 26.6),
    "ZeroCTL": (25.8, 23.3),
    "Shift+L3": (25.8, 23.7),
    "FIRM Shift+L3": (26.8, 23.8),
    "Degrade": (35.5, 24.4),
    "FIRM Degrade": (32.8, 24.4),
}


def _bars(ax, groups: list[tuple[str, dict]], metric: int) -> None:
    width = 0.82
    gap = 1.4
    n = len(SYSTEMS)
    centers = []
    for gi, (label, values) in enumerate(groups):
        origin = gi * (n * width + gap)
        for i, (name, color, filled) in enumerate(SYSTEMS):
            pair = values.get(name)
            if pair is None:
                continue
            ax.bar(origin + i * width, pair[metric], width=width * 0.88,
                   color=color if filled else "white", edgecolor=color, linewidth=0.8, zorder=3)
        centers.append((origin + (n - 1) * width / 2, label))
    ax.set_xticks([c for c, _ in centers], [label for _, label in centers])
    ax.tick_params(axis="x", length=0)
    ax.set_xlim(-0.7, (len(groups) - 1) * (n * width + gap) + (n - 1) * width + 0.5)
    ax.grid(axis="y", color="#e6e6e6", linewidth=0.4)
    ax.grid(axis="x", visible=False)


def main() -> None:
    face = apply()
    print(f"figure font: {face}")
    fig, axes = plt.subplots(1, 2, figsize=(FULL, 2.85), constrained_layout=True)
    groups = [("Hotel", HOTEL), ("Social", SOCIAL)]
    _bars(axes[0], groups, 0)
    _bars(axes[1], groups, 1)
    axes[0].set_ylabel("Hottest L3 busy (%)")
    axes[1].set_ylabel("LLC misses per 1,000 instructions")
    axes[0].set_ylim(0, 100)
    axes[1].set_ylim(0, 32)
    for ax, letter in zip(axes, "ab"):
        ax.set_title(f"({letter})", loc="left", pad=2)
    handles = [Patch(facecolor=color if filled else "white", edgecolor=color, linewidth=0.8, label=name)
               for name, color, filled in SYSTEMS]
    fig.legend(handles=handles, ncol=4, loc="outside upper center",
               frameon=False, handlelength=1.4, columnspacing=1.0)
    out = sys.argv[1] if len(sys.argv) > 1 else "l3bars.pdf"
    from ieee_style import dress
    dress(fig)
    fig.savefig(out, bbox_inches=None)
    stem, ext = os.path.splitext(out)
    fig.savefig(stem + (".pdf" if ext.lower() == ".png" else ".png"), bbox_inches=None)
    plt.close(fig)


if __name__ == "__main__":
    main()
