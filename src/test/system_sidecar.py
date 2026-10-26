#!/usr/bin/env python3
"""dsb_ab.sh SIDECAR for system comparisons (compare_systems.sh): runs an interference driver and puts the
application's daemons under one placement system, on top of the operator's autoscaling (saturate,
least_loaded), until SIGTERM.

  saturate  nothing more: the operator's autoscaling alone (the baseline)
  l3        nothing here: the l3 pool is set when mercury starts (compare_systems.sh POOL_POLICY), and the
            operator's autoscaling hands out CPUs only from it
  shift     POST /shift: move a daemon's CPUs off an L3 domain above 80%; with a pool (SC_POOL_SHIFT=1) nothing here:
            the pool itself swaps a domain loaded from outside Mercury for a clean one (MERCURY_POOL swap=1)
  throttle  POST /throttle on every service: cut its CPU time by 75% while Mercury's CPUs are above 80% busy
  degrade   POST /degrade on the entry daemons (--degrade-daemons; hotel: frontend; social: the services that serve the
            optional routes): while the system is busy, Mercury disables optional routes one at a time (--degrade-routes,
            in shedding order) and those daemons reject them at once (hotel frontend: 503; social: ServiceException)
  l3shift   nothing here: the pool grows (l3) and swaps domains away from neighbours (MERCURY_POOL l3,swap=1)
  llm       tools/llmctl.py in its checking cycle (Gemini by default; GEMINI_API_KEY from the environment), with
            the run's end-to-end latency (loadgen.hist.jsonl) and Mercury's per-CPU counters as context

Daemons are found through GET /llm/state every second, so late starters are attached too; the load
generator and jaeger are left alone. Each attach is one line in RUN/system.jsonl.

dsb_ab.sh splits SIDECAR on spaces without quotes, so the settings also come from the environment:
SC_SYSTEM, SC_INTERFERE, SC_LLM_ARGS, SC_DEGRADE_ROUTES, SC_POOL_SHIFT (the options below take precedence).

  SC_SYSTEM=shift SC_INTERFERE="python3 X/interfere.py --phases 12 ..." SIDECAR="python3 system_sidecar.py" dsb_ab.sh ...
      (dsb_ab.sh appends --run-dir D --prefix hotel- --port 8080)
"""

from __future__ import annotations

import argparse
import json
import os
import shlex
import signal
import subprocess
import sys
import time
from urllib.request import Request, urlopen

HERE = os.path.dirname(os.path.abspath(__file__))
LLMCTL = os.path.join(HERE, "..", "..", "tools", "llmctl.py")


def call(port: int, path: str, body: dict | None = None) -> dict:
    data = None if body is None else json.dumps(body).encode()
    request = Request(f"http://127.0.0.1:{port}{path}", data=data, method="GET" if body is None else "POST")
    try:
        with urlopen(request, timeout=10) as reply:
            return json.loads(reply.read())
    except Exception as error:  # an attach that fails is logged and retried next second
        return {"ok": False, "error": str(error)}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    env = os.environ.get
    ap.add_argument("--system", default=env("SC_SYSTEM"),
                    choices=("saturate", "l3", "shift", "l3shift", "throttle", "degrade", "llm"))
    ap.add_argument("--interfere", default=env("SC_INTERFERE", ""),
                    help="interference driver command; --run-dir/--prefix/--port are appended")
    ap.add_argument("--llm-args", default=env("SC_LLM_ARGS", "--every 0.5"), help="extra llmctl.py arguments")
    ap.add_argument("--degrade-routes", default=env("SC_DEGRADE_ROUTES", "recommendations,restaurants,museums,cinema,review"),
                    help="degrade: the optional routes, in the order they are shed")
    ap.add_argument("--degrade-daemons", default=env("SC_DEGRADE_DAEMONS", "frontend"),
                    help="degrade: program names (without the prefix) that shed the routes; they must all be running")
    ap.add_argument("--run-dir", required=True)
    ap.add_argument("--prefix", required=True)
    ap.add_argument("--port", type=int, default=8080)
    a = ap.parse_args()
    if not a.system:
        ap.error("--system or SC_SYSTEM is required")

    log = open(os.path.join(a.run_dir, "system.jsonl"), "a")

    def emit(**record) -> None:
        record["t_ns"] = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
        log.write(json.dumps(record) + "\n")
        log.flush()

    children: list[subprocess.Popen] = []
    tail = ["--run-dir", a.run_dir, "--prefix", a.prefix, "--port", str(a.port)]
    if a.interfere:
        children.append(subprocess.Popen(shlex.split(a.interfere) + tail))
    shed = []
    if a.system == "llm" and env("SC_LLM_SHED") == "1":  # the llm may shed the degrade system's optional routes
        shed = ["--shed", a.degrade_routes, "--shed-daemons", ",".join(a.prefix + d for d in a.degrade_daemons.split(",")),
                "--critical", env("SC_CRITICAL", "")]
    if a.system == "llm":
        out = open(os.path.join(a.run_dir, "llmctl.out"), "a")
        children.append(subprocess.Popen(
            # SC_LLMCTL_PYTHON: an interpreter with torch for llmctl --llm distilled.
            [env("SC_LLMCTL_PYTHON", sys.executable), "-I", LLMCTL, "--url", f"http://127.0.0.1:{a.port}",
             "--log", os.path.join(a.run_dir, "llmctl.jsonl"),
             # The load generator appends one histogram line per 500 ms window to this file during the run.
             "--latency", os.path.join(a.run_dir, "loadgen.hist.jsonl")] + shed + shlex.split(a.llm_args),
            stdout=out, stderr=subprocess.DEVNULL))
    emit(event="start", system=a.system, interfere=a.interfere, llm_args=a.llm_args if a.system == "llm" else None, llm_shed=shed or None,
         degrade_routes=a.degrade_routes if a.system == "degrade" else None)

    def stop(*_) -> None:
        for child in children:
            child.send_signal(signal.SIGTERM)
        for child in children:
            try:
                child.wait(timeout=15)
            except subprocess.TimeoutExpired:
                child.kill()
        emit(event="stopped")
        sys.exit(0)

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    attached: set[str] = set()
    skip = (a.prefix + "loadgen", a.prefix + "jaeger")
    while True:
        if a.system == "degrade" and "degrade" not in attached:
            front = ",".join(a.prefix + d for d in a.degrade_daemons.split(","))
            state = call(a.port, "/llm/state")
            running = {d["daemon_id"] for d in state.get("daemons", [])}
            if all(d in running for d in front.split(",")):
                reply = call(a.port, "/degrade", {"daemon_id": front, "routes": a.degrade_routes})
                emit(event="attach", daemon=front, system="degrade", ok=reply.get("ok"), reply=reply)
                if reply.get("ok"):
                    attached.add("degrade")
        per_daemon = ("throttle",) if env("SC_POOL_SHIFT") == "1" else ("shift", "throttle")
        if a.system in per_daemon:
            state = call(a.port, "/llm/state")
            for daemon in state.get("daemons", []):
                name = daemon["daemon_id"]
                if not name.startswith(a.prefix) or name.startswith(skip) or name in attached:
                    continue
                reply = call(a.port, "/" + a.system, {"daemon_id": name})
                emit(event="attach", daemon=name, system=a.system, ok=reply.get("ok"), reply=reply)
                if reply.get("ok"):
                    attached.add(name)
        time.sleep(1)


if __name__ == "__main__":
    sys.exit(main())
