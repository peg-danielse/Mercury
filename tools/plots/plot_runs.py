#!/usr/bin/env python3
"""Paper figures from extract_runs.py output (vector PDFs, column width by default).

  plot_runs.py runs.json[:llm=llm-25,...] [more.json[:...]] -d figs/ [--width 3.4] [--critical hotels,reservation]
               [--summary p95:hotels,err:reservation,err:optional,pool] [--latency hotels]

An input's optional suffix renames systems in that file, so a second comparison (say an LLM-only rerun with a smaller
budget) sits beside the first. Figures:
  summary.pdf   per system, every repeat as a dot and the median as a tick, one panel per --summary metric
                (p95:ROUTES, p99:ROUTES, err:ROUTES, pool), small multiples on one system axis
  cost.pdf      pool core-seconds against the --latency routes' p95, one point per repeat, medians labelled
  phases.pdf    --latency p95 inside antagonist phases, per antagonist kind, against the windows outside phases
  timeline.pdf  the median repeat of every system: --latency p95 per bin with the antagonist phases shaded, and the
                pool's size (double column)
ROUTES is route names joined by '+', or critical / optional (the routes in / not in --critical).
Percentiles come from the merged latency histograms (upper bucket bounds). Failed requests are not in the histograms;
--errors-as-ms counts them as requests of that latency (a timeout) instead, so a system cannot gain p95 by failing.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import statistics

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
from matplotlib.transforms import Bbox  # noqa: E402

ORDER = ["saturate", "l3", "shift", "l3shift", "degrade", "llm"]
NAMES = {"saturate": "Fixed", "l3": "L3", "shift": "Shift", "l3shift": "L3+Shift", "degrade": "Degrade", "llm": "LLM"}
# Fixed-order categorical palette (validated for CVD separation); every system also has its own marker, and the
# contrast of the lighter hues obliges labels: systems are named on the axis or next to their marks.
COLORS = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4", "#008300"]
MARKERS = ["o", "s", "D", "^", "v", "P"]
INK, MUTED, GRID, SHADE = "#1f1f1f", "#6b6b6b", "#e4e4e4", "#ececec"


def style() -> None:
    plt.rcParams.update({
        "font.size": 7, "axes.titlesize": 7, "axes.labelsize": 7, "xtick.labelsize": 6.5, "ytick.labelsize": 6.5,
        "legend.fontsize": 6.5, "pdf.fonttype": 42, "ps.fonttype": 42, "axes.edgecolor": MUTED,
        "axes.labelcolor": INK, "xtick.color": MUTED, "ytick.color": MUTED, "text.color": INK,
        "axes.spines.top": False, "axes.spines.right": False, "axes.linewidth": 0.6, "xtick.major.width": 0.6,
        "ytick.major.width": 0.6, "axes.grid": True, "grid.color": GRID, "grid.linewidth": 0.5,
        "axes.axisbelow": True, "legend.frameon": False, "savefig.bbox": "tight", "savefig.pad_inches": 0.02,
    })


def load(specs: list[str]) -> list[dict]:
    runs = []
    for spec in specs:
        path, _, renames = spec.partition(":")
        mapping = dict(r.split("=", 1) for r in renames.split(",") if r)
        for comp in json.load(open(path))["comparisons"]:
            for run in comp["runs"]:
                run["system"] = mapping.get(run["system"], run["system"])
                runs.append(run)
    return runs


def systems_of(runs: list[dict]) -> list[str]:
    seen = {r["system"] for r in runs}
    extra = sorted((s for s in seen if s not in ORDER), key=lambda s: (re.sub(r"\d+", "", s), int(re.sub(r"\D", "", s) or 0)))
    return [s for s in ORDER if s in seen] + extra


def look(system: str) -> tuple[str, str, bool, str]:
    """Color, marker, filled, display name. LLM budgets (llm15, llm25, llm42, or a renamed llm-25) share the LLM's hue
    and differ by marker, the lower budgets hollow: composite encoding instead of more hues."""
    m = re.fullmatch(r"llm(deg)?-?(\d+)", system)
    if m:  # llmdegN: the same budget allowed to shed optional routes
        budget = int(m.group(2))
        marker = {15: "<", 20: ">", 25: ">", 42: "P"}.get(budget, "X")
        name = f"LLM+shed ({budget})" if m.group(1) else f"LLM ({budget})"
        return COLORS[ORDER.index("llm")], marker, budget >= 40 and not m.group(1), name
    base = system if system in ORDER else system.split("-")[0]
    i = ORDER.index(base) if base in ORDER else len(ORDER) - 1
    return COLORS[i], MARKERS[i], system in ORDER, NAMES.get(system, system)


def dash(system: str) -> str:
    """Line style: LLM budgets dotted (lowest) to solid (highest); other systems solid, renamed variants dashed."""
    m = re.fullmatch(r"llm(deg)?-?(\d+)", system)
    if m:
        return "-." if m.group(1) else {15: ":", 20: "--", 25: "--"}.get(int(m.group(2)), "-")
    return "-" if system in ORDER else "--"


def phase_label(p: dict) -> str:
    if p["kind"] == "load":  # interfere.py --load-scale default: level 1 x1.5, level 2 x2
        return "load\u00d7" + ("1.5" if p["level"] == 1 else "2")
    return p["kind"]


def pct(hist: dict, q: float, extra: tuple[int, int] = (0, 0)) -> float:
    """q-quantile in ms of a {upper bound ns: count} histogram, plus `extra` = (count, bound ns) requests."""
    items = sorted((int(b), c) for b, c in hist.items())
    if extra[0]:
        items = sorted(items + [extra[::-1]])
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
        out["sent"] += p["sent"]; out["errors"] += p["errors"]
        for b, c in p["hist"].items():
            out["hist"][b] = out["hist"].get(b, 0) + c
    return out


class Metrics:
    def __init__(self, critical: list[str], latency: str, errors_as_ms: float | None):
        self.critical, self.errors_as_ms = critical, errors_as_ms
        self.latency = latency

    def routes(self, spec: str, all_routes) -> list[str]:
        if spec == "critical":
            return self.critical
        if spec == "optional":
            return [r for r in all_routes if r not in self.critical]
        return spec.split("+")

    def part(self, routes: dict, spec: str, key: str | None = None) -> dict:
        return merged([(routes[r][key] if key else routes[r]) for r in self.routes(spec, routes) if r in routes])

    def value(self, run: dict, metric: str) -> float:
        kind, _, spec = metric.partition(":")
        if kind == "pool":
            return pool_core_s(run["pool"], run["duration_s"]) / 1000
        part = self.part(run["routes"], spec)
        if kind == "err":
            return 100 * part["errors"] / max(part["sent"], 1)
        return self.p(part, int(kind[1:]) / 100)

    def label(self, metric: str) -> str:
        kind, _, spec = metric.partition(":")
        if kind == "pool":
            return "pool (1000 core-s)"
        what = spec.replace("+", " + ")
        return f"{what} errors (%)" if kind == "err" else f"{what} {kind} (ms)"

    def p(self, part: dict, q: float = 0.95) -> float:
        extra = (part["errors"], int(self.errors_as_ms * 1e6)) if self.errors_as_ms else (0, 0)
        return pct(part["hist"], q, extra)

    def crit(self, routes: dict, key: str | None = None) -> dict:
        return merged([(routes[r][key] if key else routes[r]) for r in self.critical if r in routes])

    def opt(self, routes: dict) -> dict:
        return merged([v for r, v in routes.items() if r not in self.critical])

    def row(self, run: dict, metrics: list[str]) -> dict:
        lat, c, o = self.part(run["routes"], self.latency), self.crit(run["routes"]), self.opt(run["routes"])
        pool = run["pool"]
        return {"p95": self.p(lat), "p99": self.p(lat, 0.99), "crit_err": 100 * c["errors"] / max(c["sent"], 1),
                "opt_err": 100 * o["errors"] / max(o["sent"], 1), "pool_cs": pool_core_s(pool, run["duration_s"]),
                "pool_max": max(n for _, n in pool) if pool else float("nan"),
                **{k: self.value(run, k) for k in metrics}}


def pool_core_s(pool: list, end: float) -> float:
    """The pool's size integrated over [0, end]: what the system held, whether or not its daemons used it."""
    total = 0.0
    for i, (t, n) in enumerate(pool):
        t1 = pool[i + 1][0] if i + 1 < len(pool) else end
        lo, hi = max(t, 0.0), min(t1, end)
        if hi > lo:
            total += n * (hi - lo)
    return total


def dotstrip(ax, systems, rows, key, label, log=False) -> None:
    for y, s in enumerate(systems):
        color, marker, filled, _ = look(s)
        vals = [r[key] for r in rows[s]]
        jitter = [(i - (len(vals) - 1) / 2) * 0.035 for i in range(len(vals))]
        ax.scatter(vals, [y + j for j in jitter], s=9, marker=marker, linewidths=0.6, zorder=3,
                   facecolors=color if filled else "white", edgecolors=color, alpha=0.75)
        med = statistics.median(vals)
        ax.plot([med, med], [y - 0.32, y + 0.32], color=INK, lw=1.2, zorder=4, solid_capstyle="round")
    ax.set_xlabel(label)
    if log:
        ax.set_xscale("log")
    ax.grid(axis="y", visible=False)


def fig_summary(systems, rows, metrics, m: Metrics, out, width) -> None:
    fig, axes = plt.subplots(1, len(metrics), figsize=(width * 2.06, 0.26 * len(systems) + 0.55), sharey=True)
    for ax, metric in zip(axes, metrics):
        dotstrip(ax, systems, rows, metric, m.label(metric))
    axes[0].set_yticks(range(len(systems)), [look(s)[3] for s in systems])
    axes[0].invert_yaxis()
    for ax in axes:
        ax.set_xlim(left=0)
    fig.tight_layout(w_pad=0.8)
    fig.savefig(out)
    plt.close(fig)


def place_labels(fig, ax, points) -> None:
    """Label each point at the first of a few offsets whose text overlaps no label placed before it."""
    ax.set_xlim(left=0); ax.set_ylim(bottom=0)
    renderer = fig.canvas.get_renderer()
    r = 4 * fig.dpi / 72  # the median markers' radius, in display units
    placed = [Bbox.from_extents(px - r, py - r, px + r, py + r)
              for px, py in ax.transData.transform([(x, y) for x, y, _ in points])]
    for x, y, name in sorted(points, key=lambda p: -p[1]):
        for dx, dy, ha, va in [(5, 2, "left", "bottom"), (5, -2, "left", "top"), (-5, 2, "right", "bottom"),
                               (-5, -2, "right", "top"), (0, 6, "center", "bottom"), (0, -6, "center", "top")]:
            t = ax.annotate(name, (x, y), xytext=(dx, dy), textcoords="offset points", ha=ha, va=va, fontsize=6.5,
                            color=INK, zorder=5)
            box = t.get_window_extent(renderer).expanded(1.05, 1.1)
            if not any(box.overlaps(b) for b in placed):
                placed.append(box)
                break
            t.remove()
        else:
            ax.annotate(name, (x, y), xytext=(5, 2), textcoords="offset points", fontsize=6.5, color=INK)


def spread(ys: list[float], gap: float) -> list[float]:
    """Label positions at least `gap` apart: from the top down, a label too close to the one above moves down."""
    out = list(ys)
    prev = None
    for i in sorted(range(len(ys)), key=lambda i: -ys[i]):
        if prev is not None:
            out[i] = min(out[i], prev - gap)
        prev = out[i]
    return out


def fig_cost(systems, rows, m: Metrics, out, width) -> None:
    fig, ax = plt.subplots(figsize=(width, width * 0.72))
    medians = []
    for s in systems:
        color, marker, filled, name = look(s)
        xs, ys = [r["pool_cs"] / 1000 for r in rows[s]], [r["p95"] for r in rows[s]]
        ax.scatter(xs, ys, s=10, marker=marker, linewidths=0.6, alpha=0.45,
                   facecolors=color if filled else "white", edgecolors=color, zorder=2)
        mx, my = statistics.median(xs), statistics.median(ys)
        ax.scatter([mx], [my], s=34, marker=marker, linewidths=1.0, facecolors=color if filled else "white",
                   edgecolors=INK if filled else color, zorder=4)
        medians.append((mx, my, name))
    place_labels(fig, ax, medians)
    ax.set_xlabel("pool held (1000 core-seconds per run)")
    ax.set_ylabel(m.label(f"p95:{m.latency}"))
    ax.set_xlim(left=0); ax.set_ylim(bottom=0)
    fig.tight_layout()
    fig.savefig(out)
    plt.close(fig)


def fig_phases(systems, runs, m: Metrics, out, width) -> None:
    kinds = sorted({p["kind"] for r in runs for p in r["phases"]})
    cols = ["none"] + kinds
    fig, ax = plt.subplots(figsize=(width, 0.26 * len(systems) + 0.6))
    step = 0.8 / max(len(cols) - 1, 1)
    shapes = ["o", "s", "D", "^", "v", "P", "X"]
    for y, s in enumerate(systems):
        sr = [r for r in runs if r["system"] == s]
        for k, kind in enumerate(cols):
            if kind == "none":
                part = merged([m.part(r["routes"], m.latency, "off") for r in sr])
            else:
                part = merged([m.part(p["routes"], m.latency) for r in sr for p in r["phases"] if p["kind"] == kind])
            ax.scatter([m.p(part)], [y - 0.4 + k * step], s=12, marker=shapes[k], linewidths=0.6, zorder=3,
                       facecolors=INK if kind == "none" else "white", edgecolors=INK,
                       label=("no antagonist" if kind == "none" else kind) if y == 0 else None)
    ax.set_yticks(range(len(systems)), [look(s)[3] for s in systems])
    ax.invert_yaxis()
    ax.set_xlabel(m.label(f"p95:{m.latency}") + ", all repeats pooled")
    ax.set_xlim(left=0)
    ax.grid(axis="y", visible=False)
    ax.legend(ncol=min(len(cols), 3), loc="lower center", bbox_to_anchor=(0.45, 1.0), handletextpad=0.2,
              columnspacing=0.8)
    fig.tight_layout()
    fig.savefig(out)
    plt.close(fig)


def median_run(sr: list[dict], rows: list[dict]) -> dict:
    med = statistics.median(r["p95"] for r in rows)
    return min(zip(sr, rows), key=lambda x: abs(x[1]["p95"] - med))[0]


def fig_timeline(systems, runs, rows, m: Metrics, out, width, smooth: int) -> None:
    picks = {s: median_run([r for r in runs if r["system"] == s], rows[s]) for s in systems}
    n = len(systems)
    fig, axes = plt.subplots(n + 1, 1, figsize=(width * 2.06, 0.62 * n + 1.1), sharex=True,
                             gridspec_kw={"height_ratios": [1] * n + [1.5]})
    ymax = 0.0
    for ax, s in zip(axes, systems):
        run, (color, _, filled, name) = picks[s], look(s)
        for p in run["phases"]:
            ax.axvspan(p["on_s"], p["off_s"], color=SHADE, lw=0, zorder=0)
        bins: dict[float, list] = {}
        for route in m.routes(m.latency, run["routes"]):
            for b in run["series"].get(route, []):
                bins.setdefault(b["t_s"], []).append(b)
        ts = sorted(bins)
        ys = []
        for i in range(len(ts)):
            window = [x for t in ts[max(0, i - smooth + 1):i + 1] for x in bins[t]]
            ys.append(m.p(merged(window)))
        ax.plot(ts, ys, color=color, lw=1.0, ls=dash(s))
        ax.text(0.003, 0.92, name, transform=ax.transAxes, va="top", fontsize=6.5, color=INK,
                bbox={"facecolor": "white", "edgecolor": "none", "pad": 0.6, "alpha": 0.8})
        ymax = max(ymax, max((y for y in ys if y == y), default=0))
        ax.grid(axis="x", visible=False)
    for ax in axes[:n]:
        ax.set_ylim(0, ymax * 1.05)
    axes[n // 2].set_ylabel(m.label(f"p95:{m.latency}"))
    for p in picks[systems[0]]["phases"]:
        axes[0].text((p["on_s"] + p["off_s"]) / 2, 1.02, phase_label(p), transform=axes[0].get_xaxis_transform(),
                     ha="center", va="bottom", fontsize=5.5, color=MUTED)
    ax = axes[n]
    for p in picks[systems[0]]["phases"]:
        ax.axvspan(p["on_s"], p["off_s"], color=SHADE, lw=0, zorder=0)
    end = max(r["duration_s"] for r in picks.values())
    labelled = []
    for s in systems:
        run, (color, _, filled, name) = picks[s], look(s)
        pool = [(max(t, 0.0), c) for t, c in run["pool"] if t <= end]
        if not pool:
            continue
        xs, ys = [t for t, _ in pool] + [end], [c for _, c in pool] + [pool[-1][1]]
        if max(ys) == min(ys):
            continue  # fixed pools: one shared reference line below
        ax.step(xs, ys, where="post", color=color, lw=1.0, ls=dash(s))
        labelled.append((ys[-1], name, color))
    start = min(c for r in picks.values() for _, c in r["pool"][:1])
    fixed = [look(s)[3] for s in systems if len({c for _, c in picks[s]["pool"]}) == 1]
    if fixed:
        ax.axhline(start, color=MUTED, lw=0.8, ls=":")
        labelled.append((start, ", ".join(fixed), MUTED))
    top = max([y for y, _, _ in labelled] + [start])
    for (_, name, ink), y in zip(labelled, spread([y for y, _, _ in labelled], top * 0.16)):
        ax.text(end, y, " " + name, va="center", fontsize=6.5 if ink != MUTED else 6, color=INK if ink != MUTED else MUTED)
    ax.set_ylabel("pool (CPUs)")
    ax.set_ylim(bottom=0)
    ax.set_xlim(0, end)
    ax.set_xlabel("time (s)")
    ax.grid(axis="x", visible=False)
    fig.tight_layout(h_pad=0.3)
    fig.savefig(out)
    plt.close(fig)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("inputs", nargs="+", help="runs.json[:system=newname,...]")
    ap.add_argument("-d", "--dir", default="figs")
    ap.add_argument("--width", type=float, default=3.4, help="column width in inches")
    ap.add_argument("--critical", default="hotels,reservation")
    ap.add_argument("--summary", default="p95:hotels,err:reservation,err:optional,pool")
    ap.add_argument("--latency", default="hotels", help="routes of the cost, phases and timeline latency")
    ap.add_argument("--errors-as-ms", type=float, default=None)
    ap.add_argument("--smooth", type=int, default=3, help="timeline: bins merged per point")
    ap.add_argument("--format", default="pdf")
    ap.add_argument("--timeline-systems", default="",
                    help="systems in timeline.pdf, comma-separated (default: all; more than about 7 crowd it)")
    ap.add_argument("--max-api-errors", type=float, default=0.05,
                    help="drop llm runs where more than this share of the controller's episodes failed at the model "
                         "API (a 429 quota error leaves the system doing nothing: the fixed pool, not the llm)")
    a = ap.parse_args()
    style()
    runs = []
    for r in load(a.inputs):
        share = r.get("llm_api_errors", 0) / max(r.get("llm_episodes", 0), 1)
        if share > a.max_api_errors:
            print(f"dropped {r['system']} {r['repeat']}: {share:.0%} of llm episodes failed at the model API")
        else:
            runs.append(r)
    m = Metrics(a.critical.split(","), a.latency, a.errors_as_ms)
    metrics = a.summary.split(",")
    systems = systems_of(runs)
    rows = {s: [m.row(r, metrics) for r in runs if r["system"] == s] for s in systems}
    os.makedirs(a.dir, exist_ok=True)
    path = lambda name: os.path.join(a.dir, f"{name}.{a.format}")
    fig_summary(systems, rows, metrics, m, path("summary"), a.width)
    fig_cost(systems, rows, m, path("cost"), a.width)
    fig_phases(systems, runs, m, path("phases"), a.width)
    shown = [s for s in systems if not a.timeline_systems or s in a.timeline_systems.split(",")]
    fig_timeline(shown, runs, rows, m, path("timeline"), a.width, a.smooth)
    print(f"{'system':10} {'n':>3} {"p95":>7} {"p99":>7} {'crit err%':>9} {'opt err%':>8} {'pool max':>8} {'pool kcs':>8}")
    for s in systems:
        med = lambda k: statistics.median(r[k] for r in rows[s])
        print(f"{look(s)[3]:10} {len(rows[s]):3} {med('p95'):7.1f} {med('p99'):7.1f} {med('crit_err'):9.2f} "
              f"{med('opt_err'):8.2f} {med('pool_max'):8.0f} {med('pool_cs') / 1000:8.2f}")
    print(f"wrote {', '.join(path(n) for n in ['summary', 'cost', 'phases', 'timeline'])}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
