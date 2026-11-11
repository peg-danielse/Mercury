#!/usr/bin/env python3
"""Paper figures from extract_runs.py output (vector PDFs, column width by default).

  plot_runs.py runs.json[:llm=llm-25,...] [more.json[:...]] -d figs/ [--width 3.39|6.90] [--critical hotels,reservation]
               [--summary p95:hotels,err:reservation,err:optional,pool] [--latency hotels]

An input's optional suffix renames systems in that file, so a second comparison (say an LLM-only rerun with a smaller
budget) sits beside the first. Figures:
  summary.pdf   per system, every repeat as a dot and the median as a tick, one panel per --summary metric
                (p95:ROUTES, p99:ROUTES, err:ROUTES, pool, poolmax), small multiples on one system axis.
                pool is the pool's core-seconds; poolmax is the largest pool, in CPUs. A p99 panel is
                logarithmic, so one long tail does not pin the other systems to the axis.
  cost.pdf      daemon core-seconds against the --latency routes' p95. Each repeat is a small mark and
                the median a larger one. gemini38 and luna use the CDF's two accent styles; other
                controllers are muted; minimum and maximum are black. Labels sit off the points
  tradeoff      error bars, cdf, and pool core-seconds against p95 in one row at two-column width (6.90 in). Letters (a) (b) (c)
  phases.pdf    --latency p95 inside antagonist phases, per antagonist kind, against the windows outside phases
  timeline.pdf  the median repeat of every system: --latency p50 (dotted) and p95 per bin with the antagonist
                phases shaded, the pool's size, and allocated core-seconds cumulated over the run (double column)
  actions       actions per second for systems that called tools: each repeat a dot, the mean a dash, the
                min–max a whisker. Rate = tool events / llm_span_s (last − first llmctl event).
                One column, 3.39 in. Height follows the model count so 8 pt labels fit
  action-mix    share of each tool in a system's calls, pooled over the repeats kept by --max-api-errors
  action-bars   the same size. One bar per model, from zero, stacked by tool and normalized so the
                bar is that model's mix of calls. A repeat with no calls is left out
  p50, p95      point = median of that percentile, dots = repeats, line = min–max (not a confidence interval)
  alloc         the same marks for core-seconds allocated to the application's daemons (load generator excluded)
  pool          the same marks for the largest pool size a repeat reached, in CPUs
  errors        critical and optional error rates, each over its own routes, with the same median and min–max marks
  cdf           one full-width empirical step per system on a shared logarithmic latency axis.
                Repeats are pooled. gemini38 is a solid blue curve and gpt-luna a dashed vermilion
                curve; the others are thin gray-blue. The list at the right is the median p95
                across repeats, not a reading of the pooled curve. Failed requests are absent
  speedup       one line per system other than --baseline. Each point is one route, with the
                repeats of that route folded by the median speedup (--speedup-by route), or one
                repeat of the merged metric (--speedup-by run).
                Latency and error rate: baseline / method. Throughput: method / baseline.
                The x order is --focus, worst to best. The horizontal line is 1.
                --rename-dir BASENAME:old=new keeps a second saturate pool and a gnn directory distinct
  pctspeedup    S(p) = baseline percentile / controller percentile, p1–p99 and p99.5, p99.9.
                One row per endpoint. The right-hand panel is p90–p99.9.
                Repeats are pooled only when both sides measured that endpoint. PDF and SVG.
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
from matplotlib.lines import Line2D  # noqa: E402
from ieee_style import COLUMN, FULL, MAX_HEIGHT, snap  # noqa: E402
from matplotlib.ticker import NullFormatter  # noqa: E402
from matplotlib.patches import Rectangle  # noqa: E402
from matplotlib.transforms import Bbox  # noqa: E402

ORDER = ["saturate", "l3", "shift", "l3shift", "degrade", "llm"]
NAMES = {"saturate": "Fixed", "l3": "L3", "shift": "Shift", "l3shift": "L3+Shift", "degrade": "Degrade", "llm": "LLM"}
# Fixed-order categorical palette (validated for CVD separation); every system also has its own marker, and the
# contrast of the lighter hues obliges labels: systems are named on the axis or next to their marks.
COLORS = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4", "#008300"]
MARKERS = ["o", "s", "D", "^", "v", "P"]
INK, MUTED, GRID, SHADE = "#1a1a1a", "#5c5c5c", "#e6e6e6", "#f2f2f2"
# One hue for labeled models. Vermilion is only the fixed-pool baselines, which are not models.
PAPER, BASELINE = "#567A94", "#D55E00"
ACTION_LABEL = {
    "move_daemon": "move service", "scale_daemon": "scale service", "grow_pool": "add node",
    "swap_domain": "swap domain", "inspect": "inspect", "shrink_pool": "remove node",
    "shed_route": "shed route", "hold": "hold", "restore_route": "restore route",
    "add_replica": "add replica", "remove_replica": "remove replica",
}
# Tool order and hues of the action-mix bars. A name absent from a comparison is left out of that figure,
# so the hues stay put when a run never sheds or never inspects.
ACTIONS = ["move_daemon", "scale_daemon", "grow_pool", "swap_domain", "inspect", "shrink_pool", "shed_route", "hold", "restore_route", "add_replica", "remove_replica"]
ACTION_COLOR = {
    "move_daemon": "#2a78d6", "scale_daemon": "#5b4bb7", "grow_pool": "#eb6834", "swap_domain": "#1baf7a", "inspect": "#eda100",
    "shrink_pool": "#e87ba4", "shed_route": "#7a4ea3", "hold": "#008300", "restore_route": "#c23b3b",
    "add_replica": "#3d8b8b", "remove_replica": "#8c6d1f",
}


def style() -> str:
    """IEEE type (ieee_style.apply) plus the ink colours. Hex cannot live in the style file."""
    from ieee_style import apply
    face = apply()
    plt.rcParams.update({
        "text.color": INK, "axes.labelcolor": INK, "axes.edgecolor": "#333333",
        "xtick.color": "#333333", "ytick.color": "#333333", "grid.color": GRID,
    })
    return face


def save(fig, out) -> None:
    """Write the requested file and its twin at the figure's own size, so 8 pt stays 8 pt.
    bbox tight would crop or grow the page and change that size when the PDF is included."""
    from ieee_style import dress
    dress(fig)
    fig.savefig(out, bbox_inches=None)
    stem, ext = os.path.splitext(out)
    twin = stem + (".pdf" if ext.lower() == ".png" else ".png")
    if twin != out:
        fig.savefig(twin, bbox_inches=None)


def series_color(system: str) -> str:
    """gemini38 is blue and luna is vermilion. Other models are blue-gray.
    The fixed-pool baselines are vermilion."""
    name = model_name(system)
    if name in FOCUS:
        return FOCUS[name]["color"]
    return BASELINE if system in ("fixed8", "fixed40") else PAPER


def paper_row_height(n: int = 1) -> float:
    """Height of the tradeoff figure, in inches."""
    return max(2.70, 0.10 * max(n, 1) + 0.95) * 0.90


def action_figsize() -> tuple[float, float]:
    """Action figures: shorter than the tradeoff row, and a little wider than that square was."""
    side = paper_row_height()
    return side * 1.23, side * 0.86


def quant_grid(ax) -> None:
    ax.grid(axis="x", visible=True, color=GRID, linewidth=0.4)
    ax.grid(axis="y", visible=False)


# Shared encoding for the latency CDF and the resource–latency scatter only.
# Okabe–Ito blue and vermilion, split by line style and marker so the two curves
# stay distinct where they overlap and in grayscale. Other controllers are one
# muted gray-blue. Fixed pools are black, distinguished by marker.
FOCUS = {
    "gemini38": {"color": "#0072B2", "ls": "-", "marker": "o"},
    "luna": {"color": "#D55E00", "ls": (0, (3.0, 1.3)), "marker": "^"},
}
# Printed names. The run id stays llm25-luna.
DISPLAY = {"luna": "gpt-luna"}
OTHER_COLOR = "#7E8E9C"
FIXED_COLOR = "#1a1a1a"
# The non-LLM baselines on the CDF and the cost scatter. Markers and dashes keep them apart in grayscale.
POLICY = {
    "l3": {"color": "#009E73", "ls": "-", "marker": "v", "label": "L3"},
    "shift": {"color": "#56B4E9", "ls": "--", "marker": "P", "label": "Shift"},
    "l3shift": {"color": "#882255", "ls": "-.", "marker": "X", "label": "Shift+L3"},
    "degrade": {"color": "#CC79A7", "ls": (0, (1.4, 1.0)), "marker": "h", "label": "Degrade"},
}


def model_name(system: str) -> str:
    m = re.fullmatch(r"llm(?:deg)?\d+-(.*)", system)
    if m:
        return m.group(1)
    if system == "fixed8":
        return "minimum"
    if system == "fixed40":
        return "maximum"
    return look(system)[3]


# Set by fig_tradeoff so that figure alone draws each system in a tab20 colour.
_PALETTE: dict[str, object] | None = None


def tab20_colors(systems, rows) -> dict[str, object]:
    """One tab20 colour per model, in the error-bar order (smallest median critical error first).
    minimum and maximum stay black, so they are left out of the palette. The darker colour of
    each pair comes first, so neighbouring bars are different hues."""
    ranked = sorted((s for s in systems if s not in ("fixed8", "fixed40")),
                    key=lambda s: statistics.median(r["crit_err"] for r in rows[s]))
    cmap = plt.get_cmap("tab20").colors
    palette = [cmap[i] for i in range(0, 20, 2)] + [cmap[i] for i in range(1, 20, 2)]
    return {s: palette[i % len(palette)] for i, s in enumerate(ranked)}


def mark_of(system: str) -> dict:
    """Color, line style, marker, and label for the CDF and the cost scatter."""
    st = _mark_of(system)
    if _PALETTE is not None and system in _PALETTE:
        return {**st, "color": _PALETTE[system]}
    return st


def _mark_of(system: str) -> dict:
    name = model_name(system)
    if name in FOCUS:
        return {**FOCUS[name], "label": DISPLAY.get(name, name), "filled": True, "lw": 1.6, "focus": True, "z": 5}
    if system == "fixed8":
        return {"color": FIXED_COLOR, "ls": "-", "marker": "D", "label": name, "filled": True, "lw": 1.0, "focus": False, "z": 4, "named": True}
    if system == "fixed40":
        return {"color": FIXED_COLOR, "ls": "-", "marker": "s", "label": name, "filled": False, "lw": 1.0, "focus": False, "z": 4, "named": True}
    if system in POLICY:
        return {**POLICY[system], "filled": True, "lw": 1.0, "focus": False, "z": 3, "named": True}
    if system.startswith("gnn-") and system.removeprefix("gnn-") in POLICY:
        base = POLICY[system.removeprefix("gnn-")]
        return {**base, "ls": ":", "filled": False, "label": "FIRM " + base["label"], "lw": 1.0, "focus": False, "z": 3, "named": True}
    return {"color": OTHER_COLOR, "ls": "-", "marker": "o", "label": name, "filled": True, "lw": 1.0, "focus": False, "z": 2}


def load(specs: list[str], dir_renames: dict[str, dict[str, str]] | None = None) -> list[dict]:
    """File suffixes rename systems in that file. dir_renames maps a comparison directory's
    basename to a second rename, so two saturate pools in one JSON stay distinct."""
    dir_renames = dir_renames or {}
    runs = []
    for spec in specs:
        path, _, renames = spec.partition(":")
        mapping = dict(r.split("=", 1) for r in renames.split(",") if r)
        for comp in json.load(open(path))["comparisons"]:
            by_dir = dir_renames.get(os.path.basename(comp["dir"].rstrip("/")), {})
            for run in comp["runs"]:
                system = by_dir.get(run["system"], run["system"])
                run["system"] = mapping.get(system, system)
                runs.append(run)
    return runs


# A gnn- controller sits beside the saturate run of the same controller.
EVAL_ORDER = ["fixed8", "fixed40", "shift", "gnn-shift", "l3", "gnn-l3", "l3shift", "gnn-l3shift",
              "degrade", "gnn-degrade", "llm40-luna"]


def systems_of(runs: list[dict]) -> list[str]:
    seen = {r["system"] for r in runs}
    if any(s.startswith("gnn-") for s in seen):
        return [s for s in EVAL_ORDER if s in seen] + sorted(seen - set(EVAL_ORDER))
    extra = sorted((s for s in seen if s not in ORDER), key=lambda s: (re.sub(r"\d+", "", s), int(re.sub(r"\D", "", s) or 0)))
    return [s for s in ORDER if s in seen] + extra


# Named models (llm25-luna) drawn together. A budget alone (llm15) keeps the shared LLM hue below.
_NAMED: dict[str, tuple[str, str, str]] = {}
_NAMED_MARKS = ["o", "s", "D", "^", "v", "P", "X", "h", "p", "<", ">", "8", "*"]
_NAMED_DASH = ["-", "--", "-.", ":"]


def assign_looks(systems: list[str]) -> None:
    """Give each llm<budget>-<model> its own marker, dash and a cycled hue, and the model name as its label.
    Called once per figure set. Budget-only names are left to look()."""
    _NAMED.clear()
    named = [s for s in systems if re.fullmatch(r"llm(?:deg)?\d+-.+", s)]
    for i, system in enumerate(named):
        _NAMED[system] = (COLORS[i % len(COLORS)], _NAMED_MARKS[i % len(_NAMED_MARKS)], _NAMED_DASH[(i // len(COLORS)) % len(_NAMED_DASH)])


def short_llm(system: str) -> str:
    """llm25-luna -> gpt-luna. A bare budget (llm15) keeps the paper name."""
    m = re.fullmatch(r"llm(?:deg)?(\d+)-(.*)", system)
    return DISPLAY.get(m.group(2), m.group(2)) if m else look(system)[3]


def look(system: str) -> tuple[str, str, bool, str]:
    """Color, marker, filled, display name. LLM budgets (llm15, llm25, llm42, or a renamed llm-25) share the LLM's hue
    and differ by marker, the lower budgets hollow: composite encoding instead of more hues."""
    if system in _NAMED:
        color, marker, _ls = _NAMED[system]
        return color, marker, True, short_llm(system)
    if system == "fixed8":  # saturate on a pool of 8, renamed so it can sit beside Fixed 40
        return COLORS[ORDER.index("saturate")], "o", True, "minimum"
    if system == "fixed40":  # saturate on a pool of 40: the overprovisioned twin of Fixed, hollow so the two stay distinct
        return COLORS[ORDER.index("saturate")], "s", False, "maximum"
    if system.startswith("gnn-") and system.removeprefix("gnn-") in ORDER:
        base = system.removeprefix("gnn-")
        i = ORDER.index(base)
        return COLORS[i], MARKERS[i], False, "FIRM " + NAMES[base]
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
    if system in _NAMED:
        return _NAMED[system][2]
    m = re.fullmatch(r"llm(deg)?-?(\d+)", system)
    if m:
        return "-." if m.group(1) else {15: ":", 20: "--", 25: "--"}.get(int(m.group(2)), "-")
    if system.startswith("gnn-") and system.removeprefix("gnn-") in ORDER:
        return ":"
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
        if kind == "poolmax":
            return max((n for _, n in run["pool"]), default=float("nan"))
        if kind == "cores":  # mean cores allocated to the application's daemons over the measured window
            return run["cpu"]["alloc_core_s"] / run["cpu"]["window_s"]
        if kind == "tput":  # requests sent per second, all routes (errors included)
            return sum(r["sent"] for r in run["routes"].values()) / run["duration_s"]
        part = self.part(run["routes"], spec)
        if kind == "err":
            return 100 * part["errors"] / max(part["sent"], 1)
        return self.p(part, int(kind[1:]) / 100)

    def label(self, metric: str) -> str:
        kind, _, spec = metric.partition(":")
        if kind == "pool":
            return "pool (1000 core-s)"
        if kind == "poolmax":
            return "pool (CPUs)"
        if kind == "cores":
            return "cpuset cores (sum)"
        if kind == "tput":
            return "throughput (req/s)"
        what = spec.replace("+", " + ")
        return f"{what} errors (\\%)" if kind == "err" else f"{what} {kind} (ms)"

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
        return {"p50": self.p(lat, 0.5), "p95": self.p(lat), "p99": self.p(lat, 0.99),
                "crit_err": 100 * c["errors"] / max(c["sent"], 1),
                "opt_err": 100 * o["errors"] / max(o["sent"], 1), "alloc_cs": run["cpu"]["alloc_core_s"],  # core-seconds allocated to the daemons (load generator excluded)
                "pool_cs": pool_core_s(pool, run["duration_s"]),  # pool size integrated over the run
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
    fig, axes = plt.subplots(1, len(metrics), figsize=(FULL, 0.42 * len(systems) + 0.85), sharey=True)
    if len(metrics) == 1:
        axes = [axes]
    for ax, metric in zip(axes, metrics):
        kind = metric.split(":", 1)[0]
        dotstrip(ax, systems, rows, metric, m.label(metric), log=(kind == "p99"))
        if kind != "p99":
            ax.set_xlim(left=0)
    axes[0].set_yticks(range(len(systems)), [look(s)[3] for s in systems])
    axes[0].invert_yaxis()
    fig.tight_layout(w_pad=0.8)
    save(fig, out)
    plt.close(fig)


def place_labels(fig, ax, points) -> None:
    """Label each point at the first of a few offsets whose text overlaps no label placed before it.
    The axes limits must already be the ones the figure will save."""
    renderer = fig.canvas.get_renderer()
    r = 4 * fig.dpi / 72  # the median markers' radius, in display units
    placed = [Bbox.from_extents(px - r, py - r, px + r, py + r)
              for px, py in ax.transData.transform([(x, y) for x, y, _ in points])]
    for x, y, name in sorted(points, key=lambda p: -p[1]):
        for dx, dy, ha, va in [(6, 3, "left", "bottom"), (6, -3, "left", "top"), (-6, 3, "right", "bottom"),
                               (-6, -3, "right", "top"), (0, 8, "center", "bottom"), (0, -8, "center", "top"),
                               (14, 10, "left", "bottom"), (14, -10, "left", "top"), (-14, 10, "right", "bottom"),
                               (-14, -10, "right", "top")]:
            t = ax.annotate(name, (x, y), xytext=(dx, dy), textcoords="offset points", ha=ha, va=va, fontsize=8,
                            color=INK, zorder=5)
            box = t.get_window_extent(renderer).expanded(1.05, 1.1)
            if not any(box.overlaps(b) for b in placed):
                placed.append(box)
                break
            t.remove()
        else:
            ax.annotate(name, (x, y), xytext=(5, 2), textcoords="offset points", fontsize=8, color=INK)


def spread(ys: list[float], gap: float) -> list[float]:
    """Label positions at least `gap` apart: from the top down, a label too close to the one above moves down."""
    out = list(ys)
    prev = None
    for i in sorted(range(len(ys)), key=lambda i: -ys[i]):
        if prev is not None:
            out[i] = min(out[i], prev - gap)
        prev = out[i]
    return out


def _face(st: dict, hollow_fill: str):
    return st["color"] if st["filled"] else hollow_fill


def draw_cost(ax, systems, rows, zoom_x: bool, xkey: str = "alloc_cs") -> list[dict]:
    """Each repeat is a small mark. A larger mark is the median of those repeats.
    A system with one repeat is drawn once, as that median. Returns the medians.
    `alloc_cs` is core-seconds given to the daemons. `pool_cs` is the pool's size integrated over the run."""
    medians, all_x, all_y = [], [], []
    ordered = sorted(systems, key=lambda s: mark_of(s)["z"])
    for s in ordered:
        st = mark_of(s)
        xs = [r[xkey] / 1000 for r in rows[s]]
        ys = [r["p95"] for r in rows[s]]
        all_x.extend(xs)
        all_y.extend(ys)
        edge = INK if st["focus"] else st["color"]
        if len(xs) > 1:
            ax.scatter(xs, ys, s=16 if st["focus"] else 14, marker=st["marker"],
                       linewidths=0.8 if st["focus"] else 0.45, alpha=0.55 if st["focus"] else 0.45, zorder=3 if st["focus"] else 2,
                       facecolors=_face(st, "none"), edgecolors=edge)
        mx, my = statistics.median(xs), statistics.median(ys)
        ax.scatter([mx], [my], s=52 if st["focus"] else 34, marker=st["marker"],
                   linewidths=1.0 if st["focus"] else 0.85,
                   facecolors=_face(st, "white"), edgecolors=edge, zorder=st["z"])
        medians.append({"x": mx, "y": my, "s": s, "st": st, "n": len(xs)})
    ax.set_ylim(0, (max(all_y) * 1.06) if all_y else 1)
    if zoom_x and all_x:
        span = max(all_x) - min(all_x) or 1.0
        ax.set_xlim(min(all_x) - 0.05 * span, max(all_x) + 0.06 * span)
    else:
        ax.set_xlim(left=0)
    ax.grid(axis="y", visible=True, color=GRID, linewidth=0.4)
    ax.grid(axis="x", visible=False)
    ax.set_axisbelow(True)
    ax.set_xlabel("Pool (1000 core-s)" if xkey == "pool_cs" else "Allocated CPU (1000 core-s)")
    ax.set_ylabel("p95 request latency (ms)")
    if any(p["n"] > 1 for p in medians):
        ax.legend(handles=[
            Line2D([0], [0], marker="o", color=OTHER_COLOR, lw=0, markersize=3.5, alpha=0.55, label="one repeat"),
            Line2D([0], [0], marker="o", color=OTHER_COLOR, lw=0, markersize=6.5, label="median"),
        ], loc="upper right", handletextpad=0.3, borderaxespad=0.2)
    return medians


def _outside(fig, ax, dx_in):
    """Axes coordinates shifted dx_in inches past the right spine."""
    from matplotlib.transforms import ScaledTranslation
    return ax.transAxes + ScaledTranslation(dx_in, 0, fig.dpi_scale_trans)


def _margin_data(fig, ax, dx_in):
    """x is inches past the right spine; y is data."""
    from matplotlib.transforms import blended_transform_factory
    return blended_transform_factory(_outside(fig, ax, dx_in), ax.transData)


def _text_box(fig, artist):
    fig.canvas.draw()
    return artist.get_window_extent(fig.canvas.get_renderer()).expanded(1.04, 1.2)


def place_cost_labels(fig, ax, medians: list[dict]) -> None:
    """Name a median only when the name fits beside the point. No leader lines.
    gemini38, luna, Fixed 8, and Fixed 40 are placed even when the slot is crowded.
    The other names in that cluster are left off."""
    fig.canvas.draw()
    ax_bb = ax.get_window_extent(fig.canvas.get_renderer())
    for p in medians:
        p["px"], p["py"] = ax.transData.transform((p["x"], p["y"]))
    obstacles = []
    r = 8 * fig.dpi / 72
    for p in medians:
        obstacles.append(Bbox.from_extents(p["px"] - r, p["py"] - r, p["px"] + r, p["py"] + r))
    legend = ax.get_legend()
    if legend is not None:
        obstacles.append(legend.get_window_extent(fig.canvas.get_renderer()).expanded(1.05, 1.15))
    # Opposite sides, so the two highlighted names do not sit on each other.
    focus_at = {
        "gemini38": [(-2, 18, "center", "bottom"), (0, 22, "center", "bottom"), (-10, 18, "right", "bottom")],
        "luna": [(-6, -8, "right", "top"), (-12, -4, "right", "center"), (0, -12, "center", "top")],
    }
    pinned = {
        "fixed8": [(14, -2, "left", "center"), (16, -12, "left", "top"), (10, -16, "left", "top")],
        "fixed40": [(-12, 6, "right", "center"), (-14, 12, "right", "bottom"), (-10, -10, "right", "top")],
        "l3": [(10, 8, "left", "bottom"), (-10, 8, "right", "bottom"), (0, 14, "center", "bottom")],
        "shift": [(10, -8, "left", "top"), (-10, 8, "right", "bottom"), (12, 4, "left", "center")],
        "l3shift": [(-10, 8, "right", "bottom"), (10, 8, "left", "bottom"), (0, -12, "center", "top")],
        "degrade": [(10, 4, "left", "bottom"), (-12, -6, "right", "top"), (0, 14, "center", "bottom")],
    }
    near = [(8, 2, "left", "bottom"), (-8, 2, "right", "bottom"), (8, -2, "left", "top"),
            (-8, -2, "right", "top"), (0, 10, "center", "bottom"), (0, -10, "center", "top")]
    order = sorted(medians, key=lambda p: (not p["st"]["focus"], p["s"] not in ("fixed8", "fixed40"), -p["y"]))
    for p in order:
        color = p["st"]["color"] if p["st"]["focus"] or p["st"].get("named") else INK
        offsets = pinned.get(p["s"], focus_at.get(model_name(p["s"]), focus_at.get(p["st"]["label"], near)))
        for dx, dy, ha, va in offsets:
            ann = ax.annotate(p["st"]["label"], (p["x"], p["y"]), xytext=(dx, dy), textcoords="offset points",
                              ha=ha, va=va, fontsize=plt.rcParams["font.size"], color=color, zorder=6, annotation_clip=False)
            box = _text_box(fig, ann)
            inside = ax_bb.x0 <= box.x0 and box.x1 <= ax_bb.x1 and ax_bb.y0 <= box.y0 and box.y1 <= ax_bb.y1
            if inside and not any(box.overlaps(b) for b in obstacles):
                obstacles.append(box)
                break
            ann.remove()
        else:
            # Every free slot was taken. Keep the highlighted name on its first offset anyway.
            if p["st"]["focus"] or p["s"] in pinned:
                dx, dy, ha, va = offsets[0]
                ax.annotate(p["st"]["label"], (p["x"], p["y"]), xytext=(dx, dy), textcoords="offset points",
                            ha=ha, va=va, fontsize=plt.rcParams["font.size"], color=color, zorder=6, annotation_clip=False)


def fig_cost(systems, rows, m: Metrics, out, width, zoom_x: bool = False) -> None:
    fig, ax = plt.subplots(figsize=(width, width))
    # Equal margins on a square page, so the axes box is square too.
    fig.subplots_adjust(left=0.13, right=0.97, bottom=0.10, top=0.94)
    medians = draw_cost(ax, systems, rows, zoom_x)
    place_cost_labels(fig, ax, medians)
    save(fig, out)
    plt.close(fig)


def _cdf(hist: dict) -> tuple[list[float], list[float]]:
    """Latency (ms) and the fraction of histogram requests at or below it. Upper-bound buckets."""
    items = sorted((int(b), c) for b, c in hist.items() if c)
    total = sum(c for _, c in items)
    acc, xs, ys = 0, [], []
    for bound, count in items:
        acc += count
        xs.append(bound / 1e6)
        ys.append(acc / total if total else 0.0)
    return xs, ys


def _latency_curves(systems, runs, m: Metrics) -> list[tuple]:
    """Pooled empirical CDF and the median, across repeats, of that route's p95.
    The listed p95 is not the latency where the pooled curve crosses 95%."""
    curves = []
    for s in systems:
        own = [r for r in runs if r["system"] == s]
        hist = merged(m.part(r["routes"], m.latency) for r in own)["hist"]
        xs, ys = _cdf(hist)
        if not xs:
            continue
        p95 = statistics.median(m.p(m.part(r["routes"], m.latency)) for r in own)
        curves.append((s, xs, ys, p95))
    curves.sort(key=lambda c: (c[3], model_name(c[0])))
    return curves


def draw_cdf(ax, curves, fraction: bool = False, guide: float = 0.95) -> None:
    """Empirical steps. Gray controllers first, then named baselines such as maximum,
    then gemini38 solid, then luna dashed on top so a shared stretch stays visible.
    `fraction` labels the axis 0, 0.5, 1. Otherwise the ticks are percentages.
    `guide` is the horizontal reference, 0.95 by default and 0.9 on the tradeoff."""
    from matplotlib.ticker import NullFormatter, PercentFormatter

    rest = [c for c in curves if not mark_of(c[0])["focus"]]
    plain = [c for c in rest if not mark_of(c[0]).get("named")]
    named = [c for c in rest if mark_of(c[0]).get("named")]
    focus = [c for c in curves if mark_of(c[0])["focus"]]
    focus.sort(key=lambda c: mark_of(c[0])["ls"] != "-")  # solid under the dash
    for s, xs, ys, _ in plain:
        st = mark_of(s)
        ax.step(xs, ys, where="post", color=st["color"], lw=st["lw"], ls=st["ls"], zorder=2)
    for s, xs, ys, _ in named:
        st = mark_of(s)
        # minimum is dotted so its slower curve stays distinct from maximum's solid step.
        ls = ":" if s == "fixed8" else st["ls"]
        ax.step(xs, ys, where="post", color=st["color"], lw=st["lw"], ls=ls, zorder=3)
    for s, xs, ys, _ in focus:
        st = mark_of(s)
        ax.step(xs, ys, where="post", color=st["color"], lw=st["lw"], ls=st["ls"], zorder=4)
    xs_all = [x for _, xs, _, _ in curves for x in xs]
    ax.axhline(guide, color="#c8c8c8", lw=0.5, zorder=1)
    ax.set_xscale("log")
    if xs_all:
        ax.set_xlim(min(xs_all) * 0.85, max(xs_all) * 1.12)
    ax.set_ylim(0, 1.0)
    ax.set_yticks([0, 0.5, 1])
    if fraction:
        ax.yaxis.set_major_formatter(plt.FuncFormatter(lambda v, _p: f"{v:g}"))
        ax.tick_params(axis="y", pad=1)
    else:
        ax.yaxis.set_major_formatter(PercentFormatter(xmax=1, decimals=0))
    ax.xaxis.set_major_formatter(plt.FuncFormatter(lambda v, _p: f"{v:.0f}" if v >= 1 else f"{v:g}"))
    ax.xaxis.set_minor_formatter(NullFormatter())
    ax.set_xlabel("Request latency (ms)")
    ax.set_ylabel("Cumulative fraction of requests")
    ax.grid(axis="y", visible=True, color=GRID, linewidth=0.4)
    ax.grid(axis="x", visible=False)
    ax.set_axisbelow(True)
    guide_label = f"{guide:g}" if fraction else f"{guide * 100:.0f}\\%"
    ax.text(0.012, guide, guide_label, transform=ax.get_yaxis_transform(), ha="left", va="bottom",
            fontsize=plt.rcParams["font.size"], color=MUTED, clip_on=False)


def cdf_names_inside(ax, curves) -> None:
    """Named curves inside the CDF, lower right. Unmarked gray curves are not labelled."""
    curves = [c for c in curves if mark_of(c[0])["focus"] or mark_of(c[0]).get("named")]
    n = len(curves)
    if not n:
        return
    cols = 2 if n > 4 else 1
    rows = (n + cols - 1) // cols
    # Lower right: the curves have already risen, so the names sit under them.
    dy = min(0.09, 0.42 / rows)
    x0 = 0.62 if n <= 4 else 0.48
    for i, (s, *_) in enumerate(curves):
        col, row = divmod(i, rows)
        y = 0.04 + (rows - 1 - row) * dy
        x = x0 + col * 0.28
        st = mark_of(s)
        color = st["color"] if st["focus"] or _PALETTE is not None else INK
        ls = ":" if s == "fixed8" else st["ls"]
        ax.plot([x, x + 0.045], [y, y], transform=ax.transAxes, color=st["color"],
                ls=ls, lw=st["lw"], clip_on=True, solid_capstyle="butt")
        ax.text(x + 0.055, y, st["label"], transform=ax.transAxes,
                ha="left", va="center", fontsize=plt.rcParams["font.size"], color=color)


def cdf_key(fig, ax, curves, value_in: float = 1.72) -> None:
    """Ranked list in the right margin. The number is the median p95 across repeats.
    `value_in` is how far, in inches, the p95 column sits past the right spine."""
    n = len(curves)
    if not n:
        return
    name_at = _outside(fig, ax, 0.28)
    value_at = _outside(fig, ax, value_in)
    ax.text(1.0, 1.0, "median p95", transform=_outside(fig, ax, value_in), ha="right", va="bottom",
            fontsize=8, color=MUTED, clip_on=False)
    for i, (s, _, _, p95) in enumerate(curves):
        st = mark_of(s)
        y = 1 - (i + 0.5) / n
        color = st["color"] if st["focus"] else INK
        if st["focus"]:
            ax.annotate("", xy=(1.0, y), xytext=(1.0, y),
                        xycoords=_outside(fig, ax, 0.04), textcoords=_outside(fig, ax, 0.22),
                        arrowprops={"arrowstyle": "-", "color": st["color"], "lw": st["lw"], "linestyle": st["ls"]},
                        annotation_clip=False)
        ax.text(1.0, y, st["label"], transform=name_at, ha="left", va="center", fontsize=8,
                color=color, clip_on=False)
        ax.text(1.0, y, f"{p95:.0f} ms", transform=value_at, ha="right", va="center", fontsize=8,
                color=color, clip_on=False)


def fig_cdf(systems, runs, m: Metrics, out, width) -> None:
    """One full-width ECDF. `width` is the figure width in inches."""
    curves = _latency_curves(systems, runs, m)
    fig, ax = plt.subplots(figsize=(width, max(3.05, 0.17 * len(curves) + 1.15)))
    fig.subplots_adjust(left=0.10, right=0.68, bottom=0.16, top=0.93)
    draw_cdf(ax, curves)
    cdf_key(fig, ax, curves)
    save(fig, out)
    plt.close(fig)


def draw_error_bars(ax, systems, rows) -> None:
    """Thin horizontal bars. Length is the median critical error rate across repeats.
    Optional routes are not drawn. Smallest median at the top. On the tradeoff (`_PALETTE`)
    each model bar and its name use that model's tab20 colour. minimum (`fixed8`) is filled
    black and maximum (`fixed40`) is white with a black outline. Without the palette the
    marked models keep their colours and every other bar is blue-gray."""
    ranked = sorted(systems, key=lambda s: statistics.median(r["crit_err"] for r in rows[s]))
    for y, system in enumerate(ranked):
        med = statistics.median(r["crit_err"] for r in rows[system])
        st = mark_of(system)
        if system == "fixed40":
            ax.barh(y, med, height=0.42, facecolor="white", edgecolor=FIXED_COLOR, linewidth=1.0, zorder=3)
            continue
        color = st["color"] if _PALETTE is not None or st["focus"] or system == "fixed8" else PAPER
        ax.barh(y, med, height=0.42, color=color, linewidth=0, zorder=3)
    ax.set_yticks(range(len(ranked)), [mark_of(s)["label"] for s in ranked])
    for tick, system in zip(ax.get_yticklabels(), ranked):
        st = mark_of(system)
        if _PALETTE is not None or st["focus"]:
            tick.set_color(st["color"])
    ax.set_ylim(-0.55, len(ranked) - 0.45)
    ax.invert_yaxis()
    ax.set_xlim(left=0)
    ax.set_xlabel(r"Critical error rate (\%)")
    quant_grid(ax)


def fig_tradeoff(systems, runs, rows, m: Metrics, out, width, zoom_x: bool) -> None:
    """(a) error bars, (b) the latency CDF, (c) the resource–latency scatter, in one row.
    The canvas is two columns, 6.90 in. Type is 7 pt. Of the width left after the axis
    labels, the error bars take 30%, the CDF 40%, and the cost scatter 30%. The page is
    20% taller than 0.092 in per system plus the margins; that extra height is in the
    panels. Each model is one tab20 colour. minimum is filled black and maximum is a
    black outline. Named curves are labelled inside the CDF.
    The CDF axis is 0, 0.5, and 1. minimum is a dotted black step on the CDF.
    `width` is unused: this figure is always the double-column measure."""
    del width
    global _PALETTE
    curves = _latency_curves(systems, runs, m)
    n = max(len(systems), 1)
    # 7 pt on this figure only, so the names can sit closer than the 8 pt row.
    row, bottom, top = 0.092, 0.46, 0.12
    ax_h = max(1.15, row * n)
    height = min(8.0, ax_h + bottom + top)
    # 20% taller page. The 0.46 in and 0.12 in margins stay; the panels take the extra.
    height = min(8.0, height * 1.2)
    ax_h = height - bottom - top
    err_label, err_gap = 0.64, 0.06
    cdf_label, gap, cost_label, right = 0.46, 0.08, 0.40, 0.05
    # Of the width left after the axis labels: error bars 30%, CDF 40%, cost scatter 30%.
    plot = FULL - (err_label + err_gap + cdf_label + gap + cost_label + right)
    err_w, cdf_w, cost_w = plot * 0.30, plot * 0.40, plot * 0.30
    total = FULL
    sizes = ("font.size", "axes.labelsize", "axes.titlesize", "xtick.labelsize", "ytick.labelsize", "legend.fontsize")
    saved = {k: plt.rcParams[k] for k in sizes}
    plt.rcParams.update({k: 7 for k in sizes})
    _PALETTE = tab20_colors(systems, rows)
    try:
        fig = plt.figure(figsize=(total, height))
        y0 = bottom / height
        h = ax_h / height
        x_err = err_label
        ax_e = fig.add_axes([x_err / total, y0, err_w / total, h])
        x_cdf = x_err + err_w + err_gap + cdf_label
        ax0 = fig.add_axes([x_cdf / total, y0, cdf_w / total, h])
        x_cost = x_cdf + cdf_w + gap + cost_label
        ax1 = fig.add_axes([x_cost / total, y0, cost_w / total, h])
        draw_error_bars(ax_e, systems, rows)
        draw_cdf(ax0, curves, fraction=True, guide=0.9)
        cdf_names_inside(ax0, curves)
        medians = draw_cost(ax1, systems, rows, zoom_x, xkey="pool_cs")
        place_cost_labels(fig, ax1, medians)
        for ax, letter in ((ax_e, "(a)"), (ax0, "(b)"), (ax1, "(c)")):
            ax.text(0.0, 1.02, letter, transform=ax.transAxes, ha="left", va="bottom",
                    fontsize=plt.rcParams["font.size"], color=INK, clip_on=False)
        save(fig, out)
        plt.close(fig)
    finally:
        _PALETTE = None
        plt.rcParams.update(saved)


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
    have_alloc = any(picks[s].get("alloc") for s in systems)
    tail = 2 if have_alloc else 1
    fig, axes = plt.subplots(n + tail, 1, figsize=(FULL, 1.22 * n + 1.3 * tail + 1.0), sharex=True,
                             gridspec_kw={"height_ratios": [1] * n + [1.5] * tail})
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
        p50, p95 = [], []
        for i in range(len(ts)):
            window = [x for t in ts[max(0, i - smooth + 1):i + 1] for x in bins[t]]
            part = merged(window)
            p50.append(m.p(part, 0.50))
            p95.append(m.p(part))
        ax.plot(ts, p95, color=color, lw=1.0, ls=dash(s))
        ax.plot(ts, p50, color=MUTED, lw=1.0, ls=":")
        ax.text(0.003, 0.92, name, transform=ax.transAxes, va="top", fontsize=8, color=INK,
                bbox={"facecolor": "white", "edgecolor": "none", "pad": 0.6, "alpha": 0.8})
        ymax = max(ymax, max((y for y in p95 if y == y), default=0))
        ax.grid(axis="x", visible=False)
    for ax in axes[:n]:
        ax.set_ylim(0, ymax * 1.05)
    axes[0].legend(handles=[
        Line2D([0], [0], color=INK, lw=1.0, label="p95"),
        Line2D([0], [0], color=MUTED, lw=1.0, ls=":", label="p50"),
    ], loc="upper right", fontsize=8, handlelength=1.4, borderpad=0.2)
    axes[n // 2].set_ylabel(f"{m.latency.replace('+', ' + ')} (ms)")
    for p in picks[systems[0]]["phases"]:
        axes[0].text((p["on_s"] + p["off_s"]) / 2, 1.02, phase_label(p), transform=axes[0].get_xaxis_transform(),
                     ha="center", va="bottom", fontsize=8, color=MUTED)
    ax = axes[n]
    for p in picks[systems[0]]["phases"]:
        ax.axvspan(p["on_s"], p["off_s"], color=SHADE, lw=0, zorder=0)
    end = max(r["duration_s"] for r in picks.values())
    labelled = []
    handles = []
    for s in systems:
        run, (color, _, filled, name) = picks[s], look(s)
        pool = [(max(t, 0.0), c) for t, c in run["pool"] if t <= end]
        if not pool:
            continue
        xs, ys = [t for t, _ in pool] + [end], [c for _, c in pool] + [pool[-1][1]]
        if max(ys) == min(ys):
            continue  # constant pools are dotted references, grouped by size below
        (line,) = ax.step(xs, ys, where="post", color=color, lw=1.0, ls=dash(s))
        handles.append(line)
        labelled.append((ys[-1], name, color))
    # A pool that never changes is a dotted reference. Different constants (Fixed at 8, Fixed 40) each get a line;
    # one shared line would draw the larger pool at the smaller size.
    fixed_at: dict[float, list[str]] = {}
    for s in systems:
        sizes = {c for _, c in picks[s].get("pool") or []}
        if len(sizes) == 1:
            fixed_at.setdefault(next(iter(sizes)), []).append(look(s)[3])
    for cores, names in sorted(fixed_at.items()):
        ref = ax.axhline(cores, color=MUTED, lw=1.0, ls=":")
        handles.append(ref)
        label = ", ".join(names) + (f" ({cores:g})" if len(fixed_at) > 1 else "")
        labelled.append((cores, label, MUTED))
    # End-of-line names collide once more than a handful of pools finish at the same size.
    if len(handles) > 6:
        ax.legend(handles=handles, labels=[name for _, name, _ in labelled], loc="upper left",
                  bbox_to_anchor=(1.005, 1), borderaxespad=0, fontsize=8, handlelength=1.6, labelspacing=0.25)
    else:
        top = max((y for y, _, _ in labelled), default=0)
        for (_, name, ink), y in zip(labelled, spread([y for y, _, _ in labelled], top * 0.16)):
            ax.text(end, y, " " + name, va="center", fontsize=8,
                    color=INK if ink != MUTED else MUTED)
    ax.set_ylabel("pool (CPUs)")
    ax.set_ylim(bottom=0)
    ax.set_xlim(0, end)
    ax.grid(axis="x", visible=False)
    if have_alloc:
        ax.set_xlabel("")
        ax = axes[n + 1]
        for p in picks[systems[0]]["phases"]:
            ax.axvspan(p["on_s"], p["off_s"], color=SHADE, lw=0, zorder=0)
        cost_handles, cost_names = [], []
        for s in systems:
            run, (color, _, filled, name) = picks[s], look(s)
            steps = run.get("alloc") or []
            if not steps:
                continue
            acc, xs, ys = 0.0, [], []
            for step in steps:
                if step["t_s"] > end:
                    break
                acc += step["core_s"]
                xs.append(step["t_s"])
                ys.append(acc / 1000)
            if not xs:
                continue
            (line,) = ax.plot(xs, ys, color=color, lw=1.0, ls=dash(s))
            cost_handles.append(line)
            cost_names.append(name)
        if len(cost_handles) > 6:
            ax.legend(handles=cost_handles, labels=cost_names, loc="upper left", bbox_to_anchor=(1.005, 1),
                      borderaxespad=0, fontsize=8, handlelength=1.6, labelspacing=0.25)
        ax.set_ylabel("allocated (1000 core-s)")
        ax.set_ylim(bottom=0)
        ax.grid(axis="x", visible=False)
    ax.set_xlabel("time (s)")
    fig.tight_layout(h_pad=0.35, rect=(0, 0, 0.78, 0.98))
    fig.savefig(out)
    plt.close(fig)


def action_groups(runs: list[dict]) -> tuple[list[str], dict[str, list[dict]]]:
    """Systems that issued tool calls, fastest mean rate first. A repeat with no span has no rate."""
    by: dict[str, list[dict]] = {}
    for run in runs:
        if run.get("llm_actions") and run.get("llm_span_s"):
            by.setdefault(run["system"], []).append(run)
    rate = lambda s: statistics.mean(sum(r["llm_actions"].values()) / r["llm_span_s"] for r in by[s])
    return sorted(by, key=rate, reverse=True), by


def fig_bar(systems, rows, key, label, out, width, scale: float = 1.0, fmt: str = "{:.1f}") -> None:
    """Point = median of `key` / scale, ranked with the smallest at the top. Faint dots are the repeats.
    The line is their min and max, not a confidence interval. The axis starts at 0."""
    ranked = sorted(systems, key=lambda s: statistics.median(r[key] for r in rows[s]))
    fig, ax = plt.subplots(figsize=(width, 0.36 * len(ranked) + 0.75))
    for y, system in enumerate(ranked):
        color = series_color(system)
        vals = [r[key] / scale for r in rows[system]]
        med = statistics.median(vals)
        if max(vals) > min(vals):
            ax.plot([min(vals), max(vals)], [y, y], color=color, lw=1.2, zorder=2, solid_capstyle="butt")
        ax.scatter(vals, [y] * len(vals), s=18, color=color, alpha=0.35, zorder=3, linewidths=0)
        ax.scatter([med], [y], s=36, color=color, zorder=4, linewidths=0)
        ax.text(max(vals), y, "   " + fmt.format(med), va="center", ha="left", fontsize=8, color=INK)
    ax.set_yticks(range(len(ranked)), [look(s)[3] for s in ranked])
    for tick, system in zip(ax.get_yticklabels(), ranked):
        if model_name(system) in FOCUS:
            tick.set_color(FOCUS[model_name(system)]["color"])
    ax.invert_yaxis()
    ax.set_xlabel(label)
    ax.set_xlim(left=0)
    quant_grid(ax)
    fig.tight_layout()
    save(fig, out)
    plt.close(fig)


def fig_errors(systems, rows, out, width) -> None:
    """Critical and optional error rates. Each rate is over its own routes. Point = median, line = min–max
    across repeats. Ordered by the critical median, smallest at the top. Not a stacked share of one request set."""
    ranked = sorted(systems, key=lambda s: statistics.median(r["crit_err"] for r in rows[s]))
    fig, ax = plt.subplots(figsize=(width, 0.38 * len(ranked) + 0.9))
    for y, system in enumerate(ranked):
        for key, color, dy in (("crit_err", PAPER, -0.14), ("opt_err", BASELINE, 0.14)):
            vals = [r[key] for r in rows[system]]
            med = statistics.median(vals)
            if max(vals) > min(vals):
                ax.plot([min(vals), max(vals)], [y + dy, y + dy], color=color, lw=1.15, zorder=2, solid_capstyle="butt")
            ax.scatter(vals, [y + dy] * len(vals), s=14, color=color, alpha=0.35, zorder=3, linewidths=0)
            ax.scatter([med], [y + dy], s=28, color=color, zorder=4, linewidths=0)
    ax.set_yticks(range(len(ranked)), [look(s)[3] for s in ranked])
    ax.invert_yaxis()
    ax.set_xlabel(r"Error rate (\%)")
    ax.set_xlim(left=0)
    quant_grid(ax)
    ax.legend(handles=[
        Line2D([0], [0], marker="o", color=PAPER, lw=1.15, label="critical"),
        Line2D([0], [0], marker="o", color=BASELINE, lw=1.15, label="optional"),
    ], ncol=2, loc="lower center", bbox_to_anchor=(0.5, 1.02), handletextpad=0.4, columnspacing=1.2)
    fig.tight_layout()
    save(fig, out)
    plt.close(fig)


def fig_actions(runs, out, width, square: bool = False, rows: dict | None = None) -> list[str]:
    """Actions per second: a dot per repeat, a dash at the mean, a whisker for min–max.
    One column (3.39 in). Height is one row per model, enough for 8 pt type, plus the
    axis label and the legend. The whisker, the dots, and the name use the tradeoff's
    tab20 colour for that model. The mean dash stays black. `width` and `square` are unused."""
    del width, square
    systems, by = action_groups(runs)
    if not systems:
        return []
    palette = tab20_colors(systems, rows) if rows else {}
    wide = COLUMN
    left_in, right_in, bottom_in, top_in, row_in = 0.82, 0.52, 0.50, 0.34, 0.13
    high = min(8.0, top_in + bottom_in + row_in * len(systems))
    fig, ax = plt.subplots(figsize=(wide, high))
    fig.subplots_adjust(left=left_in / wide, right=1 - right_in / wide,
                        bottom=bottom_in / high, top=1 - top_in / high)
    for y, system in enumerate(systems):
        vals = [sum(r["llm_actions"].values()) / r["llm_span_s"] for r in by[system]]
        lo, hi, mean = min(vals), max(vals), statistics.mean(vals)
        color = palette.get(system, series_color(system))
        if hi > lo:
            ax.plot([lo, hi], [y, y], color=color, lw=1.2, solid_capstyle="butt", zorder=2)
        ax.scatter(vals, [y] * len(vals), s=18, color=color, alpha=0.35, zorder=3, linewidths=0)
        ax.plot([mean, mean], [y - 0.28, y + 0.28], color=INK, lw=1.3, zorder=4, solid_capstyle="round")
        ax.annotate(f"{mean:.2f}", xy=(hi, y), xytext=(12, 0), textcoords="offset points",
                    va="center", ha="left", fontsize=8, color=INK, annotation_clip=False)
    ax.set_yticks(range(len(systems)), [short_llm(s) for s in systems])
    for tick, system in zip(ax.get_yticklabels(), systems):
        if system in palette:
            tick.set_color(palette[system])
        elif model_name(system) in FOCUS:
            tick.set_color(FOCUS[model_name(system)]["color"])
    ax.invert_yaxis()
    ax.set_xlabel("Actions per second")
    ax.set_xlim(left=0)
    quant_grid(ax)
    ax.legend(handles=[
        Line2D([0], [0], color=INK, lw=1.3, label="mean"),
        Line2D([0], [0], color=MUTED, lw=1.2, label="min–max"),
    ], ncol=2, loc="lower center", bbox_to_anchor=(0.5, 1.02), handletextpad=0.4, columnspacing=1.2)
    save(fig, out)
    plt.close(fig)
    return systems


def fig_action_bars(runs, out, width) -> None:
    """Stacked bars, shorter than the tradeoff row and a little wider. One bar per model, rising from zero.
    A segment is that tool's share of the model's calls, pooled over repeats that issued at least one
    call, so the stack sums to 100%. A repeat with no calls is left out. Fastest mean rate at the left.
    `width` is unused."""
    from matplotlib.ticker import PercentFormatter

    del width
    systems, by = action_groups(runs)
    if not systems:
        return
    present = [a for a in ACTIONS if any(r["llm_actions"].get(a) for s in systems for r in by[s])]
    present += sorted({n for s in systems for r in by[s] for n in r["llm_actions"]} - set(ACTIONS))
    if not present:
        return
    totals = []
    for system in systems:
        counts: dict[str, int] = {}
        for run in by[system]:
            for name, n in run["llm_actions"].items():
                counts[name] = counts.get(name, 0) + n
        totals.append(counts)
    wide, high = action_figsize()
    top_in, bottom_in = 0.82, 0.66
    high += (top_in - 0.46) + (bottom_in - 0.52)
    fig, ax = plt.subplots(figsize=(wide, high))
    fig.subplots_adjust(left=0.42 / wide, right=1 - 0.08 / wide,
                        bottom=bottom_in / high, top=1 - top_in / high)
    bottoms = [0.0] * len(systems)
    for name in present:
        heights = [c.get(name, 0) / sum(c.values()) if c else 0.0 for c in totals]
        ax.bar(range(len(systems)), heights, bottom=bottoms, width=0.72, linewidth=0,
               color=ACTION_COLOR.get(name, MUTED), zorder=2, label=ACTION_LABEL.get(name, name))
        bottoms = [b + h for b, h in zip(bottoms, heights)]
    ax.set_xticks(range(len(systems)), [short_llm(s) for s in systems], rotation=60, ha="right")
    for tick, system in zip(ax.get_xticklabels(), systems):
        if model_name(system) in FOCUS:
            tick.set_color(FOCUS[model_name(system)]["color"])
    ax.set_xlim(-0.6, len(systems) - 0.4)
    ax.set_ylim(0, 1)
    ax.yaxis.set_major_formatter(PercentFormatter(xmax=1, decimals=0))
    ax.set_ylabel("Share of calls")
    ax.grid(axis="y", visible=True, color=GRID, linewidth=0.4)
    ax.grid(axis="x", visible=False)
    ax.set_axisbelow(True)
    ax.legend(ncol=1, loc="lower center", bbox_to_anchor=(0.5, 1.01),
              handlelength=0.8, handletextpad=0.4, labelspacing=0.2, borderaxespad=0.0)
    save(fig, out)
    plt.close(fig)


def fig_action_mix(runs, out, width) -> None:
    systems, by = action_groups(runs)
    if not systems:
        return
    present = [a for a in ACTIONS if any(r["llm_actions"].get(a) for s in systems for r in by[s])]
    present += sorted({n for s in systems for r in by[s] for n in r["llm_actions"]} - set(ACTIONS))
    fig, ax = plt.subplots(figsize=(FULL, 0.36 * len(systems) + 0.95))
    for y, system in enumerate(systems):
        counts: dict[str, int] = {}
        for run in by[system]:
            for name, n in run["llm_actions"].items():
                counts[name] = counts.get(name, 0) + n
        total = sum(counts.values())
        left = 0.0
        for name in present:
            share = counts.get(name, 0) / total
            if not share:
                continue
            ax.barh(y, share, left=left, height=0.62, color=ACTION_COLOR.get(name, MUTED), zorder=2)
            if share >= 0.12:
                ax.text(left + share / 2, y, f"{share * 100:.0f}\\%", ha="center", va="center", fontsize=8, color="white")
            left += share
    ax.set_yticks(range(len(systems)), [short_llm(s) for s in systems])
    for tick, system in zip(ax.get_yticklabels(), systems):
        if model_name(system) in FOCUS:
            tick.set_color(FOCUS[model_name(system)]["color"])
    ax.invert_yaxis()
    ax.set_xlim(0, 1)
    ax.set_xlabel("share of the system's actions (pooled over repeats)")
    ax.grid(axis="y", visible=False)
    ax.legend(handles=[Rectangle((0, 0), 1, 1, color=ACTION_COLOR.get(a, MUTED), label=a) for a in present],
              ncol=min(len(present), 4), loc="lower center", bbox_to_anchor=(0.5, 1.0),
              handlelength=0.8, handletextpad=0.3, columnspacing=0.8)
    fig.tight_layout()
    fig.savefig(out)
    plt.close(fig)


def _metric_kind(metric: str) -> str:
    return metric.partition(":")[0]


def _is_percentile(kind: str) -> bool:
    return len(kind) > 1 and kind[0] == "p" and kind[1:].isdigit()


def _speedup(kind: str, baseline: float, value: float) -> float:
    """Larger is better. Latency and error rate: baseline / method. Throughput: method / baseline."""
    if baseline != baseline or value != value or baseline <= 0 or value <= 0:
        return float("nan")
    if _is_percentile(kind) or kind == "err":
        return baseline / value
    if kind == "tput":
        return value / baseline
    raise SystemExit(f"{kind} has no speedup direction (use p50, p95, p99, err, or tput)")


def _workload_values(runs: list[dict], m: Metrics, metric: str, by: str, routes: list[str] | None) -> dict[str, dict]:
    """One finite measurement per (system, workload). A workload is a repeat, or a route in a repeat.
    The first run wins if a system is recorded twice for the same workload.
    A route the run did not serve is skipped, so two benchmarks can share one figure."""
    kind, _, spec = metric.partition(":")
    sample = next((r["routes"] for r in runs if r["routes"]), {})
    if routes is None:
        routes = m.routes(spec, sample) if spec else list(m.critical)
    values: dict[str, dict] = {}
    for run in runs:
        slot = values.setdefault(run["system"], {})
        if by == "run":
            key = (run["repeat"],)
            if key in slot:
                print(f"two runs for {run['system']} {run['repeat']}; kept the first")
                continue
            slot[key] = m.value(run, metric if spec else f"{kind}:critical")
            continue
        if by != "route":
            raise SystemExit(f"--speedup-by must be route or run, not {by}")
        for route in routes:
            part = run["routes"].get(route)
            if not part:
                continue
            key = (run["repeat"], route)
            if key in slot:
                print(f"two runs for {run['system']} {run['repeat']} {route}; kept the first")
                continue
            if kind == "err":
                slot[key] = 100 * part["errors"] / max(part["sent"], 1)
            elif kind == "tput":
                slot[key] = part["sent"] / run["duration_s"]
            elif _is_percentile(kind):
                slot[key] = m.p(part, int(kind[1:]) / 100)
            else:
                raise SystemExit(f"{kind} has no speedup direction (use p50, p95, p99, err, or tput)")
    return values


def _route_label(route: str) -> str:
    return {"read-home-timeline": "home timeline", "read-user-timeline": "user timeline",
            "compose-post": "compose-post", "read-page": "read page",
            "compose-review": "compose review", "read-movie-reviews": "movie reviews"}.get(route, route)


def _workload_label(key: tuple) -> str:
    if len(key) == 1:
        return key[0]
    repeat, route = key
    return f"{_route_label(route)} {repeat}"


_HOTEL_ROUTES = {"hotels", "reservation", "recommendations", "review", "restaurants", "museums", "cinema"}
_SOCIAL_ROUTES = {"compose-post", "read-home-timeline", "read-user-timeline", "follow"}
_MEDIA_ROUTES = {"read-page", "compose-review", "read-movie-reviews"}


def _app_of(route: str) -> str:
    """Panel title for a route. A route from another application is not called Social."""
    if route in _HOTEL_ROUTES:
        return "Hotel"
    if route in _SOCIAL_ROUTES:
        return "Social"
    if route in _MEDIA_ROUTES:
        return "Media"
    return ""


def _repeat_ord(key: tuple):
    rep = key[0]
    return int(rep[1:]) if rep.startswith("r") and rep[1:].isdigit() else rep


def fig_speedup(runs, m: Metrics, out, width, metric: str, by: str, baseline: str, focus: str,
                routes: list[str] | None, keep: set[str] | None, complete: bool) -> None:
    """Paragon-style speedup. Workloads the focus system ran are ordered by its speedup, worst to best.
    `--focus median` orders by the median speedup of the drawn systems. Routes the focus never ran
    follow, ordered by that median. With `--speedup-complete`, a workload is kept only when every
    drawn system has it. The y-axis is logarithmic when the ratios span more than 10x."""
    if baseline not in {r["system"] for r in runs}:
        raise SystemExit(f"baseline {baseline} is not in these runs")
    values = _workload_values(runs, m, metric, by, routes)
    if focus != "median" and focus not in values:
        raise SystemExit(f"focus {focus} is not in these runs")
    kind = _metric_kind(metric)
    base = values[baseline]
    methods = [s for s in systems_of(runs) if s != baseline and s in values and (not keep or s in keep)]
    focus_routes = {k[1] for k in values.get(focus, {})} if by == "route" and focus != "median" else set()
    paired, rest, dropped = [], [], []
    for key, b in base.items():
        others = []
        missing = False
        for system in methods:
            other = _speedup(kind, b, values[system].get(key, float("nan")))
            if other == other:
                others.append(other)
            else:
                missing = True
        if complete and (missing or not others):
            dropped.append(key)
            continue
        if focus == "median":
            if others:
                paired.append((statistics.median(others), _repeat_ord(key), key))
            else:
                dropped.append(key)
            continue
        # Hotel routes are the left group. A repeat the focus did not measure is left out,
        # so later hotel repeats do not stretch the axis past luna. Order is the other
        # systems' median.
        if by == "route" and key[1] in _HOTEL_ROUTES:
            spd = _speedup(kind, b, values[focus].get(key, float("nan")))
            if spd != spd:
                dropped.append(key)
                continue
            others = [o for system in methods if system != focus
                      for o in [_speedup(kind, b, values[system].get(key, float("nan")))] if o == o]
            rest.append((statistics.median(others) if others else spd, _repeat_ord(key), key))
            continue
        spd = _speedup(kind, b, values[focus].get(key, float("nan")))
        if spd == spd:
            paired.append((spd, _repeat_ord(key), key))
            continue
        if by == "route" and key[1] in focus_routes:
            dropped.append(key)
            continue
        if others:
            rest.append((statistics.median(others), _repeat_ord(key), key))
        else:
            dropped.append(key)
    if dropped:
        why = "a drawn system has no measurement" if complete or focus == "median" else "focus ran this route, but not this repeat"
        print(f"left out ({why}): " + ", ".join(_workload_label(k) for k in sorted(dropped, key=_repeat_ord)))
    if rest and paired:
        print(f"hotel, then social, then media; hotel by the median of the other systems, "
              f"then {look(focus)[3]}, worst to best")
    # The focus line is drawn with a higher z-order. Legend order is focus, Fixed 40, then the policies.
    legend_order = [s for s in [focus, "fixed40", "l3", "shift", "l3shift", "degrade",
                                "gnn-l3", "gnn-shift", "gnn-l3shift", "gnn-degrade"] if s in methods]
    legend_order += [s for s in methods if s not in legend_order]

    def fold_routes(entries, hotel: bool):
        """One point per route: the median of the per-repeat speedups already paired above."""
        by_route: dict[str, list] = {}
        for _, _, key in entries:
            by_route.setdefault(key[1], []).append(key)
        rows = []
        for route, keys in by_route.items():
            per: dict[str, float] = {}
            for system in methods:
                ys = [y for key in keys
                      if (y := _speedup(kind, base[key], values[system].get(key, float("nan")))) == y]
                if ys:
                    per[system] = statistics.median(ys)
            if hotel:
                others = [per[s] for s in methods if s != focus and s in per]
                score = statistics.median(others) if others else per.get(focus, float("nan"))
            elif focus == "median":
                score = statistics.median(list(per.values())) if per else float("nan")
            else:
                score = per.get(focus, float("nan"))
            if score != score:
                continue
            reps = sorted({key[0] for key in keys}, key=lambda rep: _repeat_ord((rep, route)))
            rows.append((score, route, per, reps))
        rows.sort(key=lambda row: (row[0], row[1]))
        return rows

    placed = []
    cursor = 0.0
    if by == "route":
        folded = []
        # Hotel, social, and media stay separate groups. Hotel is ordered by the other systems.
        by_app = {"Hotel": [], "Social": [], "Media": [], "": []}
        for item in rest + paired:
            by_app[_app_of(item[2][1])].append(item)
        for name in ("Hotel", "Social", "Media", ""):
            if by_app[name]:
                folded.append(fold_routes(by_app[name], hotel=(name == "Hotel")))
        if not any(folded):
            raise SystemExit("no workload left to plot")
        for gi, block in enumerate(folded):
            if gi and placed:
                cursor += 1.0
            placed.append([(cursor + i, route) for i, (_, route, _, _) in enumerate(block)])
            cursor += len(block)
        series = {s: [[per.get(s, float("nan")) for _, _, per, _ in block] for block in folded]
                  for s in legend_order}
    else:
        paired.sort()
        rest.sort()
        groups = []
        if rest:
            groups.append([key for _, _, key in rest])
        if paired:
            groups.append([key for _, _, key in paired])
        if not groups:
            raise SystemExit("no workload left to plot")
        for gi, group in enumerate(groups):
            if gi and placed:
                cursor += 1.0
            block = []
            for key in group:
                block.append((cursor, key))
                cursor += 1.0
            placed.append(block)
        series = {s: [[_speedup(kind, base[key], values[s].get(key, float("nan"))) for _, key in block]
                      for block in placed] for s in legend_order}
    finite = [y for blocks in series.values() for ys in blocks for y in ys if y == y and y > 0]
    n_points = sum(len(block) for block in placed)
    log = max(finite) / min(finite) > 10
    width = FULL if n_points > 6 or len(legend_order) > 2 else width
    legend_rows = (len(legend_order) + 1 + 3) // 4
    label_in = 0.45 if n_points > 24 else (1.15 if n_points > 12 else 0.95)
    # Full-width speedup matches the anomaly plot: a 1.12 in y-axis, with a numbered workload index.
    if by == "route" or n_points > 24:
        height = 1.94
    else:
        height = min(MAX_HEIGHT, 2.05 + label_in + 0.28 * legend_rows)
    fig, ax = plt.subplots(figsize=(width, height), constrained_layout=True)
    ax.axhline(1.0, color="#333333", lw=0.8, zorder=1)
    for left, right in zip(placed, placed[1:]):
        ax.axvline((left[-1][0] + right[0][0]) / 2, color="#bdbdbd", lw=0.6, zorder=0)
    for system in legend_order:
        st = mark_of(system)
        if system == focus:
            st = {**st, "ls": "-", "lw": 2.0, "z": 6}
        elif system == "fixed40":
            st = {**st, "ls": "--", "lw": 1.0}
        for block, ys in zip(placed, series[system]):
            ax.plot([x for x, _ in block], ys, color=st["color"], ls=st["ls"], lw=st["lw"], zorder=st["z"])
    xs = [x for block in placed for x, _ in block]
    order = [key for block in placed for _, key in block]
    rotation = 90 if n_points > 12 else 40
    if by == "route" or n_points > 24:
        ax.set_xticks(xs, [str(i) for i in range(1, n_points + 1)])
        ax.tick_params(axis="x", length=2, pad=1)
        for i, key in enumerate(order, start=1):
            label = _route_label(key) if by == "route" else _workload_label(key)
            print(f"{i:3}  {label}")
    else:
        ax.set_xticks(xs, [_workload_label(k) for k in order], rotation=rotation,
                      ha="center" if rotation == 90 else "right")
    ax.set_xlim(xs[0] - 0.5, xs[-1] + 0.5)
    if log:
        ax.set_yscale("log")
        ax.yaxis.set_major_formatter(plt.FuncFormatter(lambda v, _p: f"{v:g}"))
        ax.yaxis.set_minor_formatter(NullFormatter())
    pad = 1.35 if log else 0.08 * (max(finite) - min(finite) + 1e-9)
    lo, hi = (min(finite) / pad, max(finite) * pad) if log else (min(finite) - pad, max(finite) + pad)
    ax.set_ylim(min(lo, 0.85 if not log else 1 / pad), max(hi, 1.15 if not log else pad))
    ax.set_xlabel("Workloads")
    ax.set_ylabel(f"Speedup over {look(baseline)[3]}")
    ax.grid(axis="y", visible=True, color=GRID, linewidth=0.4)
    ax.grid(axis="x", visible=False)
    what = {"p50": "p50 latency", "p95": "p95 latency", "p99": "p99 latency",
            "err": "error rate", "tput": "throughput"}[kind]
    if focus == "median":
        note = f"{what}\nby median, worst to best"
    else:
        if rest and {k[1] for *_, k in rest} <= _HOTEL_ROUTES:
            later = [name for name, routes in (("social", _SOCIAL_ROUTES), ("media", _MEDIA_ROUTES))
                     if any(k[1] in routes for *_, k in paired)]
            note = f"{what}\nhotel by median\nthen {', '.join(later) or look(focus)[3]}\nby {look(focus)[3]}"
        else:
            note = f"{what}\nby {look(focus)[3]}, worst to best"
            if rest:
                note += "\nthen by median"
    if log:
        note = "log scale\n" + note
    ax.text(0.02, 0.98, note, transform=ax.transAxes, ha="left", va="top", fontsize=8, color=INK, zorder=6)
    handles = [Line2D([0], [0], color="#333333", lw=0.8, label=look(baseline)[3])]
    handles += [Line2D([0], [0], color=mark_of(s)["color"],
                       ls="-" if s == focus else ("--" if s == "fixed40" else mark_of(s)["ls"]),
                       lw=2.0 if s == focus else 1.0, label=mark_of(s)["label"])
               for s in legend_order]
    ax.legend(handles=handles, ncol=4, loc="lower center", bbox_to_anchor=(0.5, 1.02),
              handlelength=2.2, columnspacing=0.9)
    save(fig, out)
    plt.close(fig)
    order_name = "the median of the drawn lines" if focus == "median" else look(focus)[3]
    if rest and paired and focus != "median":
        order_name = f"hotel by the median of the other systems, then {order_name}"
    print(f"speedup {what}: {'baseline/method' if kind != 'tput' else 'method/baseline'}; "
          f"baseline {look(baseline)[3]}; order {order_name}, worst to best; "
          f"{'log' if log else 'linear'} y; {len(order)} workloads")
    for system in legend_order:
        ys = [y for block in series[system] for y in block if y == y]
        if not ys:
            print(f"{mark_of(system)['label']:16} no paired workloads")
            continue
        print(f"{mark_of(system)['label']:16} n={len(ys):2} "
              f"min={min(ys):8.3f} median={statistics.median(ys):8.3f} max={max(ys):8.3f}")


# Empirical percentiles of a pooled histogram. p99.5 and p99.9 sit past the integer tail.
_PCT = [i / 100 for i in range(1, 100)] + [0.995, 0.999]


def _positive_hist(part: dict | None) -> dict | None:
    """Histogram counts of completed requests. An empty histogram, or one with no positive counts, is missing."""
    if not part:
        return None
    hist = {b: c for b, c in (part.get("hist") or {}).items() if c and c > 0}
    return hist or None


def _pool_hist(by_route: dict, route: str, system: str, repeats: list[str]) -> dict:
    out: dict = {}
    for rep in repeats:
        hist = _positive_hist(by_route[route][system][rep]["routes"].get(route))
        if not hist:
            continue
        for bound, count in hist.items():
            out[bound] = out.get(bound, 0) + count
    return out


def _pct_curve(base: dict, other: dict) -> tuple[list[float], list[float]]:
    """S(p) at each percentile. A non-positive quantile is a gap, not an infinite speedup."""
    xs, ys = [], []
    for q in _PCT:
        xs.append(q * 100)
        ys.append(_speedup("p95", pct(base, q), pct(other, q)))
    return xs, ys


def _pct_style(system: str, focus: str) -> dict:
    st = mark_of(system)
    if system == focus:
        return {**st, "ls": "-", "lw": 2.0, "z": 6}
    if system == "fixed40":
        return {**st, "ls": "--", "lw": 1.0, "z": 4}
    return st


def _apply_speedup_scale(ax, ys, log: bool) -> None:
    finite = [y for y in ys if y == y and y > 0]
    if not finite:
        ax.set_ylim(0.5, 2)
        return
    if log:
        pad = 1.45
        lo, hi = min(finite) / pad, max(finite) * pad
        ax.set_yscale("log")
        ax.set_ylim(min(lo, 1 / pad), max(hi, pad))
        ax.yaxis.set_major_formatter(plt.FuncFormatter(lambda v, _p: f"{v:g}"))
        ax.yaxis.set_minor_formatter(NullFormatter())
    else:
        span = max(finite) - min(finite)
        pad = 0.1 * (span + 1e-9)
        ax.set_ylim(min(min(finite) - pad, 0.85), max(max(finite) + pad, 1.15))


def _pct_series(by_route, route, baseline, methods, app):
    """One curve per controller. Repeats match on this endpoint only, so two applications that both
    use r1 are not pooled together."""
    series = []
    base_runs = by_route.get(route, {}).get(baseline, {})
    for system in methods:
        own = by_route.get(route, {}).get(system, {})
        reps = sorted((rep for rep in base_runs if rep in own), key=lambda rep: _repeat_ord((rep, route)))
        if not reps:
            print(f"{app} {_route_label(route):16} {mark_of(system)['label']:16} no paired repeats")
            continue
        base = _pool_hist(by_route, route, baseline, reps)
        other = _pool_hist(by_route, route, system, reps)
        xs, ys = _pct_curve(base, other)
        if not any(y == y and y > 0 for y in ys):
            print(f"{app} {_route_label(route):16} {mark_of(system)['label']:16} no positive percentile")
            continue
        series.append((system, xs, ys))
        print(f"{app} {_route_label(route):16} {mark_of(system)['label']:16} "
              f"repeats={','.join(reps)} requests {sum(base.values())} vs {sum(other.values())}")
    return series


def _draw_pct(ax, series, focus, xlim, ticks, labels, ys, shade: bool) -> None:
    finite = [y for y in ys if y == y and y > 0]
    log = bool(finite) and max(finite) / min(finite) > 10
    ax.axhline(1.0, color="#333333", ls="--", lw=0.8, zorder=1)
    if shade:
        ax.axvspan(90, 99.9, color=SHADE, lw=0, zorder=0)
    for system, xs, points in series:
        st = _pct_style(system, focus)
        kept = [(x, y) for x, y in zip(xs, points) if xlim[0] - 1e-9 <= x <= xlim[1] + 1e-9]
        ax.plot([x for x, _ in kept], [y for _, y in kept], color=st["color"], ls=st["ls"], lw=st["lw"], zorder=st["z"])
    ax.set_xlim(*xlim)
    ax.set_xticks(ticks)
    if labels:
        ax.set_xticklabels(labels)
    _apply_speedup_scale(ax, finite, log)
    ax.grid(axis="y", visible=True, color=GRID, linewidth=0.4)
    ax.grid(axis="x", visible=False)


def fig_pct_speedup(runs, out, baseline: str, focus: str, routes: list[str], keep: set[str] | None) -> None:
    """Percentile latency speedup. One row per endpoint, so different request populations stay apart.
    The right-hand panel is the tail, p90–p99.9. A controller's curve uses only the repeats where both
    it and the baseline recorded that endpoint. The quantile is the histogram upper bound (`pct`)."""
    by_route: dict[str, dict[str, dict]] = {}
    for run in runs:
        for route in routes:
            if not _positive_hist(run["routes"].get(route)):
                continue
            slot = by_route.setdefault(route, {}).setdefault(run["system"], {})
            if run["repeat"] in slot:
                print(f"two runs for {run['system']} {run['repeat']} {route}; kept the first")
                continue
            slot[run["repeat"]] = run
    if not any(baseline in by_route.get(route, {}) for route in routes):
        raise SystemExit(f"baseline {baseline} is not in these runs")
    present = {s for route in by_route.values() for s in route}
    methods = [s for s in systems_of(runs) if s != baseline and s in present and (not keep or s in keep)]
    panels = [(_app_of(route), route) for route in routes if route in by_route]
    if not panels:
        raise SystemExit("no endpoint left to plot")
    nrows = len(panels)
    height = min(MAX_HEIGHT, 0.62 + 1.62 * nrows)
    fig, axes = plt.subplots(nrows, 2, figsize=(FULL, height), squeeze=False, constrained_layout=True,
                             width_ratios=[1.35, 1])
    drawn: list[str] = []
    for row, (app, route) in enumerate(panels):
        series = _pct_series(by_route, route, baseline, methods, app)
        for system, _, _ in series:
            if system not in drawn:
                drawn.append(system)
        full_y = [y for _, _, ys in series for y in ys]
        tail_y = [y for _, xs, ys in series for x, y in zip(xs, ys) if x >= 90]
        full, zoom = axes[row]
        _draw_pct(full, series, focus, (1, 100), [1, 25, 50, 75, 99], None, full_y, shade=True)
        _draw_pct(zoom, series, focus, (90, 100), [90, 95, 99, 99.9], ["90", "95", "99", "99.9"], tail_y, shade=False)
        full.set_title(f"{app}: {_route_label(route)}", loc="left", fontsize=8, color=INK)
        zoom.set_title("p90–p99.9", loc="left", fontsize=8, color=INK)
        full.set_ylabel("Speedup (×)")
        zoom.set_ylabel("Speedup (×)")
        if row == nrows - 1:
            full.set_xlabel("Percentile")
            zoom.set_xlabel("Percentile")
    order = [s for s in [focus, "fixed40", "l3", "shift", "l3shift", "degrade",
                         "gnn-l3", "gnn-shift", "gnn-l3shift", "gnn-degrade"] if s in drawn]
    order += [s for s in drawn if s not in order]
    handles = [Line2D([0], [0], color="#333333", ls="--", lw=0.8, label="1×")]
    handles += [Line2D([0], [0], color=_pct_style(s, focus)["color"],
                       ls=_pct_style(s, focus)["ls"], lw=_pct_style(s, focus)["lw"],
                       label=mark_of(s)["label"]) for s in order]
    if handles:
        fig.legend(handles=handles, ncol=4, loc="outside upper center", handlelength=2.2, columnspacing=0.9)
    save(fig, out)
    svg = os.path.splitext(out)[0] + ".svg"
    fonttype = plt.rcParams["svg.fonttype"]
    plt.rcParams["svg.fonttype"] = "path"
    fig.savefig(svg, bbox_inches=None)
    plt.rcParams["svg.fonttype"] = fonttype
    plt.close(fig)
    print(f"percentile speedup: baseline/controller; baseline {look(baseline)[3]}; "
          f"{' '.join(_route_label(route) for _, route in panels)}")


def _folded_route_speedups(runs, m, metric, baseline, focus, routes, keep):
    """Median per-repeat speedup of each route. Hotel routes first, then the focus system's routes.
    Same pairing as fig_speedup: a repeat is kept only when the focus measured it."""
    values = _workload_values(runs, m, metric, "route", routes)
    if focus not in values:
        raise SystemExit(f"focus {focus} is not in these runs")
    kind = _metric_kind(metric)
    base = values[baseline]
    methods = [s for s in systems_of(runs) if s != baseline and s in values and (not keep or s in keep)]
    buckets = {name: {} for name in ("Hotel", "Social", "Media", "")}
    for key, b in base.items():
        route = key[1]
        spd = _speedup(kind, b, values[focus].get(key, float("nan")))
        if spd != spd:
            continue
        slot = buckets[_app_of(route)].setdefault(route, {"keys": [], "per": {}})
        slot["keys"].append(key)
    rows = []
    for name in ("Hotel", "Social", "Media", ""):
        hotel_group, bucket = name == "Hotel", buckets[name]
        group = []
        for route, slot in bucket.items():
            per = {}
            for system in methods:
                ys = [y for key in slot["keys"]
                      if (y := _speedup(kind, base[key], values[system].get(key, float("nan")))) == y]
                if ys:
                    per[system] = statistics.median(ys)
            focus_ys = [y for key in slot["keys"]
                        if (y := _speedup(kind, base[key], values[focus].get(key, float("nan")))) == y]
            if hotel_group:
                others = [per[s] for s in methods if s != focus and s not in ("fixed8", "fixed40") and s in per]
                score = statistics.median(others) if others else (statistics.median(focus_ys) if focus_ys else float("nan"))
            else:
                score = statistics.median(focus_ys) if focus_ys else float("nan")
            if score != score:
                continue
            group.append((score, route, per))
        group.sort(key=lambda row: (row[0], row[1]))
        if group:
            rows.append(group)
    legend = [s for s in [focus, "fixed40", "l3", "shift", "l3shift", "degrade",
                          "gnn-l3", "gnn-shift", "gnn-l3shift", "gnn-degrade"] if s in methods]
    legend += [s for s in methods if s not in legend]
    return legend, rows


# Blues for the systems the bars compare. FIRM keeps the same hue and stays hollow.
_BAR_BLUE = {
    "llm40-luna": "#D55E00",
    "l3": "#08519c", "gnn-l3": "#08519c",
    "shift": "#2171b5", "gnn-shift": "#2171b5",
    "l3shift": "#4292c6", "gnn-l3shift": "#4292c6",
    "degrade": "#6baed6", "gnn-degrade": "#6baed6",
}


def _bar_color(system: str) -> str:
    return _BAR_BLUE.get(system, "#08519c")


def _baseline_name(baseline: str) -> str:
    """Printed baseline name. `fixed8` is minimum and `fixed40` is maximum."""
    return look(baseline)[3]


def fig_speedup_bars(runs, m, out, baseline, focus, routes, keep, metrics: list[str], fig_w: float,
                     lines: bool = False) -> None:
    """Latency speedup, one row per percentile, the same numbered workloads on each.
    Bars grow up or down from the line at 1. With lines, each system is one line and a missing
    measurement is a gap."""
    if baseline not in {r["system"] for r in runs}:
        raise SystemExit(f"baseline {baseline} is not in these runs")
    folded = {metric: _folded_route_speedups(runs, m, metric, baseline, focus, routes, keep) for metric in metrics}
    legend, order_rows = folded[metrics[0]]
    # Exchange the first three hotel workloads with workloads 5–7. Workload 4 stays put.
    if order_rows and len(order_rows[0]) >= 7 and all(route in _HOTEL_ROUTES for _, route, _ in order_rows[0]):
        order_rows[0][0:3], order_rows[0][4:7] = order_rows[0][4:7], order_rows[0][0:3]
    order = [route for group in order_rows for _, route, _ in group]
    splits = []
    count = 0
    for group in order_rows[:-1]:
        count += len(group)
        splits.append(count)
    by_metric = {}
    for metric in metrics:
        _, groups = folded[metric]
        by_metric[metric] = {route: per for group in groups for _, route, per in group}
    for i, route in enumerate(order, start=1):
        print(f"{i:3}  {_route_label(route)}")
    n = len(legend)
    bar_w = 0.12
    gap = 0.7 if lines else 0.42
    pitch = 1.0 if lines else n * bar_w + gap
    centers = []
    cursor = 0.0
    for i in range(len(order)):
        if i in splits:
            cursor += gap
        centers.append(cursor + (0 if lines else (n - 1) * bar_w / 2))
        cursor += pitch
    # Each percentile row is a short axis. 2.14 in total leaves about 0.40 in of y-axis per row.
    height = min(MAX_HEIGHT, 0.90 + 0.62 * len(metrics))
    fig, axes = plt.subplots(len(metrics), 1, figsize=(fig_w, height), sharex=True, squeeze=False,
                             constrained_layout=True)
    labels = {"p50": "p50 latency", "p90": "p90 latency", "p95": "p95 latency", "p99": "p99 latency"}
    for ax, metric in zip(axes[:, 0], metrics):
        per = by_metric[metric]
        finite = [v for route in order for v in per.get(route, {}).values() if v == v and v > 0]
        log = bool(finite) and max(finite) / min(finite) > 10
        ax.axhline(1.0, color="#333333", lw=0.8, zorder=2)
        for split in splits:
            ax.axvline((centers[split - 1] + centers[split]) / 2, color="#bdbdbd", lw=0.6, zorder=0)
        for j, system in enumerate(legend):
            st = mark_of(system)
            filled = st.get("filled", True)
            color = "#333333" if system in ("fixed8", "fixed40") else _bar_color(system)
            if lines:
                ys = []
                for route in order:
                    value = per.get(route, {}).get(system, float("nan"))
                    ys.append(value if value == value and value > 0 else float("nan"))
                ax.plot(centers, ys, color=color, lw=1.6 if system == focus else 1.0,
                        ls="-" if filled else (0, (1.4, 1.0)),
                        zorder=4 if system == focus else 3)
                continue
            xs, bottoms, heights = [], [], []
            for i, route in enumerate(order):
                value = per.get(route, {}).get(system, float("nan"))
                if value != value or value <= 0:
                    continue
                lo, hi = (1.0, value) if value >= 1 else (value, 1.0)
                xs.append(centers[i] + (j - (n - 1) / 2) * bar_w)
                bottoms.append(lo)
                heights.append(hi - lo)
            ax.bar(xs, heights, bar_w * 0.9, bottom=bottoms,
                   color=color if filled else "white", edgecolor=color,
                   linewidth=0.6, zorder=3)
        _apply_speedup_scale(ax, finite, log)
        ax.set_title(labels.get(metric, metric), loc="left", fontsize=8, color=INK)
        ax.grid(axis="y", visible=True, color=GRID, linewidth=0.4)
        ax.grid(axis="x", visible=False)
        ax.tick_params(axis="x", labelbottom=False)
    axes[-1, 0].set_xticks(centers, [str(i) for i in range(1, len(order) + 1)])
    axes[-1, 0].tick_params(axis="x", labelbottom=True)
    axes[-1, 0].set_xlim(centers[0] - pitch / 2, centers[-1] + pitch / 2)
    fig.supxlabel("Workloads")
    fig.supylabel(f"Speedup over {_baseline_name(baseline)}")
    handles = [Line2D([0], [0], color="#333333", lw=0.8, label=_baseline_name(baseline))]
    for system in legend:
        st = mark_of(system)
        color = "#333333" if lines and system in ("fixed8", "fixed40") else _bar_color(system)
        filled = st.get("filled", True)
        if lines:
            handles.append(Line2D([0], [0], color=color, lw=1.6 if system == focus else 1.0,
                                  ls="-" if filled else (0, (1.4, 1.0)), label=st["label"]))
        else:
            handles.append(Rectangle((0, 0), 1, 1, facecolor=color if filled else "white",
                                     edgecolor=color, linewidth=0.6, label=st["label"]))
    fig.legend(handles=handles, ncol=3 if fig_w < FULL else 4, loc="outside upper center",
               handlelength=1.2, columnspacing=0.9)
    save(fig, out)
    plt.close(fig)
    kind = "lines" if lines else "bars"
    print(f"speedup {kind}: {' and '.join(metrics)}; order is {metrics[0]}, hotel by the other systems, "
          f"then social, then media, by {look(focus)[3]}, worst to best")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("inputs", nargs="+", help="runs.json[:system=newname,...]")
    ap.add_argument("-d", "--dir", default="figs")
    ap.add_argument("--width", type=float, default=3.39,
                    help="figure width in inches: 3.39 one column, 6.90 two columns. Other values snap to the nearer")
    ap.add_argument("--critical", default="hotels,reservation")
    ap.add_argument("--summary", default="p50:hotels,p95:hotels,tput,cores,err:reservation,err:optional,pool")
    ap.add_argument("--latency", default="hotels", help="routes of the cost, phases and timeline latency")
    ap.add_argument("--errors-as-ms", type=float, default=None)
    ap.add_argument("--smooth", type=int, default=3, help="timeline: bins merged per point")
    ap.add_argument("--format", default="pdf")
    ap.add_argument("--only", default="",
                    help="comma list of figures to write (summary,cost,phases,timeline,actions,action-mix,action-bars,p50,p95,alloc,pool,errors,cdf,tradeoff,speedup,speedup-bars,speedup-lines,pctspeedup). "
                         "Default writes the paper set")
    ap.add_argument("--baseline", default="fixed8", help="speedup: system the other lines are divided by")
    ap.add_argument("--focus", default="llm40-luna",
                    help="speedup: line that sets the workload order, worst to best, or median")
    ap.add_argument("--speedup-complete", action="store_true",
                    help="speedup: keep a workload only when every drawn system measured it")
    ap.add_argument("--speedup-metric", default="p95", help="speedup: p50, p95, p99, err, or tput, optional :ROUTES")
    ap.add_argument("--speedup-by", default="route", choices=("route", "run"),
                    help="speedup: one point per route, repeats folded by the median speedup, or one point per repeat")
    ap.add_argument("--speedup-routes", default="",
                    help="speedup: routes to plot, comma-separated (default: --critical). A run that lacks a route skips it")
    ap.add_argument("--pct-routes", default="",
                    help="pctspeedup: endpoints, comma-separated (default: --critical). Each endpoint is its own panel")
    ap.add_argument("--bar-metrics", default="p50,p90",
                    help="speedup-bars: percentiles drawn as rows, comma-separated")
    ap.add_argument("--speedup-systems", default="",
                    help="speedup: systems to draw besides the baseline (default: all)")
    ap.add_argument("--rename-dir", action="append", default=[], metavar="BASENAME:OLD=NEW",
                    help="rename systems from the comparison directory with this basename")
    ap.add_argument("--square", action="store_true",
                    help="accepted for old commands; actions and action-bars are shorter than the tradeoff row and a little wider")
    ap.add_argument("--zoom-x", action="store_true",
                    help="cost: fit the x-axis to the core-seconds present, instead of starting at zero")
    ap.add_argument("--timeline-systems", default="",
                    help="systems in timeline.pdf, comma-separated (default: all; more than about 7 crowd it)")
    ap.add_argument("--max-api-errors", type=float, default=0.05,
                    help="drop llm runs where more than this share of the controller's episodes failed at the model "
                         "API (a 429 quota error leaves the system doing nothing: the fixed pool, not the llm)")
    a = ap.parse_args()
    a.width = snap(a.width)
    print(f"figure font: {style()}")
    dir_renames = {}
    for item in a.rename_dir:
        base, _, pairs = item.partition(":")
        dir_renames[base] = dict(p.split("=", 1) for p in pairs.split(",") if "=" in p)
    runs = []
    for r in load(a.inputs, dir_renames):
        share = r.get("llm_api_errors", 0) / max(r.get("llm_episodes", 0), 1)
        if share > a.max_api_errors:
            print(f"dropped {r['system']} {r['repeat']}: {share:.0%} of llm episodes failed at the model API")
        else:
            runs.append(r)
    m = Metrics(a.critical.split(","), a.latency, a.errors_as_ms)
    metrics = a.summary.split(",")
    systems = systems_of(runs)
    assign_looks(systems)
    rows = {s: [m.row(r, metrics) for r in runs if r["system"] == s] for s in systems}
    os.makedirs(a.dir, exist_ok=True)
    path = lambda name: os.path.join(a.dir, f"{name}.{a.format}")
    want = [n for n in a.only.split(",") if n] or ["summary", "cost", "phases", "timeline", "actions", "action-mix"]
    names = []
    if "summary" in want:
        fig_summary(systems, rows, metrics, m, path("summary"), a.width)
        names.append("summary")
    if "cost" in want:
        fig_cost(systems, rows, m, path("cost"), a.width, zoom_x=a.zoom_x)
        names.append("cost")
    if "phases" in want:
        fig_phases(systems, runs, m, path("phases"), a.width)
        names.append("phases")
    if "timeline" in want:
        shown = [s for s in systems if not a.timeline_systems or s in a.timeline_systems.split(",")]
        fig_timeline(shown, runs, rows, m, path("timeline"), a.width, a.smooth)
        names.append("timeline")
    if "actions" in want and fig_actions(runs, path("actions"), a.width, square=a.square, rows=rows):
        names.append("actions")
        if "action-mix" in want:
            fig_action_mix(runs, path("action-mix"), a.width)
            names.append("action-mix")
    elif "action-mix" in want and action_groups(runs)[0]:
        fig_action_mix(runs, path("action-mix"), a.width)
        names.append("action-mix")
    if "action-bars" in want and action_groups(runs)[0]:
        fig_action_bars(runs, path("action-bars"), a.width)
        names.append("action-bars")
    if "p50" in want:
        fig_bar(systems, rows, "p50", f"{m.latency} p50 (ms)", path("p50"), a.width)
        names.append("p50")
    if "p95" in want:
        fig_bar(systems, rows, "p95", f"{m.latency} p95 (ms)", path("p95"), a.width)
        names.append("p95")
    if "alloc" in want:
        fig_bar(systems, rows, "alloc_cs", "Allocated (1000 core-s)", path("alloc"), a.width, scale=1000)
        names.append("alloc")
    if "pool" in want:
        fig_bar(systems, rows, "pool_max", "Pool (CPUs)", path("pool"), a.width, fmt="{:.0f}")
        names.append("pool")
    if "errors" in want:
        fig_errors(systems, rows, path("errors"), a.width)
        names.append("errors")
    if "cdf" in want:
        fig_cdf(systems, runs, m, path("cdf"), a.width)
        names.append("cdf")
    if "tradeoff" in want:
        fig_tradeoff(systems, runs, rows, m, path("tradeoff"), a.width, a.zoom_x)
        names.append("tradeoff")
    if "speedup" in want:
        route_list = [r for r in a.speedup_routes.split(",") if r] or None
        keep = {s for s in a.speedup_systems.split(",") if s} or None
        kind = a.speedup_metric.partition(":")[0]
        stem = "speedup" if kind == "p95" else f"speedup-{kind}"
        fig_speedup(runs, m, path(stem), a.width, a.speedup_metric, a.speedup_by,
                    a.baseline, a.focus, route_list, keep, a.speedup_complete)
        names.append(stem)
    if "speedup-bars" in want or "speedup-lines" in want:
        route_list = [r for r in a.speedup_routes.split(",") if r] or None
        keep = {s for s in a.speedup_systems.split(",") if s} or None
        bar_metrics = [s for s in a.bar_metrics.split(",") if s] or ["p50", "p90"]
        if "speedup-bars" in want:
            stem = "speedup-bars" if a.width >= FULL else "speedup-bars-column"
            fig_speedup_bars(runs, m, path(stem), a.baseline, a.focus, route_list, keep, bar_metrics, a.width)
            names.append(stem)
        if "speedup-lines" in want:
            stem = "speedup-lines" if a.width >= FULL else "speedup-lines-column"
            fig_speedup_bars(runs, m, path(stem), a.baseline, a.focus, route_list, keep, bar_metrics, a.width,
                             lines=True)
            names.append(stem)
    if "pctspeedup" in want:
        pct_routes = [r for r in (a.pct_routes or a.critical).split(",") if r]
        keep = {s for s in a.speedup_systems.split(",") if s} or None
        fig_pct_speedup(runs, path("pctspeedup"), a.baseline, a.focus, pct_routes, keep)
        names.append("pctspeedup")
    print(f"{'system':10} {'n':>3} {'p50':>7} {'p95':>7} {'p99':>7} {'crit err%':>9} {'opt err%':>8} {'pool max':>8} "
          f"{'alloc kcs':>9} {'req/s':>7} {'cores':>6}")
    for s in systems:
        med = lambda k: statistics.median(r[k] for r in rows[s])
        print(f"{look(s)[3]:10} {len(rows[s]):3} {med('p50'):7.1f} {med('p95'):7.1f} {med('p99'):7.1f} "
              f"{med('crit_err'):9.2f} {med('opt_err'):8.2f} {med('pool_max'):8.0f} {med('alloc_cs') / 1000:8.2f} "
              f"{statistics.median(m.value(r, 'tput') for r in runs if r['system'] == s):7.1f} "
              f"{statistics.median(m.value(r, 'cores') for r in runs if r['system'] == s):6.2f}")
    print(f"wrote {', '.join(path(n) for n in names)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
