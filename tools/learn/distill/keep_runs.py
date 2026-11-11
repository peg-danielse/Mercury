#!/usr/bin/env python3
"""Keep llm runs whose critical-route p95 and error rate are both under a limit.

  keep_runs.py RUNS.json [RUNS.json ...] --max-p95-ms 100 --max-error-pct 1

RUNS.json is the file extract_runs.py writes. The critical routes' histograms are merged
(the same upper-bound percentile as plot_runs.pct). Failed requests are not in those
histograms, so the error rate is a separate limit: a fast p95 of the requests that
completed is not enough. Prints one kept run per line: system, repeat, p95 ms, error %,
episodes, path. The path is the dsb_ab.sh directory reconstruct.py reads.
"""

from __future__ import annotations

import argparse
import json
import sys


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


def score(run: dict, routes: list[str]) -> tuple[float, float]:
    parts = []
    for route in routes:
        parts.extend(run.get("series", {}).get(route) or [])
    m = merged(parts)
    err = 100.0 * m["errors"] / m["sent"] if m["sent"] else float("nan")
    return pct(m["hist"], 0.95), err


def runs_of(doc: dict) -> list[dict]:
    if "comparisons" in doc:
        return [r for c in doc["comparisons"] for r in c["runs"]]
    return doc.get("runs") or []


def keep(docs: list[dict], routes: list[str], max_p95: float, max_err: float, prefixes: list[str]) -> list[dict]:
    kept = []
    for doc in docs:
        for run in runs_of(doc):
            name = run.get("system") or ""
            if prefixes and not any(name == p or name.startswith(p) for p in prefixes):
                continue
            p95, err = score(run, routes)
            if p95 <= max_p95 and err <= max_err:
                kept.append({"system": name, "repeat": run.get("repeat"), "p95_ms": p95, "error_pct": err,
                             "episodes": run.get("llm_episodes"), "path": run.get("path")})
    kept.sort(key=lambda r: (r["system"], str(r["repeat"]), r["path"] or ""))
    return kept


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("runs", nargs="+")
    ap.add_argument("--critical", default="compose-post,read-home-timeline")
    ap.add_argument("--max-p95-ms", type=float, required=True)
    ap.add_argument("--max-error-pct", type=float, required=True)
    ap.add_argument("--systems", default="llm", help="comma-separated system names or prefixes (default: llm)")
    a = ap.parse_args()
    routes = [r for r in a.critical.split(",") if r]
    prefixes = [s for s in a.systems.split(",") if s]
    docs = [json.load(open(p)) for p in a.runs]
    rows = keep(docs, routes, a.max_p95_ms, a.max_error_pct, prefixes)
    print(f"{'system':16} {'rep':4} {'p95':8} {'err%':7} {'eps':5} path")
    for r in rows:
        print(f"{r['system']:16} {str(r['repeat']):4} {r['p95_ms']:8.1f} {r['error_pct']:7.2f} "
              f"{str(r['episodes']):5} {r['path']}")
    print(f"{len(rows)} runs kept", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
