#!/usr/bin/env python3
"""Run the infrabench controller baselines against a Mercury worker and score them.

One arm per policy, same shape as infrabench's adapt loop
(rca/controller/adapt.py): warm up, inject a CPU fault on the victim group,
let the policy act, hold for the stabilization time, then score the window
with the fault still up. Scores mirror rca/controller/score.py:
p50_us, p95_us, error_rate, latency_traces (here: requests), plus cost
(CPUs held by the service daemons, Mercury's analogue of replicas/CPU requests).

Must run as root (Mercury writes /sys/fs/cgroup). Python 3.8+, stdlib only.

  sudo python3 scripts/policy_experiment.py --label smoke
"""

from __future__ import annotations

import argparse
import csv
import http.client
import json
import os
import shutil
import signal
import socket
import statistics
import subprocess
import sys
import threading
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# infrabench registry order (rca/controller/policy/registry.py).
ARMS = (
    "none",
    "scale_nodes_ind",
    "reactive_move",
    "degrade_optional",
    "scale_out",
    "scale_out_spread",
    "combined",
    "zeroctl",
)
# Optional component cost the degrade policy subtracts (operator fraction, policies.md).
EXTRA_COST = 0.3


def percentile(values: list[float], pct: float) -> float:
    """Same interpolation as rca/controller/score.py."""
    if not values:
        return 0.0
    ordered = sorted(values)
    if len(ordered) == 1:
        return float(ordered[0])
    rank = (len(ordered) - 1) * (pct / 100.0)
    low = int(rank)
    high = min(low + 1, len(ordered) - 1)
    frac = rank - low
    return ordered[low] * (1.0 - frac) + ordered[high] * frac


class Mercury:
    def __init__(self, binary: Path, port: int, cpus: list[int] | None, rings: Path, log: Path,
                 extra_env: dict[str, str] | None = None):
        self.port = port
        self.rings = rings
        if rings.exists():
            shutil.rmtree(rings)
        rings.mkdir(parents=True)
        env = dict(os.environ, MERCURY_RINGS=str(rings), **(extra_env or {}))
        args = [str(binary), str(port)] + [str(c) for c in (cpus or [])]
        self.log_handle = open(log, "w")
        self.proc = subprocess.Popen(args, stdout=self.log_handle, stderr=subprocess.STDOUT, env=env)
        deadline = time.time() + 10
        while time.time() < deadline:
            try:
                self.get("/ping")
                return
            except OSError:
                time.sleep(0.1)
        raise RuntimeError("mercury did not start; see " + str(log))

    def request(self, method: str, path: str, body: dict | None = None) -> tuple[int, str]:
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=30)
        payload = json.dumps(body) if body is not None else None
        conn.request(method, path, body=payload, headers={"Content-Type": "application/json"})
        resp = conn.getresponse()
        text = resp.read().decode()
        conn.close()
        return resp.status, text

    def get(self, path: str):
        status, text = self.request("GET", path)
        if status != 200:
            raise RuntimeError(f"GET {path} -> {status} {text}")
        return json.loads(text) if text.startswith("{") else text

    def post(self, path: str, body: dict, ok=(200,)):
        status, text = self.request("POST", path, body)
        if status not in ok:
            raise RuntimeError(f"POST {path} {body} -> {status} {text}")
        return json.loads(text) if text.startswith("{") else text

    def stop(self, daemon_ids: list[str]):
        for daemon_id in daemon_ids:
            try:
                self.post("/kill", {"daemon_id": daemon_id}, ok=(200, 400, 404))
            except Exception:
                pass
        self.proc.send_signal(signal.SIGTERM)
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        self.log_handle.close()
        for daemon_id in daemon_ids:
            cg = Path("/sys/fs/cgroup/mercury") / daemon_id
            try:
                cg.rmdir()
            except OSError:
                pass


class Load:
    """Closed-loop clients on keep-alive connections to /cpu of each service."""

    def __init__(self, ports: list[int], threads_per_service: int, think_ms: float):
        self.rows: list[tuple[float, float, bool]] = []  # (t, latency_us, failed)
        self.mu = threading.Lock()
        self.stop = threading.Event()
        self.threads = [
            threading.Thread(target=self._client, args=(port, think_ms), daemon=True)
            for port in ports
            for _ in range(threads_per_service)
        ]
        for thread in self.threads:
            thread.start()

    def _client(self, port: int, think_ms: float):
        conn = None
        while not self.stop.is_set():
            start = time.monotonic()
            failed = False
            try:
                if conn is None:
                    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
                conn.request("GET", "/cpu")
                resp = conn.getresponse()
                resp.read()
                failed = resp.status != 200
            except (OSError, http.client.HTTPException):
                failed = True
                if conn is not None:
                    conn.close()
                conn = None
            end = time.monotonic()
            with self.mu:
                self.rows.append((end, (end - start) * 1e6, failed))
            if failed:
                time.sleep(0.05)
            elif think_ms > 0:
                time.sleep(think_ms / 1000.0)

    def window(self, start: float, end: float) -> dict:
        with self.mu:
            rows = [r for r in self.rows if start <= r[0] < end]
        latencies = [r[1] for r in rows if not r[2]]
        failed = sum(1 for r in rows if r[2])
        return {
            "p50_us": round(percentile(latencies, 50)),
            "p95_us": round(percentile(latencies, 95)),
            "error_rate": round(failed / len(rows), 4) if rows else 0.0,
            "latency_traces": len(rows),
            "throughput_rps": round(len(latencies) / max(end - start, 1e-9), 2),
            "mean_us": round(statistics.fmean(latencies)) if latencies else 0,
        }

    def close(self):
        self.stop.set()
        for thread in self.threads:
            thread.join(timeout=12)


def groups_by(mapping: dict, level: str) -> list[dict]:
    return [g for g in mapping["groups"] if g["level"] == level]


def pick_level(mapping: dict, wanted: str) -> str:
    if wanted != "auto":
        return wanted
    for level in ("node", "l3", "core"):
        usable = [g for g in groups_by(mapping, level) if set(g["linux_cpus"]) & set(mapping["allocatable"])]
        if len(usable) >= 2:
            return level
    return "core"


def service_cpus(mapping: dict, svc: str) -> list[int]:
    for daemon in mapping["daemons"]:
        if daemon["daemon_id"] == svc:
            return daemon["cpus"]
    return []


def arm_documents(arm: str, svc: str, level: str, ceiling: str, llm: str = "mock",
                  every: int = 10) -> tuple[dict | None, dict | None]:
    """(policies document, scale document) for one service, per arm."""
    if arm == "none":
        return None, None
    if arm == "scale_nodes_ind":
        # Calzarossa et al.: mean CPU over the window above 0.8. Mercury cannot add a machine;
        # it moves the daemon off the overloaded group (policies.md Baselines table).
        return {"daemon_id": svc, "shift": {"trigger": "mean", "theta": 0.80, "level": level}}, None
    if arm == "reactive_move":
        # Sandpiper: 3 of the last 5 intervals above 0.75, move to least loaded legal group.
        return {"daemon_id": svc, "shift": {"trigger": "hotspot", "util": 0.75, "level": level}}, None
    if arm == "degrade_optional":
        return {"daemon_id": svc, "degrade": {"limit": 0.80, "level": level, "components": [{"name": "extra", "cost": EXTRA_COST}]}}, None
    if arm == "scale_out":
        # Two replicas, hostname pin kept: two CPUs inside the group the service already sits in.
        return {"daemon_id": svc, "restrict": {"ceiling": ceiling}}, {"daemon_id": svc, "min": 2, "max": 2}
    if arm == "scale_out_spread":
        # Pin dropped: the second CPU may come from anywhere in the pool.
        return None, {"daemon_id": svc, "min": 2, "max": 2}
    if arm == "combined":
        # rca/controller/policy/combined.py staggers 0.80 (scale), 0.85 (move), 0.90 (degrade).
        return {
            "daemon_id": svc,
            "shift": {"trigger": "either", "theta": 0.80, "util": 0.85, "level": level},
            "degrade": {"limit": 0.90, "level": level, "components": [{"name": "extra", "cost": EXTRA_COST}]},
        }, None
    if arm == "zeroctl":
        # infrabench --policy zeroctl: the model picks one cross-layer action per call.
        return {"daemon_id": svc, "zeroctl": {"llm": llm, "every": every, "level": level,
                                              "components": [{"name": "extra", "cost": EXTRA_COST}]}}, None
    raise ValueError(arm)


def resolve_llm(wanted: str) -> str:
    """auto: openai when OPENAI_API_KEY is set, else gemini when GEMINI_API_KEY is set, else none."""
    if wanted != "auto":
        return wanted
    if os.environ.get("OPENAI_API_KEY"):
        return "openai"
    if os.environ.get("GEMINI_API_KEY"):
        return "gemini"
    return "none"


def write_situation(path: Path, load: "Load", reference: dict, seconds: int) -> None:
    now_end = time.monotonic()
    now = load.window(now_end - seconds, now_end)
    doc = {
        "application": "hello_server services under Mercury",
        "now": {"seconds": seconds, "requests": now["latency_traces"], "p50_ms": round(now["p50_us"] / 1000, 1),
                "p95_ms": round(now["p95_us"] / 1000, 1), "error_rate": now["error_rate"],
                "requests_per_s": now["throughput_rps"]},
        "reference": {"p50_ms": round(reference["p50_us"] / 1000, 1), "p95_ms": round(reference["p95_us"] / 1000, 1),
                      "error_rate": reference["error_rate"], "requests_per_s": reference["throughput_rps"]},
        "load": "closed-loop /cpu clients, steady",
    }
    tmp = path.with_suffix(".tmp")
    tmp.write_text(json.dumps(doc) + "\n")
    tmp.replace(path)


def cost(mapping: dict, services: list[str]) -> dict:
    cpus = sum(len(service_cpus(mapping, s)) for s in services)
    return {"replicas": len(services), "cpus": cpus}


def run_arm(arm: str, args, out_root: Path) -> dict:
    out_dir = out_root / arm
    out_dir.mkdir(parents=True, exist_ok=True)
    extra_env: dict[str, str] = {}
    llm = resolve_llm(args.llm)
    situation_file = out_dir / "situation.json"
    if arm == "zeroctl":
        if llm == "none":
            row = {"policy": arm, "skipped": "zeroctl needs a model: set OPENAI_API_KEY or GEMINI_API_KEY "
                   "(and pass it through sudo with --preserve-env), or use --llm mock"}
            (out_dir / "last_adaptation.json").write_text(json.dumps(row, indent=2) + "\n")
            print(f"[{arm}] skipped: {row['skipped']}")
            return row
        extra_env["MERCURY_ZEROCTL"] = str(REPO / "scripts" / "zeroctl" / "zeroctl.py")
        extra_env["ZEROCTL_SITUATION_FILE"] = str(situation_file)

    build = Path(args.build).resolve()
    mercury = Mercury(build / "mercury", args.port, args.cpus, out_dir / "rings", out_dir / "mercury.log", extra_env)
    services = [f"svc{i}" for i in range(args.services)]
    started = list(services)
    load = None
    try:
        mapping = mercury.get("/map")
        level = pick_level(mapping, args.level)
        allocatable = mapping["allocatable"]
        level_groups = [g for g in groups_by(mapping, level) if set(g["linux_cpus"]) & set(allocatable)]
        victim = level_groups[0] if args.victim is None else next(g for g in mapping["groups"] if g["name"] == args.victim)
        victim_cpus = [c for c in victim["linux_cpus"] if c in allocatable]
        if not victim_cpus:
            raise RuntimeError(f"victim group {victim['name']} has no allocatable CPU")
        # The scale_out ceiling: the parent group one level up when the victim is a single core.
        ceiling = victim["name"] if len(victim_cpus) >= 2 or not victim["parent"] else victim["parent"]
        hello = build / "test" / "hello_server"
        for i, svc in enumerate(services):
            mercury.post("/start", {
                "daemon_id": svc,
                "exec_path": str(hello),
                "cpu": victim_cpus[i % len(victim_cpus)],
                "listen_port": args.service_port + i,
                "env": {"MERCURY_POLICY": str(out_dir / "rings" / f"{svc}.policy")},
            })
        time.sleep(0.5)
        mapping = mercury.get("/map")
        cost_before = cost(mapping, services)
        for svc in services:
            policies, scale = arm_documents(arm, svc, level, ceiling, llm, args.zeroctl_every)
            if policies is not None:
                mercury.post("/policies", policies)
            if scale is not None:
                mercury.post("/scale", scale)
        load = Load([args.service_port + i for i in range(len(services))], args.clients, args.think_ms)
        t0 = time.monotonic()
        print(f"[{arm}] level={level} victim={victim['name']} cpus={victim_cpus} warmup {args.warmup}s")
        time.sleep(args.warmup)
        t_inject = time.monotonic()
        burn_threads = args.burn_threads or len(victim_cpus) * 2
        mercury.post("/start", {
            "daemon_id": "inject",
            "exec_path": str(build / "test" / "burn"),
            "cpu": victim_cpus[0],
            "env": {"BURN_THREADS": str(burn_threads)},
        })
        started.append("inject")
        mercury.post("/cpus", {"daemon_id": "inject", "cpus": ",".join(str(c) for c in victim_cpus)})
        print(f"[{arm}] inject burn x{burn_threads} on {victim['name']}; hold {args.hold}s, score {args.measure}s")
        reference = load.window(max(t0, t_inject - args.measure), t_inject)
        deadline = time.monotonic() + args.hold + args.measure
        while time.monotonic() < deadline:
            if arm == "zeroctl":
                write_situation(situation_file, load, reference, 10)
            time.sleep(min(2.0, max(deadline - time.monotonic(), 0)))
        t_end = time.monotonic()
        baseline = load.window(max(t0, t_inject - args.measure), t_inject)
        score = load.window(t_end - args.measure, t_end)
        during = load.window(t_inject, t_end - args.measure)
        state = mercury.get("/policies")
        mapping = mercury.get("/map")
        applied = [e for e in state["events"] if e.get("action") not in ("attach", "detach")]
        row = {
            "policy": arm,
            "mercury_level": level,
            "llm": llm if arm == "zeroctl" else None,
            "victim": victim["name"],
            "applied": applied,
            "errors": [],
            "services": services,
            "cost_before": cost_before,
            "cost_after": cost(mapping, services),
            "baseline": baseline,
            "stabilization": during,
            "score": score,
            "placement_after": {s: service_cpus(mapping, s) for s in services},
            "policy_state": state["daemons"],
            "at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        }
        with open(out_dir / "adaptations.jsonl", "a") as handle:
            handle.write(json.dumps(row) + "\n")
        (out_dir / "last_adaptation.json").write_text(json.dumps(row, indent=2) + "\n")
        (out_dir / "policies.json").write_text(json.dumps(state, indent=2) + "\n")
        print(f"[{arm}] baseline p95={baseline['p95_us']}us fault p95={score['p95_us']}us "
              f"err={score['error_rate']} actions={len(applied)} cpus {row['cost_before']['cpus']}->{row['cost_after']['cpus']}")
        return row
    finally:
        if load is not None:
            load.close()
        mercury.stop(started)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--label", default=time.strftime("policies-%Y%m%d-%H%M%S"))
    parser.add_argument("--output-root", default=str(REPO / "output"))
    parser.add_argument("--build", default=str(REPO / "build"))
    parser.add_argument("--arms", default=",".join(ARMS), help="comma list, from: " + ",".join(ARMS))
    parser.add_argument("--port", type=int, default=18200, help="mercury worker port")
    parser.add_argument("--service-port", type=int, default=18300)
    parser.add_argument("--cpus", type=lambda s: [int(c) for c in s.split(",")] if s else None, default=None,
                        help="mercury share, comma list incl. CPU 0 (default: every online CPU)")
    parser.add_argument("--services", type=int, default=2)
    parser.add_argument("--clients", type=int, default=2, help="closed-loop clients per service")
    parser.add_argument("--think-ms", type=float, default=0.0)
    parser.add_argument("--level", default="auto", choices=("auto", "node", "l3", "core", "socket"),
                        help="group level shift moves between; auto = node when the host has 2+ NUMA nodes")
    parser.add_argument("--victim", default=None, help="group name to inject on (default: first usable group)")
    parser.add_argument("--burn-threads", type=int, default=0, help="default: 2 x victim CPUs")
    parser.add_argument("--llm", default="auto", choices=("auto", "openai", "gemini", "mock", "none"),
                        help="zeroctl model: auto = openai if OPENAI_API_KEY, gemini if GEMINI_API_KEY, else skip")
    parser.add_argument("--zeroctl-every", type=int, default=10, help="seconds between zeroctl model calls")
    parser.add_argument("--warmup", type=int, default=15)
    parser.add_argument("--hold", type=int, default=15, help="stabilize seconds after inject before scoring")
    parser.add_argument("--measure", type=int, default=20, help="score window seconds")
    args = parser.parse_args()
    if os.geteuid() != 0:
        print("run as root: Mercury writes /sys/fs/cgroup", file=sys.stderr)
        return 2
    arms = [a.strip() for a in args.arms.split(",") if a.strip()]
    for arm in arms:
        if arm not in ARMS:
            print(f"unknown arm {arm}; choose from {', '.join(ARMS)}", file=sys.stderr)
            return 2
    out_root = Path(args.output_root) / args.label
    out_root.mkdir(parents=True, exist_ok=True)
    (out_root / "experiment.json").write_text(json.dumps(vars(args), indent=2) + "\n")
    rows = []
    for arm in arms:
        try:
            rows.append(run_arm(arm, args, out_root))
        except Exception as exc:  # keep the other arms
            print(f"[{arm}] failed: {exc}", file=sys.stderr)
            rows.append({"policy": arm, "errors": [str(exc)]})
        time.sleep(2)
    with open(out_root / "summary.csv", "w", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["policy", "level", "victim", "actions", "baseline_p50_us", "baseline_p95_us",
                         "p50_us", "p95_us", "error_rate", "requests", "throughput_rps", "cpus_before", "cpus_after", "note"])
        for row in rows:
            score = row.get("score") or {}
            base = row.get("baseline") or {}
            writer.writerow([
                row["policy"], row.get("mercury_level", ""), row.get("victim", ""), len(row.get("applied") or []),
                base.get("p50_us", ""), base.get("p95_us", ""), score.get("p50_us", ""), score.get("p95_us", ""),
                score.get("error_rate", ""), score.get("latency_traces", ""), score.get("throughput_rps", ""),
                (row.get("cost_before") or {}).get("cpus", ""), (row.get("cost_after") or {}).get("cpus", ""),
                row.get("skipped") or "; ".join(row.get("errors") or []),
            ])
    (out_root / "summary.json").write_text(json.dumps(rows, indent=2) + "\n")
    print(f"Wrote {out_root}")
    print((out_root / "summary.csv").read_text())
    return 0 if all(not r.get("errors") for r in rows) else 1


if __name__ == "__main__":
    sys.exit(main())
