#!/usr/bin/env python3
"""Baseline versus each antagonist for a low-load sensitivity run.

  ant_sensitivity_report.py OUT

OUT is the campaign directory. Each application is a compare_systems.sh tree
under OUT/<app>, and DONE names the run directory. The baseline is the
event=baseline window. Each antagonist phase is an on/off pair. The off gaps
between phases are recovery, not the baseline.

p50 and p95 are histogram upper bounds in milliseconds, over successful
requests. The ratio is that p95 divided by the baseline p95 of the same
routes. done/s is completions per second of measured window time: the
generator leaves a gap between windows, so this is not the wall-clock rate.
Onset is the first 10 seconds of the phase; held is the rest.
One run has no repeat range.
"""

from __future__ import annotations

import glob
import json
import os
import sys

PRIMARY = {
    "hotel": "hotels",
    "social": "read-home-timeline",
    "media": "compose-review",
}
CRITICAL = {
    "hotel": ["hotels", "reservation"],
    "social": ["compose-post", "read-home-timeline"],
    "media": ["read-page", "compose-review"],
}
ONSET_S = 10.0


def percentile(hist: dict, q: float) -> float | None:
    total = sum(hist.values())
    if total == 0:
        return None
    need = q * total
    seen = 0
    for bound in sorted(hist):
        seen += hist[bound]
        if seen >= need:
            return bound / 1e6
    return None


def events_of(run: str) -> list[dict]:
    path = os.path.join(run, "antagonists.jsonl")
    if not os.path.isfile(path):
        return []
    return [json.loads(line) for line in open(path)]


def baseline_span(events: list[dict]) -> tuple[int, int] | None:
    start = None
    for event in events:
        if event.get("event") != "baseline":
            continue
        if event.get("state") == "on":
            start = event["t_ns"]
        elif event.get("state") == "off" and start is not None:
            return start, event["t_ns"]
    return None


def phases_of(events: list[dict]) -> list[dict]:
    on = None
    out = []
    for event in events:
        if event.get("event") == "on":
            on = event
        elif event.get("event") == "off" and on is not None and on.get("phase") == event.get("phase"):
            out.append({**on, "t1_ns": event["t_ns"]})
            on = None
    return out


def pin_of(events: list[dict]) -> dict | None:
    for event in events:
        if event.get("event") == "pin":
            return event
    return None


def windows_of(run: str) -> list[dict]:
    path = os.path.join(run, "loadgen.hist.jsonl")
    if not os.path.isfile(path):
        return []
    return [json.loads(line) for line in open(path)]


def accumulate(windows: list[dict], routes: list[str], t0: int, t1: int) -> dict:
    hist: dict[int, int] = {}
    sent = errors = done = 0
    measured: dict[str, int] = {}
    for w in windows:
        route = w.get("route")
        if route not in routes:
            continue
        mid = (w["t0_ns"] + w["t1_ns"]) / 2
        if not (t0 <= mid < t1):
            continue
        sent += w.get("sent", 0)
        errors += w.get("errors", 0)
        done += w.get("done", 0)
        measured[route] = measured.get(route, 0) + (w["t1_ns"] - w["t0_ns"])
        for bound, count in w.get("hist") or []:
            hist[int(bound)] = hist.get(int(bound), 0) + count
    # The generator spends part of each second between windows, so the wall clock is
    # longer than the measured time. Completions per measured second is the rate.
    seconds = (sum(measured.values()) / len(measured) / 1e9) if measured else 0.0
    return {"hist": hist, "sent": sent, "errors": errors, "done": done, "seconds": seconds}


def fmt(x, spec: str) -> str:
    if x is None:
        return f"{'':>8}"
    return format(x, spec)


def row(name: str, stat: dict, base_p95: float | None) -> str:
    p50 = percentile(stat["hist"], 0.50)
    p95 = percentile(stat["hist"], 0.95)
    err = 100.0 * stat["errors"] / stat["sent"] if stat["sent"] else None
    ratio = p95 / base_p95 if p95 is not None and base_p95 else None
    rate = stat["done"] / stat["seconds"] if stat["seconds"] else None
    return (
        f"{name:<16} {stat['sent']:>7} {fmt(err, '7.2f')} {fmt(rate, '7.1f')} "
        f"{fmt(p50, '8.2f')} {fmt(p95, '8.2f')} {fmt(ratio, '8.2f')}"
    )


def report_routes(title: str, windows: list[dict], routes: list[str], span, phases: list[dict]) -> None:
    print(title)
    print(f"{'segment':<16} {'sent':>7} {'err%':>7} {'done/s':>7} {'p50':>8} {'p95':>8} {'p95/base':>8}   onset p95   held p95")
    if span is None:
        print("  no baseline window")
        return
    base = accumulate(windows, routes, span[0], span[1])
    base_p95 = percentile(base["hist"], 0.95)
    print(row("baseline", base, base_p95))
    for phase in phases:
        t0, t1 = phase["t_ns"], phase["t1_ns"]
        stat = accumulate(windows, routes, t0, t1)
        label = f"{phase['kind']} {phase['level']}"
        print(row(label, stat, base_p95), end="")
        cut = t0 + int(ONSET_S * 1e9)
        onset = accumulate(windows, routes, t0, min(cut, t1))
        held = accumulate(windows, routes, min(cut, t1), t1)
        op = percentile(onset["hist"], 0.95)
        hp = percentile(held["hist"], 0.95)
        print(f"   {fmt(op, '8.2f')}   {fmt(hp, '8.2f')}")
    print()


def report_run(app: str, label: str, run: str) -> None:
    events = events_of(run)
    windows = windows_of(run)
    span = baseline_span(events)
    phases = phases_of(events)
    pin = pin_of(events)
    print(f"== {app}  {label}")
    print(f"   run {run}")
    if pin is None:
        print("   victim: not pinned")
    else:
        print(f"   victim: {pin.get('victim')}  use {pin.get('victim_use')} cores at pin")
        cands = pin.get("candidates") or {}
        if cands:
            ranked = ", ".join(f"{name} {cores}" for name, cores in sorted(cands.items(), key=lambda kv: -kv[1]))
            print(f"   candidates: {ranked}")
    if phases:
        print("   phases:", ", ".join(
            f"{p['kind']}{p['level']}->{str(p.get('victim', '')).split('-')[-1]}@{p.get('victim_cpu')}"
            for p in phases))
    skips = [e for e in events if e.get("event") == "skip"]
    if skips:
        print(f"   skipped phases: {len(skips)}")
    report_routes(f"primary route: {PRIMARY[app]}", windows, [PRIMARY[app]], span, phases)
    report_routes("critical routes: " + ", ".join(CRITICAL[app]), windows, CRITICAL[app], span, phases)


def main() -> None:
    root = sys.argv[1] if len(sys.argv) > 1 else "data/dsb/compare/ant-sensitivity-20261110"
    found = False
    for app in ("hotel", "social", "media"):
        app_dir = os.path.join(root, app)
        dones = sorted(glob.glob(os.path.join(app_dir, "*", "r*", "DONE")))
        if not dones:
            print(f"== {app}  no DONE under {app_dir}")
            print()
            continue
        found = True
        for done in dones:
            run = open(done).read().strip()
            label = os.path.relpath(os.path.dirname(done), app_dir)
            if os.path.isdir(run):
                report_run(app, label, run)
            else:
                print(f"== {app}  {label}  missing run {run}")
                print()
    if not found:
        sys.exit(1)


if __name__ == "__main__":
    main()
