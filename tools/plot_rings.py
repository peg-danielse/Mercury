#!/usr/bin/env python3
"""Allocation against use, per daemon, from Mercury's 10 ms cgroup ring archives (rings/<id>.mring).

    plot_rings.py RINGS_DIR [--prefix hotel-] [--from S] [--to S] [--smooth MS] [--out FILE] [--show]

Each sample (every kSampleMs = 10 ms) holds the daemon cgroup's task-clock, user and system time in
that gap, the cpu.pressure "some" time (or the runnable count without PSI) and, from Mercury builds
after 2026-10-19, the cpuset size ("cores"). Plotted per daemon:
  cores     cpuset size (allocation), a step line
  used      task-clock / gap: cores actually busy
  pressure  pressure / gap: share of the gap in which at least one thread waited for a CPU
            (unmet demand; it does not say how many threads waited)
The top panel sums used and cores over the daemons on one 10 ms grid. Archives can hold several
runs (Mercury appends); only the run that holds the newest sample is used, cut at gaps over 2 s.
Prints, per daemon: core-seconds allocated and used, utilisation, and the share of time under
pressure (>= 0.10, saturate's add threshold) while every allocated core was busy (>= 0.85).
"""
import argparse
import glob
import os
import struct
import sys

import numpy as np

MAGIC = b"MRING1\0\0"
HEADER = 40
STEP_NS = 10_000_000
RUN_GAP_NS = 2_000_000_000


def read_mring(path):
    data = open(path, "rb").read()
    if data[:8] != MAGIC:
        raise ValueError(f"{path}: not a Mercury ring archive")
    _, count, max_counters, record = struct.unpack_from("<4I", data, 8)
    committed, offset = struct.unpack_from("<2Q", data, 24)
    names, at = [], HEADER
    for _ in range(count):
        (size,) = struct.unpack_from("<I", data, at)
        names.append(data[at + 4:at + 4 + size].decode())
        at += 4 + size
    committed = min(committed, (len(data) - offset) // record)
    rows = np.frombuffer(data, dtype=np.uint64, count=committed * record // 8, offset=offset)
    rows = rows.reshape(committed, record // 8)
    ts = rows[:, 0].astype(np.int64)
    values = rows[:, 4:4 + max_counters][:, :count].astype(np.float64)
    return names, ts, {name: values[:, i] for i, name in enumerate(names)}


def last_run(ts):
    """Index range of the run holding the newest sample."""
    breaks = np.flatnonzero(np.diff(ts) > RUN_GAP_NS)
    start = int(breaks[-1]) + 1 if len(breaks) else 0
    return start, len(ts)


def smooth(x, ms):
    n = max(1, int(round(ms / (STEP_NS / 1e6))))
    if n == 1 or len(x) < n:
        return x
    return np.convolve(x, np.ones(n) / n, mode="same")


def on_grid(idx, values, n):
    """values at grid slots idx, held until the next sample, 0 outside [idx[0], idx[-1]]."""
    filled = np.full(n, np.nan)
    filled[idx] = values
    last = np.where(~np.isnan(filled), np.arange(n), 0)
    np.maximum.accumulate(last, out=last)
    filled = filled[last]
    filled[: idx[0]] = 0
    filled[idx[-1] + 1:] = 0
    return np.nan_to_num(filled)


def load(rings_dir, prefix):
    daemons = {}
    for path in sorted(glob.glob(os.path.join(rings_dir, prefix + "*.mring"))):
        names, ts, cols = read_mring(path)
        if len(ts) < 2:
            continue
        lo, hi = last_run(ts)
        daemons[os.path.basename(path)[:-len(".mring")]] = (names, ts[lo:hi], {k: v[lo:hi] for k, v in cols.items()})
    return daemons


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("rings_dir")
    ap.add_argument("--prefix", default="", help="only daemons whose id starts with this (e.g. hotel-)")
    ap.add_argument("--from", dest="t_from", type=float, default=None, help="seconds from the start")
    ap.add_argument("--to", dest="t_to", type=float, default=None, help="seconds from the start")
    ap.add_argument("--smooth", type=float, default=0, help="moving mean in ms (0: raw 10 ms samples)")
    ap.add_argument("--out", default=None, help="PNG path (default RINGS_DIR/alloc_vs_use.png)")
    ap.add_argument("--show", action="store_true", help="open an interactive window to zoom in")
    args = ap.parse_args()

    daemons = load(args.rings_dir, args.prefix)
    if not daemons:
        sys.exit(f"no {args.prefix}*.mring with samples in {args.rings_dir}")
    # The window is the newest run; daemons from older runs (ending before it starts) are dropped.
    end = max(ts[-1] for _, ts, _ in daemons.values())
    start = min(ts[0] for _, ts, _ in daemons.values() if ts[-1] > end - RUN_GAP_NS * 30)
    daemons = {d: v for d, v in daemons.items() if v[1][-1] >= start}
    t_from = start + int((args.t_from or 0) * 1e9)
    t_to = start + int(args.t_to * 1e9) if args.t_to is not None else end

    grid = np.arange(t_from, t_to + 1, STEP_NS)
    total_used = np.zeros(len(grid))
    total_cores = np.zeros(len(grid))
    has_cores = False
    series, summary = {}, []
    for d, (names, ts, cols) in sorted(daemons.items()):
        gap = np.diff(ts).astype(np.float64)
        t = ts[1:]
        keep = (t >= t_from) & (t <= t_to) & (gap > 0)
        t, gap = t[keep], gap[keep]
        used = cols["task-clock"][1:][keep] / gap
        if "pressure" in cols:
            pressure = cols["pressure"][1:][keep] / gap
        else:
            pressure = None  # runnable fallback: a thread count, not a share
        cores = cols["cores"][1:][keep] if "cores" in cols else None
        if len(t) == 0:
            continue
        idx = np.clip((t - t_from) // STEP_NS, 0, len(grid) - 1).astype(int)
        # Samples jitter around 10 ms, so a grid slot can hold none or two: hold each value until the next.
        total_used += on_grid(idx, used, len(grid))
        if cores is not None:
            has_cores = True
            total_cores += on_grid(idx, cores, len(grid))
        series[d] = (t, used, pressure, cores)
        seconds = gap.sum() / 1e9
        used_s = float((used * gap).sum() / 1e9)
        row = {"daemon": d, "seconds": seconds, "used_core_s": used_s}
        if cores is not None:
            alloc_s = float((cores * gap).sum() / 1e9)
            row.update(alloc_core_s=alloc_s, util=used_s / alloc_s if alloc_s else 0,
                       mean_cores=float(cores.mean()), max_cores=int(cores.max()))
            if pressure is not None:
                short = (pressure >= 0.10) & (used >= 0.85 * cores)
                row["short_pct"] = 100.0 * float(gap[short].sum() / gap.sum())
        if pressure is not None:
            row["pressure_pct"] = 100.0 * float(gap[pressure >= 0.10].sum() / gap.sum())
        summary.append(row)

    print(f"window {(t_to - t_from) / 1e9:.1f} s, {len(series)} daemons, 10 ms samples"
          + ("" if has_cores else "  (no cores column: archive from an older Mercury)"))
    print(f"{'daemon':28s} {'alloc core-s':>12s} {'used core-s':>11s} {'util':>6s} {'mean/max cores':>15s}"
          f" {'pressure>=.1':>13s} {'short':>7s}")
    for r in summary:
        alloc = f"{r['alloc_core_s']:12.1f}" if "alloc_core_s" in r else f"{'-':>12s}"
        util = f"{100 * r['util']:5.0f}%" if "util" in r else f"{'-':>6s}"
        cores = f"{r['mean_cores']:8.2f}/{r['max_cores']:<6d}" if "mean_cores" in r else f"{'-':>15s}"
        pres = f"{r['pressure_pct']:12.1f}%" if "pressure_pct" in r else f"{'-':>13s}"
        short = f"{r['short_pct']:6.1f}%" if "short_pct" in r else f"{'-':>7s}"
        print(f"{r['daemon']:28s} {alloc} {r['used_core_s']:11.1f} {util} {cores} {pres} {short}")
    print("util = used/allocated core-seconds; pressure = time with a thread waiting for a CPU;"
          " short = pressure while every allocated core was >= 85% busy (demand above allocation)")

    import matplotlib
    if not args.show:
        matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    n = len(series) + 1
    fig, axes = plt.subplots(n, 1, figsize=(14, 1.9 * n + 1), sharex=True, squeeze=False)
    axes = axes[:, 0]
    secs = (grid - t_from) / 1e9
    ax = axes[0]
    ax.plot(secs, smooth(total_used, args.smooth), lw=0.6, color="C0", label="used (sum)")
    if has_cores:
        ax.step(secs, total_cores, where="post", lw=1.2, color="k", label="cores (sum of cpusets)")
    ax.set_ylabel("all daemons\ncores")
    ax.legend(loc="upper left", fontsize=8, ncol=2)
    ax.grid(alpha=0.3)
    for ax, (d, (t, used, pressure, cores)) in zip(axes[1:], series.items()):
        x = (t - t_from) / 1e9
        if pressure is not None:
            twin = ax.twinx()
            twin.fill_between(x, 0, smooth(pressure, args.smooth), step="post", color="C3", alpha=0.25, lw=0)
            twin.set_ylim(0, 1)
            twin.set_yticks([0, 1])
            twin.tick_params(labelsize=7, colors="C3")
        ax.plot(x, smooth(used, args.smooth), lw=0.6, color="C0")
        if cores is not None:
            ax.step(x, cores, where="post", lw=1.2, color="k")
        ax.set_ylabel(d, rotation=0, ha="right", va="center", fontsize=8)
        ax.set_ylim(bottom=0)
        ax.grid(alpha=0.3)
    axes[-1].set_xlabel("seconds")
    label = f"10 ms samples, moving mean {args.smooth:g} ms" if args.smooth else "raw 10 ms samples"
    fig.suptitle(f"cores allocated (black) vs used (blue), pressure share (red, right axis): {label}", fontsize=10)
    fig.tight_layout(rect=(0, 0, 1, 0.985))
    out = args.out or os.path.join(args.rings_dir, "alloc_vs_use.png")
    fig.savefig(out, dpi=120)
    print(f"wrote {out}")
    if args.show:
        plt.show()


if __name__ == "__main__":
    main()
