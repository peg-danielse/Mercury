#!/usr/bin/env python3
"""Sample Mercury DSB daemons every --interval-ms: per daemon cpuset size, cpu.stat usage, thread count;
busy threads (schedstat run >= --busy of the interval) and cross-daemon pin collisions (busy threads of
different daemons whose last CPU is the same); CPU 0 (Mercury worker) utilisation from /proc/stat.
Per daemon also wait_ns/slices: summed schedstat run-queue wait and timeslice deltas (wait per slice
= wake-up delay proxy, used where perf is not installed). Writes one JSON line per sample.   dsb_sample.py --prefix hotel- --out f.jsonl [--interval-ms 100] [--busy 0.5]"""
import argparse, json, os, time

ap = argparse.ArgumentParser()
ap.add_argument("--prefix", required=True); ap.add_argument("--out", required=True)
ap.add_argument("--interval-ms", type=int, default=100); ap.add_argument("--busy", type=float, default=0.5)
ap.add_argument("--duration-s", type=float, default=1e9)
a = ap.parse_args()
CG = "/sys/fs/cgroup/mercury"

def rd(p):
    try:
        with open(p) as f:
            return f.read()
    except OSError:
        return None

def cpucount(spec):
    n = 0
    for part in (spec or "").strip().split(","):
        if part:
            x, _, y = part.partition("-"); n += int(y or x) - int(x) + 1
    return n

def cpu0():
    for l in open("/proc/stat"):
        if l.startswith("cpu0 "):
            v = list(map(int, l.split()[1:])); return sum(v), v[3] + v[4]
prev_ws = {}
prev_run, prev_cpu0, t_prev = {}, cpu0(), time.monotonic_ns()
end = time.monotonic() + a.duration_s
out = open(a.out, "w")
while time.monotonic() < end:
    time.sleep(a.interval_ms / 1000)
    now = time.monotonic_ns(); iv = now - t_prev; t_prev = now
    rec = {"ts_ns": now, "iv_ns": iv, "daemons": {}}
    oncpu = {}  # cpu -> list of daemon ids with a busy thread there
    busy_n = 0
    try:
        names = sorted(d for d in os.listdir(CG) if d.startswith(a.prefix))
    except OSError:
        names = []
    for d in names:
        tids = (rd(f"{CG}/{d}/cgroup.threads") or "").split()
        if not tids:
            continue
        usage = 0
        for l in (rd(f"{CG}/{d}/cpu.stat") or "").splitlines():
            if l.startswith("usage_usec"):
                usage = int(l.split()[1])
        busy = 0; wsum = 0; nsl = 0
        for t in tids:
            ss = rd(f"/proc/{t}/schedstat"); st = rd(f"/proc/{t}/stat")
            if not ss or not st:
                continue
            f3 = ss.split(); run = int(f3[0]); k = (d, t)
            w0, s0 = prev_ws.get(k, (int(f3[1]), int(f3[2]))); prev_ws[k] = (int(f3[1]), int(f3[2]))
            wsum += int(f3[1]) - w0; nsl += int(f3[2]) - s0
            dr = run - prev_run.get(k, run); prev_run[k] = run
            if dr >= a.busy * iv:
                busy += 1
                cpu = int(st[st.rindex(")") + 2:].split()[36])
                oncpu.setdefault(cpu, []).append(d)
        busy_n += busy
        rec["daemons"][d] = {"cores": cpucount(rd(f"{CG}/{d}/cpuset.cpus")), "usage_usec": usage, "threads": len(tids), "busy": busy, "wait_ns": wsum, "slices": nsl}
    coll = {c: v for c, v in oncpu.items() if len(set(v)) > 1}
    rec["busy_threads"] = busy_n
    rec["collision_cpus"] = len(coll)
    rec["collision_threads"] = sum(len(v) for v in coll.values())
    rec["stacked_cpus"] = sum(1 for v in oncpu.values() if len(v) > 1)
    tot, idle = cpu0(); dt, di = tot - prev_cpu0[0], idle - prev_cpu0[1]; prev_cpu0 = (tot, idle)
    rec["cpu0_util"] = 1 - di / dt if dt > 0 else 0.0
    out.write(json.dumps(rec) + "\n"); out.flush()
    if len(prev_run) > 200000:
        prev_run.clear(); prev_ws.clear()
