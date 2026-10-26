"""DSB closed-loop A/B (Mercury src/test/dsb_ab.sh runs): python dsb_eval.py <run_dir> <out_dir> [label]
Per policy (mean over repeats): throughput, p95/p99 (median over routes and windows; also at peak users),
errors, core-s allocated vs used (cgroup cpu.stat), cpuset changes/s, cross-daemon pin collisions,
Mercury CPU 0 utilisation, gnn per-tick cost, max threads per daemon. Writes table.md, dsb.json and plots."""
import collections, glob, json, os, re, sys
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

run, out = sys.argv[1], sys.argv[2]; label = sys.argv[3] if len(sys.argv) > 3 else os.path.basename(run.rstrip("/"))
os.makedirs(out, exist_ok=True)
RX = re.compile(r"^route (\S+) clients (\d+) arrival_rate (\d+) service_rate (\d+) avg_us (\S+) p95_us (\S+) p99_us (\S+) errors (\d+)")

def parse(d):
    rows = []  # (step index, route, clients, service, p95, p99, errors)
    last, step = None, -1
    for l in open(os.path.join(d, "operate.out"), errors="replace"):
        m = RX.match(l)
        if not m:
            continue
        r, c = m.group(1), int(m.group(2))
        if c != last:
            step += 1; last = c
        rows.append((step, r, c, int(m.group(4)), float(m.group(6)), float(m.group(7)), int(m.group(8))))
    S = [json.loads(l) for l in open(os.path.join(d, "sample.jsonl"))]
    S = [s for s in S if s["daemons"]]
    names = sorted({k for s in S for k in s["daemons"] if not k.endswith("loadgen")})
    t0 = S[0]["ts_ns"] if S else 0
    t = np.array([(s["ts_ns"] - t0) / 1e9 for s in S]); iv = np.array([s["iv_ns"] / 1e9 for s in S])
    cores = np.array([[s["daemons"].get(n, {}).get("cores", 0) for n in names] for s in S])
    usage = np.array([[s["daemons"].get(n, {}).get("usage_usec", np.nan) for n in names] for s in S], dtype=float)
    thr = np.array([[s["daemons"].get(n, {}).get("threads", 0) for n in names] for s in S])
    du = np.nan_to_num(np.diff(usage, axis=0), nan=0.0).clip(0) / 1e6
    present = (thr > 0)
    changes = int(((np.diff(cores, axis=0) != 0) & present[1:] & present[:-1]).sum())
    cost = collections.defaultdict(lambda: [0, 0, 0, 0])
    for l in open(os.path.join(d, "mercury.log"), errors="replace"):
        m = re.search(r"^gnn daemon_id=(\S+) .*ticks=(\d+) mean_us read=(\d+) infer=(\d+) cpuset=(\d+) act=(\d+)", l)
        if m and int(m.group(3)) < 20000:
            k = int(m.group(2)); c = cost[m.group(1)]
            c[0] += k; c[1] += k * int(m.group(3)); c[2] += k * int(m.group(4)); c[3] += k * int(m.group(6))
    return {"rows": rows, "names": names, "t": t, "cores": cores, "threads_max": dict(zip(names, thr.max(0).tolist())),
            "core_s_alloc": float((cores[1:] * iv[1:, None]).sum()), "core_s_used": float(du.sum()),
            "per_daemon_alloc": dict(zip(names, (cores[1:] * iv[1:, None]).sum(0).tolist())),
            "per_daemon_used": dict(zip(names, du.sum(0).tolist())),
            "changes_per_s": changes / max(1e-9, t[-1] - t[0]) if len(t) > 1 else 0.0,
            "collision_%": 100 * float(np.mean([s["collision_cpus"] > 0 for s in S])),
            "collision_threads_mean": float(np.mean([s["collision_threads"] for s in S])),
            "busy_threads_mean": float(np.mean([s["busy_threads"] for s in S])),
            "cpu0_util_%": 100 * float(np.mean([s["cpu0_util"] for s in S])), "cost": dict(cost)}

res = collections.defaultdict(list)
for d in sorted(glob.glob(os.path.join(run, "*-*"))):
    if os.path.exists(os.path.join(d, "sample.jsonl")):
        res[os.path.basename(d).split("-", 1)[1]].append(parse(d))
tab = {}
for pol, L in res.items():
    rows = [r for x in L for r in x["rows"]]
    peak = max(r[2] for r in rows)
    steps = {}
    for x in L:
        for st, r, c, sv, p95, p99, e in x["rows"]:
            steps.setdefault(id(x), {}).setdefault(st, []).append(sv)
    thr = np.mean([np.mean([sum(v) / 4 for v in s.values()]) for s in steps.values()])  # 4 windows per step, summed over routes
    cs = [x["cost"] for x in L if x["cost"]]
    cost = {}
    if cs:
        n = sum(c[0] for x in cs for c in x.values())
        for i, k in ((1, "read"), (2, "infer"), (3, "act")):
            cost[k] = sum(c[i] for x in cs for c in x.values()) / n
    tab[pol] = {"repeats": len(L), "throughput_req_s": float(thr),
                "p95_med_ms": float(np.median([r[4] for r in rows])) / 1e3, "p99_med_ms": float(np.median([r[5] for r in rows])) / 1e3,
                "p99_peak_med_ms": float(np.median([r[5] for r in rows if r[2] == peak])) / 1e3,
                "p99_p90_over_windows_ms": float(np.percentile([r[5] for r in rows], 90)) / 1e3,
                "errors": int(sum(r[6] for r in rows)) / len(L),
                "core_s_alloc": float(np.mean([x["core_s_alloc"] for x in L])), "core_s_used": float(np.mean([x["core_s_used"] for x in L])),
                "used/alloc": float(np.mean([x["core_s_used"] / x["core_s_alloc"] for x in L])),
                "changes_per_s": float(np.mean([x["changes_per_s"] for x in L])),
                "collision_%_samples": float(np.mean([x["collision_%"] for x in L])),
                "collision_threads_mean": float(np.mean([x["collision_threads_mean"] for x in L])),
                "cpu0_util_%": float(np.mean([x["cpu0_util_%"] for x in L]))}
    tab[pol].update({f"gnn_{k}_us": v for k, v in cost.items()})
    tab[pol]["threads_max"] = {k: max(x["threads_max"].get(k, 0) for x in L) for k in L[0]["names"]}
    tab[pol]["per_daemon_alloc"] = {k: float(np.mean([x["per_daemon_alloc"].get(k, 0) for x in L])) for k in L[0]["names"]}
    tab[pol]["per_daemon_used"] = {k: float(np.mean([x["per_daemon_used"].get(k, 0) for x in L])) for k in L[0]["names"]}
pols = sorted(tab)
keys = [k for k in tab[pols[0]] if not isinstance(tab[pols[0]][k], dict)]
lines = ["| metric | " + " | ".join(pols) + " |", "|---|" + "---|" * len(pols)]
for k in keys:
    lines.append(f"| {k} | " + " | ".join((f"{tab[p][k]:.3f}" if isinstance(tab[p].get(k), float) else str(tab[p].get(k, "n/a"))) for p in pols) + " |")
open(os.path.join(out, "table.md"), "w").write("\n".join(lines) + "\n")
json.dump(tab, open(os.path.join(out, "dsb.json"), "w"), indent=1)
print("\n".join(lines))
# latency vs users (up and down legs), per policy, p99 median over routes
fig, ax = plt.subplots(1, 3, figsize=(17, 4.5))
for i, p in enumerate(pols):
    for j, x in enumerate(res[p]):
        by = collections.defaultdict(list); tp = collections.defaultdict(list)
        for st, r, c, sv, p95, p99, e in x["rows"]:
            by[(st, c)].append(p99 / 1e3); tp[(st, c)].append(sv / 4)
        ks = sorted(by)
        ax[0].plot([c for _, c in ks], [np.median(by[k]) for k in ks], color=f"C{i}", alpha=0.7, marker=".", label=p if j == 0 else None)
        ax[1].plot([c for _, c in ks], [sum(tp[k]) for k in ks], color=f"C{i}", alpha=0.7, marker=".", label=p if j == 0 else None)
for a, yl in ((ax[0], "p99 (ms), median over routes"), (ax[1], "throughput (req/s, all routes)")):
    a.set_xscale("log", base=2); a.set_xlabel("users per route (ramp up and down)"); a.set_ylabel(yl); a.legend()
ax[0].set_yscale("log")
x = np.arange(2); w = 0.35
ax[2].bar(x - w / 2, [tab[p]["core_s_alloc"] for p in pols], w, label="allocated")
ax[2].bar(x + w / 2, [tab[p]["core_s_used"] for p in pols], w, label="used (cpu.stat)")
ax[2].set_xticks(x, pols); ax[2].set_ylabel("core-s per run (services)"); ax[2].legend()
fig.suptitle(f"{label}: latency/throughput vs users and core-seconds"); fig.tight_layout(); fig.savefig(os.path.join(out, "latency_cores.png"), dpi=110)
# cores vs time, repeat 1 of each policy: total and per daemon
fig, ax = plt.subplots(len(pols), 1, figsize=(13, 3.6 * len(pols)), sharex=True)
for i, p in enumerate(pols):
    xx = res[p][0]
    ax[i].stackplot(xx["t"], xx["cores"].T, labels=[n.split("-", 1)[1] for n in xx["names"]], alpha=0.85)
    ax[i].set_ylabel("allocated cores"); ax[i].set_title(f"{p} (repeat 1): cpuset sizes per daemon, stacked")
ax[0].legend(ncol=6, fontsize=7, loc="upper left"); ax[-1].set_xlabel("time (s)")
fig.tight_layout(); fig.savefig(os.path.join(out, "cores_time.png"), dpi=110)
# per-daemon core-s
fig, ax = plt.subplots(figsize=(13, 4))
names = sorted(tab[pols[0]]["per_daemon_alloc"]); x = np.arange(len(names)); w = 0.2
for i, p in enumerate(pols):
    ax.bar(x + (2 * i - 1.5) * w, [tab[p]["per_daemon_alloc"][n] for n in names], w, label=f"{p} allocated", color=f"C{i}")
    ax.bar(x + (2 * i - 0.5) * w, [tab[p]["per_daemon_used"].get(n, 0) for n in names], w, label=f"{p} used", color=f"C{i}", alpha=0.45)
ax.set_xticks(x, [n.split("-", 1)[1] for n in names], rotation=45, ha="right"); ax.set_ylabel("core-s per run"); ax.legend()
fig.tight_layout(); fig.savefig(os.path.join(out, "core_s_per_daemon.png"), dpi=110)
