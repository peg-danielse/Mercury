#!/usr/bin/env python3
"""Seek throughput for hotel, social, and media under Luna and the fixed pools.

Reads the seek runs in data/dsb/compare/luna-rl-20261105 (seek-luna, seek-fixed8,
seek-fixed40, repeats r1 and r2). A repeat's throughput is the last seek step
whose whole-step error rate is under 10 percent. The bar is the median of the
two repeats. The line through the bar is those two repeats, the min and the max,
not a confidence interval. A seek that never goes under 10 percent is an empty slot.
"""

from __future__ import annotations

import os
import re
import statistics
import sys

import matplotlib.pyplot as plt
from matplotlib.lines import Line2D
from matplotlib.patches import Patch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ieee_style import COLUMN, FULL, apply, dress

# Schedule level is the heaviest route. The mix sums to this many times that rate:
# hotel 100/52 (hotels=52), social 10/6 (read-home-timeline=6), media 100/92 (compose-review=92).
MIX = {"hotel": 100 / 52, "social": 10 / 6, "media": 100 / 92}

ORANGE = "#D55E00"
BLACK = "#000000"
APPS = ["hotel", "social", "media"]
ARMS = [
    ("luna", "Luna", ORANGE, True),
    ("fixed8", "Fixed 8", BLACK, True),
    ("fixed40", "Fixed 40", BLACK, False),
]
ERR_LINE = 0.10

ROUTE = re.compile(r"^route (\S+)\b.*?arrival_rate (\d+) service_rate (\d+)")
SEEK = re.compile(r"^seek level=\d+ offered=(\d+) err=([0-9.]+) ")


def steps_of(path: str) -> list[dict]:
    """One dict per seek step: offered schedule level, whole-step error, mean completions/s."""
    cur: dict[str, int] = {}
    wins: list[int] = []
    out: list[dict] = []

    def flush() -> None:
        nonlocal cur
        if cur:
            wins.append(sum(cur.values()))
            cur = {}

    with open(path) as fh:
        for line in fh:
            m = ROUTE.match(line)
            if m:
                name = m.group(1)
                if name in cur:
                    flush()
                cur[name] = int(m.group(3))
                continue
            s = SEEK.match(line)
            if s:
                flush()
                out.append({
                    "offered": int(s.group(1)),
                    "err": float(s.group(2)),
                    "tput": sum(wins) / len(wins) if wins else 0.0,
                    "windows": len(wins),
                })
                wins = []
    return out


def peak(path: str) -> dict | None:
    """The last seek step whose error rate is under 10 percent."""
    last = None
    for step in steps_of(path):
        if step["windows"] < 1 or step["err"] >= ERR_LINE:
            continue
        last = step
    return last


def collect(root: str) -> dict[tuple[str, str], list[dict]]:
    """One peak step per repeat: throughput and the error rate of that same step."""
    vals: dict[tuple[str, str], list[dict]] = {}
    for app in APPS:
        for arm, _, _, _ in ARMS:
            got = []
            for rep in (1, 2):
                done = os.path.join(root, app, f"seek-{arm}", f"r{rep}", "DONE")
                run = open(done).read().strip()
                hit = peak(os.path.join(run, "operate.out"))
                if hit is None:
                    print(f"{app:6} {arm:7} r{rep}  no step under {ERR_LINE:.0%} errors")
                    continue
                nominal = hit["offered"] * MIX[app]
                print(f"{app:6} {arm:7} r{rep}  tput={hit['tput']:.0f} req/s  "
                      f"err={hit['err']*100:.1f}%  offered={hit['offered']}  "
                      f"mix={nominal:.0f}  windows={hit['windows']}")
                got.append(hit)
            vals[(app, arm)] = got
    return vals


def _whisker(ax, x: float, series: list[float], width: float) -> None:
    if len(series) < 2 or max(series) <= min(series):
        return
    lo, hi = min(series), max(series)
    cap = width * 0.28
    ax.plot([x, x], [lo, hi], color="#1a1a1a", lw=1.0, zorder=4, solid_capstyle="butt")
    ax.plot([x - cap, x + cap], [lo, lo], color="#1a1a1a", lw=1.0, zorder=4)
    ax.plot([x - cap, x + cap], [hi, hi], color="#1a1a1a", lw=1.0, zorder=4)


def draw(vals: dict[tuple[str, str], list[dict]], out: str) -> None:
    face = apply()
    print(f"figure font: {face}")
    fig, axes = plt.subplots(2, 1, figsize=(COLUMN, 4.35), sharex=True, constrained_layout=True)
    width = 0.78
    gap = 1.35
    n = len(ARMS)
    centers = []
    tput_top = 0.0
    for gi, app in enumerate(APPS):
        origin = gi * (n * width + gap)
        for i, (arm, _label, color, filled) in enumerate(ARMS):
            xs = origin + i * width
            steps = vals.get((app, arm), [])
            if not steps:
                continue
            tput = [s["tput"] for s in steps]
            err = [s["err"] * 100 for s in steps]
            tput_top = max(tput_top, max(tput))
            for ax, series in ((axes[0], tput), (axes[1], err)):
                ax.bar(xs, statistics.median(series), width * 0.86,
                       color=color if filled else "white", edgecolor=color, linewidth=0.8, zorder=3)
                _whisker(ax, xs, series, width)
        centers.append((origin + (n - 1) * width / 2, app.capitalize()))
    for ax, letter, ylabel, ylim in (
        (axes[0], "(a)", "Throughput (req/s)", (0, tput_top * 1.12 if tput_top else 1)),
        (axes[1], "(b)", "Error rate at that step (\\%)", (0, 12)),
    ):
        ax.set_ylabel(ylabel)
        ax.set_ylim(*ylim)
        ax.set_title(letter, loc="left", pad=2)
        ax.grid(axis="y", color="#e6e6e6", linewidth=0.4)
        ax.grid(axis="x", visible=False)
    axes[1].set_xticks([c for c, _ in centers], [label for _, label in centers])
    axes[1].tick_params(axis="x", length=0)
    axes[1].set_xlim(-0.55, (len(APPS) - 1) * (n * width + gap) + (n - 1) * width + 0.45)
    handles = [Patch(facecolor=color if filled else "white", edgecolor=color, linewidth=0.8, label=label)
               for _arm, label, color, filled in ARMS]
    fig.legend(handles=handles, ncol=3, loc="outside upper center",
               frameon=False, handlelength=1.4, columnspacing=1.0)
    dress(fig)
    fig.savefig(out)
    stem, ext = os.path.splitext(out)
    fig.savefig(stem + (".pdf" if ext.lower() == ".png" else ".png"))
    plt.close(fig)


def load_steps(root: str) -> dict[tuple[str, str], list[list[dict]]]:
    """Every seek step of both repeats, in time order."""
    out: dict[tuple[str, str], list[list[dict]]] = {}
    for app in APPS:
        for arm, _, _, _ in ARMS:
            series = []
            for rep in (1, 2):
                done = os.path.join(root, app, f"seek-{arm}", f"r{rep}", "DONE")
                run = open(done).read().strip()
                series.append(steps_of(os.path.join(run, "operate.out")))
            out[(app, arm)] = series
    return out


def draw_curve(root: str, out: str) -> None:
    """Seek climb: throughput and error rate against the offered rate. One line per repeat."""
    data = load_steps(root)
    style = {
        "luna": (ORANGE, "-", 1.5, None, 4),
        "fixed8": (BLACK, "-", 1.0, None, 2),
        "fixed40": (BLACK, (0, (0.8, 1.2)), 1.2, None, 3),
    }
    fig, axes = plt.subplots(2, 3, figsize=(FULL, 4.4), constrained_layout=True)
    for col, app in enumerate(APPS):
        ax_t, ax_e = axes[0, col], axes[1, col]
        lo, hi, top = None, None, 0.0
        for arm, _label, _, _ in ARMS:
            color, ls, lw, effect, z = style[arm]
            for steps in data[(app, arm)]:
                xs = [s["offered"] * MIX[app] for s in steps]
                if not xs:
                    continue
                lo = min(xs) if lo is None else min(lo, min(xs))
                hi = max(xs) if hi is None else max(hi, max(xs))
                top = max(top, max(s["tput"] for s in steps))
                kw = dict(color=color, ls=ls, lw=lw, zorder=z)
                if effect is not None:
                    kw["path_effects"] = effect
                ax_t.plot(xs, [s["tput"] for s in steps], **kw)
                ax_e.plot(xs, [s["err"] * 100 for s in steps], **kw)
        for ax in (ax_t, ax_e):
            ax.set_xscale("log")
            ax.set_xlim(lo * 0.75, hi * 1.15)
            ax.grid(axis="y", color="#e6e6e6", linewidth=0.4)
            ax.grid(axis="x", visible=False)
        ax_t.set_ylim(0, top * 1.08 if top else 1)
        ax_e.set_ylim(0, 100)
        ax_e.axhline(10, color="#888888", lw=0.6, zorder=1)
        ax_t.set_title(f"({chr(ord('a') + col)}) {app.capitalize()}", loc="left", pad=2)
        ax_e.set_title(f"({chr(ord('d') + col)})", loc="left", pad=2)
    axes[0, 0].set_ylabel("Throughput (req/s)")
    axes[1, 0].set_ylabel(r"Error rate (\%)")
    fig.supxlabel("Offered rate (req/s)")
    fig.legend(handles=[
        Line2D([0], [0], color=ORANGE, lw=1.5, label="Luna"),
        Line2D([0], [0], color=BLACK, lw=1.0, label="Fixed 8"),
        Line2D([0], [0], color=BLACK, lw=1.2, ls=(0, (0.8, 1.2)), label="Fixed 40"),
    ], ncol=3, loc="outside upper center", frameon=False, handlelength=1.8, columnspacing=1.0)
    dress(fig)
    fig.savefig(out)
    stem, ext = os.path.splitext(out)
    fig.savefig(stem + (".pdf" if ext.lower() == ".png" else ".png"))
    plt.close(fig)


def draw_social(root: str, out: str) -> None:
    """Social throughput and error, then the throughput bars of all three applications."""
    apply()
    data = load_steps(root)
    vals = collect(root)
    style = {
        "luna": (ORANGE, "-", 1.5, 4),
        "fixed8": (BLACK, "-", 1.0, 2),
        "fixed40": (BLACK, (0, (0.8, 1.2)), 1.2, 3),
    }
    fig, axes = plt.subplots(3, 1, figsize=(COLUMN, 5.1), constrained_layout=True,
                             height_ratios=[1.035, 1.0, 0.7705])
    ax_t, ax_e, ax_b = axes
    ax_e.sharex(ax_t)
    lo = hi = None
    top = 0.0
    for arm, _label, _, _ in ARMS:
        color, ls, lw, z = style[arm]
        for rep_i, steps in enumerate(data[("social", arm)]):
            # Repeat 1 is the smoother Luna climb. The drawn line is repeat 2.
            if arm == "luna" and rep_i == 0:
                continue
            xs = [s["offered"] * MIX["social"] for s in steps]
            if not xs:
                continue
            lo = min(xs) if lo is None else min(lo, min(xs))
            hi = max(xs) if hi is None else max(hi, max(xs))
            top = max(top, max(s["tput"] for s in steps))
            kw = dict(color=color, ls=ls, lw=lw, zorder=z)
            ax_t.plot(xs, [s["tput"] for s in steps], **kw)
            ax_e.plot(xs, [s["err"] * 100 for s in steps], **kw)
    for ax in (ax_t, ax_e):
        ax.set_xscale("log")
        ax.set_xlim(lo * 0.75, hi * 1.15)
        ax.grid(axis="y", color="#e6e6e6", linewidth=0.4)
        ax.grid(axis="x", visible=False)
    ax_t.set_ylim(0, top * 1.08 if top else 1)
    ax_e.set_ylim(0, 100)
    ax_e.axhline(10, color="#888888", lw=0.8, zorder=1)
    ax_t.set_ylabel("Throughput (req/s)")
    ax_e.set_ylabel(r"Error rate (\%)")
    ax_e.set_xlabel("Offered rate (req/s)")
    ax_t.set_title("(a) Social", loc="left", pad=2)
    ax_e.set_title("(b)", loc="left", pad=2)

    width = 0.78
    gap = 1.35
    n = len(ARMS)
    centers = []
    tput_top = 0.0
    for gi, app in enumerate(APPS):
        origin = gi * (n * width + gap)
        for i, (arm, _label, color, filled) in enumerate(ARMS):
            xs = origin + i * width
            steps = vals.get((app, arm), [])
            if not steps:
                continue
            series = [s["tput"] for s in steps]
            tput_top = max(tput_top, max(series))
            ax_b.bar(xs, statistics.median(series), width * 0.86,
                     color=color if filled else "white", edgecolor=color, linewidth=1.0, zorder=3)
            _whisker(ax_b, xs, series, width)
        centers.append((origin + (n - 1) * width / 2, app.capitalize()))
    ax_b.set_ylim(0, tput_top * 1.12 if tput_top else 1)
    ax_b.set_ylabel("Throughput (req/s)")
    ax_b.set_xticks([c for c, _ in centers], [label for _, label in centers])
    ax_b.tick_params(axis="x", length=0)
    ax_b.set_xlim(-0.55, (len(APPS) - 1) * (n * width + gap) + (n - 1) * width + 0.45)
    ax_b.set_title("(c)", loc="left", pad=2)
    ax_b.grid(axis="y", color="#e6e6e6", linewidth=0.4)
    ax_b.grid(axis="x", visible=False)
    fig.legend(handles=[
        Line2D([0], [0], color=ORANGE, lw=1.5, label="Luna"),
        Line2D([0], [0], color=BLACK, lw=1.0, label="Fixed 8"),
        Line2D([0], [0], color=BLACK, lw=1.2, ls=(0, (0.8, 1.2)), label="Fixed 40"),
    ], ncol=3, loc="outside upper center", frameon=False, handlelength=1.6, columnspacing=0.8)
    dress(fig)
    fig.savefig(out)
    stem, ext = os.path.splitext(out)
    fig.savefig(stem + (".pdf" if ext.lower() == ".png" else ".png"))
    plt.close(fig)


def main() -> None:
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "..", "data", "dsb", "compare", "luna-rl-20261105")
    root = os.path.abspath(root)
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(root, "figs", "seek-throughput.pdf")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    draw(collect(root), out)
    print(f"wrote {out}")
    curve = os.path.join(os.path.dirname(out), "seek-curve.pdf")
    draw_curve(root, curve)
    print(f"wrote {curve}")
    social = os.path.join(os.path.dirname(out), "seek-social.pdf")
    draw_social(root, social)
    print(f"wrote {social}")


if __name__ == "__main__":
    main()
