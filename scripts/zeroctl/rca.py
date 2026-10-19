#!/usr/bin/env python3
"""RCA step that zeroctl runs before it builds the prompt.

Inputs, all optional except the rings:

* rings (always): the group loads and daemon cpusets Mercury puts in the state.
* gnn (state["rca"]["gnn"]): Paul's per-thread CPU-demand GNN (gnn-example/mercury
  train_gnn.py, LocalGNN_DB K=3 H=16, model models/gnn_v5.json). Mercury runs it in C++
  (src/gnn.cpp, src/gnn_rca.cpp) every 10 ms on each zeroctl daemon's threads, observe only,
  and FFD-packs the predicted per-thread demand into the cores the daemon needs. Here that
  becomes "which daemon is CPU-starved and by how much". The GNN estimates demand; it does not
  infer causality between services.
* jaeger (DeathStarBench-style workloads only): Jaeger query API traces. Ported from
  infrabench rca/controller/detect: blame per anomalous trace = the service of its longest
  span (spans.py dominant_service), ranked by share (llm/situation.py format_ranking).
  infrabench flags anomalous traces with per-pattern Isolation Forests trained on healthy
  traces (scikit-learn); that model is not available here, so this step uses a labelled
  latency threshold instead (the healthy reference p95 if known, else median + 3 MAD).

Without the GNN the localization falls back to a ring-load ranking, labelled as a heuristic.
Standard library only.
"""

from __future__ import annotations

import json
import os
import statistics
import time
from typing import Any
from urllib.error import URLError
from urllib.parse import quote
from urllib.request import urlopen

DEFAULT_JAEGER = "http://localhost:16686"  # DeathStarBench docker-compose / k8s port-forward
JAEGER_TIMEOUT_S = 3.0
JAEGER_LOOKBACK_S = 60
JAEGER_LIMIT = 200
STARVED_UNDER_SHARE = 0.2


def _latest(group: dict[str, Any]) -> float | None:
    loads = [v for v in group.get("loads") or [] if v is not None]
    return float(loads[-1]) if loads else None


def _mean(values: list[float]) -> float | None:
    return sum(values) / len(values) if values else None


# --------------------------------------------------------------------------- rings


def ring_ranking(state: dict[str, Any]) -> list[dict[str, Any]]:
    """Heuristic: daemons ranked by the mean latest load of the core groups their CPUs sit on."""
    groups = state.get("groups") or []
    core_load: dict[int, float] = {}
    for group in groups:
        if group.get("level") == "core":
            load = _latest(group)
            if load is not None:
                for cpu in group.get("linux_cpus") or []:
                    core_load[int(cpu)] = load
    by_name = {g["name"]: g for g in groups}
    level = state.get("move_level") or "node"
    rows = []
    for daemon in state.get("daemons") or []:
        cpus = [int(c) for c in daemon.get("cpus") or []]
        own = _mean([core_load[c] for c in cpus if c in core_load])
        placed = [by_name[g] for g in daemon.get("groups") or [] if (by_name.get(g) or {}).get("level") == level]
        group_load = max((_latest(g) or 0.0 for g in placed), default=None)
        score = own if own is not None else (group_load or 0.0)
        rows.append({"daemon_id": daemon["daemon_id"], "cpus": len(cpus), "own_load": own,
                     "group": placed[0]["name"] if placed else None, "group_load": group_load, "score": score})
    rows.sort(key=lambda r: (-r["score"], r["daemon_id"]))
    return rows


# --------------------------------------------------------------------------- gnn


def gnn_localization(gnn: dict[str, Any] | None) -> tuple[list[dict[str, Any]], str]:
    """Daemons ranked by GNN core deficit. Returns (rows, status)."""
    if not gnn:
        return [], "not reported by Mercury"
    if not gnn.get("enabled"):
        return [], "off (zeroctl.gnn false)"
    if not gnn.get("available"):
        return [], f"unavailable: {gnn.get('error') or 'model not loaded'} (model {gnn.get('model') or '?'})"
    rows = []
    for d in gnn.get("daemons") or []:
        if not d.get("ticks"):
            continue
        cpus = int(d.get("cpus") or 0)
        need = float(d.get("required_mean") or 0.0)
        rows.append({**d, "deficit": round(need - cpus, 3),
                     "starved": need > cpus or float(d.get("under_share") or 0.0) >= STARVED_UNDER_SHARE})
    if not rows:
        return [], "no ticks yet (the sampler starts when zeroctl attaches)"
    rows.sort(key=lambda r: (-r["deficit"], -float(r.get("under_share") or 0), r["daemon_id"]))
    return rows, "ok"


# --------------------------------------------------------------------------- jaeger


def _get_json(url: str) -> Any:
    with urlopen(url, timeout=JAEGER_TIMEOUT_S) as response:
        return json.loads(response.read().decode("utf-8"))


def jaeger_url(mode: str | None) -> str | None:
    """None when Jaeger is off. auto: JAEGER_URL, else the DeathStarBench default."""
    mode = (mode or "auto").strip()
    if mode == "off" or os.environ.get("ZEROCTL_JAEGER") == "off":
        return None
    if mode.startswith(("http://", "https://")):
        return mode.rstrip("/")
    return (os.environ.get("JAEGER_URL") or DEFAULT_JAEGER).rstrip("/")


def fetch_traces(base: str, lookback_s: int = JAEGER_LOOKBACK_S, limit: int = JAEGER_LIMIT) -> list[dict[str, Any]]:
    services = (_get_json(f"{base}/api/services") or {}).get("data") or []
    services = [s for s in services if s and not str(s).startswith("jaeger")]
    end = int(time.time() * 1e6)
    start = end - lookback_s * 1_000_000
    seen: dict[str, dict[str, Any]] = {}
    for service in services[:40]:
        url = f"{base}/api/traces?service={quote(str(service))}&start={start}&end={end}&limit={limit}"
        for trace in (_get_json(url) or {}).get("data") or []:
            seen.setdefault(str(trace.get("traceID")), trace)
    return list(seen.values())


def trace_rows(traces: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Per trace: total duration (root span) and the service of the longest non-root span."""
    rows = []
    for trace in traces:
        spans = trace.get("spans") or []
        if not spans:
            continue
        processes = trace.get("processes") or {}
        ids = {s.get("spanID") for s in spans}

        def is_root(span: dict[str, Any]) -> bool:
            return not any(r.get("refType") == "CHILD_OF" and r.get("spanID") in ids for r in span.get("references") or [])

        roots = [s for s in spans if is_root(s)]
        total = max((s.get("duration") or 0) for s in (roots or spans))
        inner = [s for s in spans if not is_root(s)] or spans
        longest = max(inner, key=lambda s: s.get("duration") or 0)
        service = (processes.get(longest.get("processID")) or {}).get("serviceName") or "?"
        rows.append({"trace": trace.get("traceID"), "total_us": total, "service": service,
                     "operation": longest.get("operationName")})
    return rows


def jaeger_ranking(rows: list[dict[str, Any]], reference_p95_ms: float | None) -> dict[str, Any]:
    if not rows:
        return {"traces": 0, "anomalous": 0, "ranking": [], "threshold_ms": None, "method": ""}
    totals = [r["total_us"] / 1000.0 for r in rows]
    if reference_p95_ms:
        threshold = float(reference_p95_ms)
        method = "jaeger longest-span blame, traces above the healthy p95 (heuristic, not infrabench's Isolation Forest)"
    else:
        median = statistics.median(totals)
        mad = statistics.median([abs(t - median) for t in totals]) or 0.0
        threshold = median + 3 * mad if mad else max(totals) + 1  # no spread: nothing is anomalous
        method = "jaeger longest-span blame, traces above median + 3 MAD (heuristic, not infrabench's Isolation Forest)"
    anomalous = [r for r, t in zip(rows, totals) if t > threshold]
    counts: dict[str, int] = {}
    for r in anomalous:
        counts[r["service"]] = counts.get(r["service"], 0) + 1
    ranking = sorted(counts.items(), key=lambda kv: (-kv[1], kv[0]))
    return {"traces": len(rows), "anomalous": len(anomalous), "ranking": ranking,
            "threshold_ms": round(threshold, 3), "p50_ms": round(statistics.median(totals), 3), "method": method}


def jaeger_section(mode: str | None, situation: dict[str, Any] | None) -> dict[str, Any]:
    base = jaeger_url(mode)
    if base is None:
        return {"status": "off"}
    canned = os.environ.get("ZEROCTL_JAEGER_TRACES")  # tests and offline replay: a Jaeger /api/traces JSON file
    try:
        if canned:
            with open(canned, encoding="utf-8") as handle:
                traces = (json.load(handle) or {}).get("data") or []
        else:
            traces = fetch_traces(base)
    except (OSError, URLError, ValueError) as exc:
        explicit = (mode or "auto") != "auto"
        why = f"{base} not reachable ({exc.__class__.__name__})"
        return {"status": ("error: " if explicit else "skipped: no Jaeger, not a traced (DSB) workload; ") + why}
    reference = ((situation or {}).get("reference") or {}).get("p95_ms")
    result = jaeger_ranking(trace_rows(traces), reference)
    result["status"] = "ok" if result["traces"] else "skipped: Jaeger has no traces in the last window"
    result["url"] = base if not canned else f"file {canned}"
    return result


# --------------------------------------------------------------------------- step


def run_rca(state: dict[str, Any], situation: dict[str, Any] | None = None) -> dict[str, Any]:
    rca = state.get("rca") or {}
    gnn_rows, gnn_status = gnn_localization(rca.get("gnn"))
    ring_rows = ring_ranking(state)
    jaeger = jaeger_section(rca.get("jaeger"), situation)
    model = os.path.basename(str((rca.get("gnn") or {}).get("model") or "")) or "gnn"
    if gnn_rows:
        method = f"GNN per-thread demand ({model})"
        localization = [r["daemon_id"] for r in gnn_rows if r["starved"]]
    else:
        method = "heuristic ring-load ranking (no GNN)"
        localization = [r["daemon_id"] for r in ring_rows if r["score"] > 0.75]
    return {"method": method, "localized": localization, "gnn_status": gnn_status, "gnn": gnn_rows,
            "gnn_window_s": (rca.get("gnn") or {}).get("window_s"), "rings": ring_rows, "jaeger": jaeger,
            "application": rca.get("application") or ""}


def _pct(value: float | None) -> str:
    return "--" if value is None else f"{value * 100:.0f}%"


def format_rca(result: dict[str, Any]) -> str:
    lines = []
    where = ", ".join(result["localized"]) or "no daemon"
    lines.append(f"localization [{result['method']}]: {where}")
    if result["gnn"]:
        lines.append(f"GNN demand over the last {result.get('gnn_window_s') or '?'} s (cores from FFD of predicted per-thread demand):")
        for r in result["gnn"]:
            lines.append(
                f"  {r['daemon_id']}  holds {r['cpus']} cpus, needs {float(r['required_mean']):.1f} "
                f"(p90 {r['required_p90']}, max {r['required_max']}), under-provisioned {_pct(r.get('under_share'))} of "
                f"{r['ticks']} ticks, predicted demand {float(r['pred_demand_mean']):.2f} cores, "
                f"{float(r['threads_mean']):.0f} threads{'  STARVED' if r['starved'] else ''}"
            )
    else:
        lines.append(f"GNN: {result['gnn_status']}")
    lines.append("ring load (heuristic, busiest first): " + ("; ".join(
        f"{r['daemon_id']} own cpus {_pct(r['own_load'])}, {r['group'] or '?'} {_pct(r['group_load'])}"
        for r in result["rings"]) or "no daemons"))
    jaeger = result["jaeger"]
    if jaeger.get("status") == "ok":
        whole = jaeger["anomalous"] or 1
        ranked = ", ".join(f"{name} ({count / whole * 100:.0f}%)" for name, count in jaeger["ranking"]) or "none"
        lines.append(
            f"traces: {jaeger['anomalous']} of {jaeger['traces']} above {jaeger['threshold_ms']} ms "
            f"(p50 {jaeger['p50_ms']} ms); where the degradation shows, most affected first: {ranked}  [{jaeger['method']}]"
        )
    elif jaeger.get("status") == "off":
        lines.append("traces: Jaeger off")
    else:
        lines.append(f"traces: {jaeger.get('status')}")
    return "\n".join(lines)
