"""Load Mercury gnn datasets (threadtrace trace.jsonl + gnnload labels.csv) into per-tick arrays."""
import bisect, json, math, os
import numpy as np

EPS = 1e-6


def load_labels(path):
    rows, end, phase_ns = {}, None, None
    with open(path) as f:
        for line in f:
            if line.startswith("#"):
                for kv in line.split():
                    if kv.startswith("phase_ms="):
                        phase_ns = int(kv.split("=")[1]) * 1_000_000
                continue
            if line.startswith("phase_start"):
                continue
            ts, ph, th, tid, dem = line.strip().split(",")
            if int(ph) < 0:
                end = int(ts)
                continue
            rows.setdefault(int(tid), []).append((int(ts), float(dem)))
    segs = {}
    for tid, lst in rows.items():
        lst.sort()
        starts = [a for a, _ in lst]
        dems = [b for _, b in lst]
        stop = end if end is not None else starts[-1] + phase_ns
        segs[tid] = (starts, dems, stop)
    return segs


def expected(seg, a, b):
    """Time-weighted demand of one thread over (a, b]; None outside the labelled span."""
    starts, dems, stop = seg
    if a < starts[0] or b > stop:
        return None
    i = max(0, bisect.bisect_right(starts, a) - 1)
    tot = 0.0
    while i < len(starts) and starts[i] < b:
        s = starts[i]
        e = starts[i + 1] if i + 1 < len(starts) else stop
        lo, hi = max(a, s), min(b, e)
        if hi > lo:
            tot += dems[i] * (hi - lo)
        i += 1
    return tot / (b - a)


def ffd_cores(dem):
    """Min unit cores so every thread gets its demand, a thread never exceeds one core and is
    never split across cores: first-fit-decreasing bin packing of demands into capacity-1 bins."""
    bins = []
    for d in sorted((x for x in dem if x > EPS), reverse=True):
        for i, used in enumerate(bins):
            if used + d <= 1.0 + EPS:
                bins[i] += d
                break
        else:
            bins.append(d)
    return len(bins)


def expected_span(seg, a, b):
    """Like expected(), for per-step tids (labels "# spans=1"): 0 outside the tid's labelled span,
    partial overlap time-weighted, so a tick straddling a step boundary is fine."""
    starts, dems, stop = seg
    tot = 0.0
    for i, s in enumerate(starts):
        e = starts[i + 1] if i + 1 < len(starts) else stop
        lo, hi = max(a, s), min(b, e)
        if hi > lo:
            tot += dems[i] * (hi - lo)
    return tot / (b - a)


def load_self(d, mode, every=1):
    """DSB traces without gnnload labels ("selflabel" file in the case dir): label = the thread's own
    run share per tick, clipped 0..1. mode "runshare": every thread-tick labelled (recorded under a
    generous fixed allocation, so run share = demand). mode "uncontended": DAgger relabel of a
    gnn-controlled run; contended thread-ticks get label -1 (masked out of loss/eval). Contended =
    wait share >= 0.05 AND wait per timeslice >= 100 us: plain Go/memcached wake-up latency is ~30 us
    per slice at 1 user (wait/run ~1 with no CPU shortage), so wait share alone marks nearly every tick.
    Ticks shorter than half the median interval (threadtrace jitter; shares blow up) are invalid."""
    recs = [json.loads(l) for l in open(os.path.join(d, "trace.jsonl"))]
    recs = [r for r in recs if r["interval_ns"] > 0]
    tids = sorted({t["tid"] for r in recs for t in r["threads"]})
    keys = ["t", "iv", "cpus", "dem", "tc", "run", "wait", "cs", "mig", "node", "cg_occ", "cg_psi", "other_run", "other_wait", "valid"]
    out = {k: [] for k in keys}; out["tids"] = tids
    t0 = recs[0]["ts_ns"] if recs else 0
    med = float(np.median([r["interval_ns"] for r in recs])) if recs else 0
    for r in recs:
        iv = r["interval_ns"]; th = {t["tid"]: t for t in r["threads"]}
        g = lambda f: [th[x][f] / iv if x in th else 0.0 for x in tids]
        run, wait = g("run_ns"), g("wait_ns")
        dem = [min(1.0, max(0.0, x)) for x in run]
        if mode == "uncontended":
            wps = [th[x]["wait_ns"] / max(th[x]["timeslices"], 1) if x in th else 0 for x in tids]
            dem = [-1.0 if w >= 0.05 and q >= 100_000 else x for x, w, q in zip(dem, wait, wps)]
        cpus = max(1, r["cgroup"]["cpuset_cpus"])
        out["valid"].append(iv >= 0.5 * med and not any(th[x]["fresh"] for x in th))
        out["t"].append((r["ts_ns"] - t0) / 1e9); out["iv"].append(iv); out["cpus"].append(r["cgroup"]["cpuset_cpus"])
        out["dem"].append(dem); out["tc"].append(g("task_clock_ns")); out["run"].append(run); out["wait"].append(wait)
        out["cs"].append([th[x]["context_switches"] * 1e9 / iv if x in th else 0.0 for x in tids])
        out["mig"].append([th[x]["cpu_migrations"] * 1e9 / iv if x in th else 0.0 for x in tids])
        out["node"].append([th[x]["node"] if x in th else -1 for x in tids])
        out["cg_occ"].append(r["cgroup"]["usage_ns"] / iv / cpus); out["cg_psi"].append(r["cgroup"]["some_ns"] / iv)
        out["other_run"].append(0.0); out["other_wait"].append(0.0)
    for k in keys:
        out[k] = np.array(out[k])
    out["valid"] = out["valid"].astype(bool)
    dem = np.clip(out["dem"], 0, 1)
    out["req_u1"] = np.ceil(dem.sum(1) - EPS).astype(int)
    out["req_u075"] = np.ceil(dem.sum(1) / 0.75 - EPS).astype(int)
    out["req_ffd"] = np.array([ffd_cores(row) for row in dem])
    return out


def load_case(d, every=1):
    """Per tick: t (s from first tick), interval, cpuset size, per-worker demand/run/wait/tc/...
    every: keep every n-th tick (sum deltas of skipped ticks, i.e. downsample)."""
    if os.path.isfile(os.path.join(d, "selflabel")):
        return load_self(d, open(os.path.join(d, "selflabel")).read().strip(), every)
    segs = load_labels(os.path.join(d, "labels.csv"))
    with open(os.path.join(d, "labels.csv")) as f:
        spans = "spans=1" in f.readline()
    recs = [json.loads(l) for l in open(os.path.join(d, "trace.jsonl"))]
    recs = [r for r in recs if r["interval_ns"] > 0]
    if every > 1:  # merge groups of `every` ticks
        merged = []
        for i in range(0, len(recs) - every + 1, every):
            grp = recs[i:i + every]
            base = json.loads(json.dumps(grp[-1]))
            base["interval_ns"] = sum(g["interval_ns"] for g in grp)
            for k in ("usage_ns", "user_ns", "system_ns", "some_ns", "full_ns"):
                base["cgroup"][k] = sum(g["cgroup"][k] for g in grp)
            by = {}
            for g in grp:
                for t in g["threads"]:
                    acc = by.setdefault(t["tid"], dict(t, task_clock_ns=0, context_switches=0, cpu_migrations=0, run_ns=0, wait_ns=0, timeslices=0))
                    for k in ("task_clock_ns", "context_switches", "cpu_migrations", "run_ns", "wait_ns", "timeslices"):
                        acc[k] += t[k]
                    acc["fresh"] = acc["fresh"] or t["fresh"]
                    acc["cpu"], acc["node"] = t["cpu"], t["node"]
            base["threads"] = list(by.values())
            merged.append(base)
        recs = merged
    tids = sorted(segs)
    out = {"tids": tids, "t": [], "iv": [], "cpus": [], "dem": [], "tc": [], "run": [], "wait": [], "cs": [], "mig": [],
           "node": [], "cg_occ": [], "cg_psi": [], "other_run": [], "other_wait": [], "valid": []}
    t0 = recs[0]["ts_ns"] if recs else 0
    for r in recs:
        iv = r["interval_ns"]
        b, a = r["ts_ns"], r["ts_ns"] - iv
        th = {t["tid"]: t for t in r["threads"]}
        if spans:  # stress-ng: workers change per step; only the tids with demand in this tick must be present
            dem = [expected_span(segs[k], a, b) for k in tids]
            ok = a >= min(s[0][0] for s in segs.values()) and b <= max(s[2] for s in segs.values()) and \
                all(k in th and not th[k]["fresh"] for k, x in zip(tids, dem) if x > 0) and sum(dem) > 0.05
            # (the ~75 ms between two stress-ng runs has no workers; those ticks are not scored)
        else:
            dem = [expected(segs[k], a, b) for k in tids]
            ok = all(x is not None for x in dem) and all(k in th and not th[k]["fresh"] for k in tids)
        cpus = max(1, r["cgroup"]["cpuset_cpus"])
        out["valid"].append(ok)
        out["t"].append((b - t0) / 1e9)
        out["iv"].append(iv)
        out["cpus"].append(r["cgroup"]["cpuset_cpus"])
        out["dem"].append([x or 0.0 for x in dem])
        g = lambda k, f: [th[x][f] / iv if x in th else 0.0 for x in tids]
        out["tc"].append(g(0, "task_clock_ns"))
        out["run"].append(g(0, "run_ns"))
        out["wait"].append(g(0, "wait_ns"))
        out["cs"].append([th[x]["context_switches"] * 1e9 / iv if x in th else 0.0 for x in tids])
        out["mig"].append([th[x]["cpu_migrations"] * 1e9 / iv if x in th else 0.0 for x in tids])
        out["node"].append([th[x]["node"] if x in th else -1 for x in tids])
        out["cg_occ"].append(r["cgroup"]["usage_ns"] / iv / cpus)
        out["cg_psi"].append(r["cgroup"]["some_ns"] / iv)
        others = [t for t in r["threads"] if t["tid"] not in segs]
        out["other_run"].append(sum(t["run_ns"] for t in others) / iv)
        out["other_wait"].append(sum(t["wait_ns"] for t in others) / iv)
    for k in list(out):
        if k != "tids":
            out[k] = np.array(out[k])
    out["valid"] = out["valid"].astype(bool)
    dem = out["dem"]
    out["req_u1"] = np.array([math.ceil(s - EPS) for s in dem.sum(1)])
    out["req_u075"] = np.array([math.ceil(s / 0.75 - EPS) for s in dem.sum(1)])
    out["req_ffd"] = np.array([ffd_cores(row) for row in dem])
    return out


def cases(run):
    return sorted(c for c in os.listdir(run) if os.path.isfile(os.path.join(run, c, "trace.jsonl")) and (os.path.isfile(os.path.join(run, c, "labels.csv")) or os.path.isfile(os.path.join(run, c, "selflabel"))))
