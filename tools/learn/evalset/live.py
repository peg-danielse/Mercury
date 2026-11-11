#!/usr/bin/env python3
"""Turn one Mercury tick into the features serve.py scores, and post them.

The vector is the last row of build.py daemon_features, with the allocation
slots filled from the previous tick the way build_luna.py lag does. Rates
(task-clock, pressure, run-queue wait, busy threads, thread count) are this
tick. llmctl --llm remote is the caller.
"""

import json
import math
from collections import deque
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

# since starts here, matching daemon_features' fill before any core change.
SINCE_NONE = 10_000.0
SECOND_NS = 1_000_000_000
TICK_NS = 10_000_000  # since is counted in 10 ms steps


class DaemonHist:
    """Allocation history of one daemon, updated once per control tick."""

    def __init__(self):
        self.n = 0
        self.prev = None  # c[t-1] once the next tick arrives; the lagged size
        self.prev2 = None  # c[t-2]
        self.change_ts = deque()

    def push(self, ts_ns, cores):
        """Record the size held now. Returns lag, delta, since, recent for this tick."""
        self.n += 1
        if self.n == 1:
            self.prev = cores
            return cores, 0.0, SINCE_NONE, 0
        lag = self.prev
        if self.prev2 is None:
            delta, changed = 0.0, False
        else:
            delta = float(self.prev - self.prev2)
            changed = self.prev != self.prev2
        if changed:
            self.change_ts.append(ts_ns)
        while self.change_ts and ts_ns - self.change_ts[0] > SECOND_NS:
            self.change_ts.popleft()
        since = SINCE_NONE if not self.change_ts else (ts_ns - self.change_ts[-1]) / TICK_NS
        recent = len(self.change_ts)
        self.prev2 = self.prev
        self.prev = cores
        return lag, delta, since, recent


def feature(clock, wait, busy, threads, pressure, lag, delta, since, recent):
    """One 25-wide row. Slot order is daemon_features."""
    cores = max(float(lag), 1.0)
    occupancy = min(max(clock / cores, 0.0), 4.0)
    held = min(max(pressure, 0.0), 1.0)
    row = [
        clock, clock, wait, clock, wait,
        0.0, 0.0,
        busy / 16.0, threads / 100.0,
        occupancy, held,
        clock, wait,
        lag / 16.0, delta / 4.0, math.log1p(since) / 10.0, recent / 10.0,
    ]
    row.extend([0.0] * 8)
    return row


class Tracker:
    def __init__(self):
        self.hist = {}

    def rows(self, daemons, sample, ts_ns):
        """daemons from /llm/state, sample the latest dsb_sample.py line or None.

        Load generator and jaeger are left out: scale_daemon refuses them, and the
        training rows did too.
        """
        sampled = (sample or {}).get("daemons") or {}
        out = []
        for daemon in daemons:
            ident = daemon.get("daemon_id") or ""
            if ident.endswith("loadgen") or ident.endswith("jaeger"):
                continue
            cores = len(daemon.get("cpus") or []) or 1
            hist = self.hist.setdefault(ident, DaemonHist())
            lag, delta, since, recent = hist.push(ts_ns, cores)
            extra = sampled.get(ident) or {}
            iv = sample.get("iv_ns") if sample else 0
            wait = (extra.get("wait_ns", 0) / iv) if iv else 0.0
            out.append({
                "id": ident,
                "daemon": feature(
                    float(daemon.get("cpu_use") or 0.0),
                    float(wait),
                    float(extra.get("busy") or 0.0),
                    float(extra.get("threads") or 0.0),
                    float(daemon.get("pressure") or 0.0),
                    float(lag), delta, since, recent,
                ),
            })
        return out


class SampleTail:
    """The newest complete line of a growing sample.jsonl, read from where the last read stopped."""

    def __init__(self, path):
        self.path = path
        self.offset = 0
        self.last = None

    def latest(self):
        if not self.path:
            return None
        try:
            with open(self.path) as handle:
                handle.seek(0, 2)
                if handle.tell() < self.offset:
                    self.offset = 0  # the file was replaced
                handle.seek(self.offset)
                chunk = handle.read()
                self.offset = handle.tell()
        except OSError:
            return self.last
        if not chunk:
            return self.last
        # A read can end mid-line. Keep that fragment for the next read.
        if not chunk.endswith("\n"):
            cut = chunk.rfind("\n")
            if cut < 0:
                self.offset -= len(chunk)
                return self.last
            self.offset -= len(chunk) - cut - 1
            chunk = chunk[:cut + 1]
        line = chunk.strip().split("\n")[-1]
        try:
            self.last = json.loads(line)
        except json.JSONDecodeError:
            pass
        return self.last


def post(url, model, session, daemons, reset, timeout=5.0):
    """POST /predict. Returns the server's JSON, or raises URLError."""
    body = json.dumps({
        "model": model, "session": session, "reset": reset, "daemons": daemons,
    }).encode()
    request = Request(url.rstrip("/") + "/predict", data=body, headers={"Content-Type": "application/json"})
    try:
        with urlopen(request, timeout=timeout) as reply:
            return json.loads(reply.read())
    except HTTPError as error:
        try:
            detail = json.loads(error.read()).get("error", "")
        except (json.JSONDecodeError, ValueError):
            detail = ""
        raise URLError(f"HTTP {error.code} {detail}".strip()) from error


def check():
    """The incremental row matches daemon_features on the lagged cpuset."""
    import sys
    from pathlib import Path

    import numpy as np

    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from build import daemon_features
    from build_luna import lag

    rng = np.random.default_rng(0)
    n = 80
    stamps = 1_000_000_000 + np.arange(n) * 50_000_000
    cores = np.clip(np.cumsum(rng.integers(-1, 2, n)) + 2, 1, 8).astype(np.int64)
    clock = rng.random(n)
    pressure = rng.random(n)
    wait = rng.random(n) * 0.3
    busy = rng.integers(0, 10, n).astype(np.float64)
    threads = rng.integers(1, 30, n).astype(np.float64)
    hist = DaemonHist()
    got = []
    for i in range(n):
        lagged, delta, since, recent = hist.push(int(stamps[i]), int(cores[i]))
        got.append(feature(float(clock[i]), float(wait[i]), float(busy[i]), float(threads[i]),
                           float(pressure[i]), float(lagged), delta, since, recent))
    ref, _, _ = daemon_features(stamps, clock, pressure, lag(cores.astype(np.float64)), wait, busy, threads)
    err = float(np.max(np.abs(np.asarray(got, np.float64) - ref)))
    if err > 1e-5:
        raise SystemExit(f"feature mismatch {err}")
    print(f"feature check ok, max abs error {err:.2e}")


if __name__ == "__main__":
    check()
