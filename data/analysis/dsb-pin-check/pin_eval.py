"""No-pinning check (social, users 1->64->1): throughput / p99 per user step and the schedstat
wake-up-delay proxy (run-queue wait per timeslice, all service threads) in the 2-user windows.
perf is not installed on dtn-01, so wait/slice from /proc/<pid>/task/<tid>/schedstat (summed per
daemon by dsb_sample.py every 100 ms) stands in for perf sched latency.
  pin_eval.py RUNDIR OUTDIR     (RUNDIR holds 1-gnn-nopin, 1-gnn, 1-saturate)"""
import json, os, sys, statistics as st
import matplotlib; matplotlib.use("Agg"); import matplotlib.pyplot as plt

run, out = sys.argv[1], sys.argv[2]; os.makedirs(out, exist_ok=True)
STEP_S, WIN = 10.0, 4

def routes(p):
    rows = []
    for l in open(p):
        if l.startswith("route "):
            f = l.split(); rows.append({"route": f[1], "clients": int(f[3]), "thr": float(f[7]), "p99": float(f[13]) / 1000})
    return rows

def steps(rows):
    nr = len({r["route"] for r in rows}); res = []
    for i in range(0, len(rows), nr * WIN):
        blk = rows[i:i + nr * WIN]
        wins = [blk[j:j + nr] for j in range(0, len(blk), nr)]
        res.append({"users": blk[0]["clients"], "thr": st.mean(sum(r["thr"] for r in w) for w in wins),
                    "p99_worst": st.median(max(r["p99"] for r in w) for w in wins)})
    return res

def wake(p, step_idx):
    S = [json.loads(l) for l in open(p)]
    lg = [s["ts_ns"] for s in S if any(k.endswith("-loadgen") for k in s["daemons"])]
    t0 = lg[0]
    a, b = t0 + step_idx * STEP_S * 1e9, t0 + (step_idx + 1) * STEP_S * 1e9
    per = {}
    for s in S:
        if a <= s["ts_ns"] < b:
            for d, v in s["daemons"].items():
                if d.endswith("-loadgen"): continue
                x = per.setdefault(d, [0, 0]); x[0] += v.get("wait_ns", 0); x[1] += v.get("slices", 0)
    W = sum(x[0] for x in per.values()); N = sum(x[1] for x in per.values())
    return {"all_us_per_slice": W / max(N, 1) / 1e3, "wait_core_s": W / 1e9,
            "per_daemon_us": {d: x[0] / max(x[1], 1) / 1e3 for d, x in sorted(per.items())}}

res = {}
for pol in ("saturate", "gnn", "gnn-nopin"):
    d = os.path.join(run, "1-" + pol)
    if not os.path.isdir(d): continue
    stp = steps(routes(os.path.join(d, "operate.out")))
    i2 = [i for i, s in enumerate(stp) if s["users"] == 2]
    res[pol] = {"steps": stp, "total_thr": st.mean(s["thr"] for s in stp),
                "wake_2u_up": wake(os.path.join(d, "sample.jsonl"), i2[0]), "wake_2u_down": wake(os.path.join(d, "sample.jsonl"), i2[-1])}
json.dump(res, open(os.path.join(out, "pin.json"), "w"), indent=1)
lab = [f"{'d' if i > len(res['saturate']['steps'])//2 else 'u'}{s['users']}" for i, s in enumerate(res["saturate"]["steps"])]
fig, ax = plt.subplots(1, 2, figsize=(12, 4))
for pol, r in res.items():
    ax[0].plot(lab, [s["thr"] for s in r["steps"]], marker="o", label=pol)
    ax[1].plot(lab, [s["p99_worst"] for s in r["steps"]], marker="o", label=pol)
ax[0].set_ylabel("throughput req/s (all routes)"); ax[1].set_ylabel("worst-route p99 ms"); ax[1].set_yscale("log")
for a in ax: a.legend(); a.tick_params(axis="x", rotation=60)
fig.suptitle("social 1->64->1: saturate vs gnn pin vs gnn no-pin"); fig.tight_layout(); fig.savefig(os.path.join(out, "pin-steps.png"), dpi=110)
with open(os.path.join(out, "table.md"), "w") as f:
    f.write("| step | " + " | ".join(f"{p} thr | {p} p99 ms" for p in res) + " |\n" + "|---" * (1 + 2 * len(res)) + "|\n")
    for i, l in enumerate(lab):
        f.write(f"| {l} | " + " | ".join(f"{r['steps'][i]['thr']:.0f} | {r['steps'][i]['p99_worst']:.1f}" for r in res.values()) + " |\n")
    f.write("| mean | " + " | ".join(f"{r['total_thr']:.0f} | " for r in res.values()) + " |\n\n")
    f.write("wake-up proxy (schedstat wait per timeslice, all service threads, 10 s at 2 users), us:\n\n")
    for p, r in res.items():
        f.write(f"- {p}: up {r['wake_2u_up']['all_us_per_slice']:.1f} (wait {r['wake_2u_up']['wait_core_s']:.2f} core-s), down {r['wake_2u_down']['all_us_per_slice']:.1f} (wait {r['wake_2u_down']['wait_core_s']:.2f} core-s)\n")
print(open(os.path.join(out, "table.md")).read())
