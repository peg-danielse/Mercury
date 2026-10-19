"""Closed-loop A/B of Mercury policies on gnnload cases, against ground-truth labels.
python eval_closed_loop.py <out_dir> name1=<run_dir1> name2=<run_dir2> ...
Per policy: under/over-allocation (cpuset vs FFD-required cores), core-seconds, scale-up/down lag,
stacking (busy threads sharing a CPU), demand met (sum run / sum demand), flapping (cpuset changes
per second), controller cost (from controller.log 'gnn ... mean_us' lines). Writes table.md,
ab.json, summary.png and one <case>.png per case."""
import json, os, re, sys
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from mdata import cases, load_case
from plot_scaling import lag_stats


def stacking(d, v, busy=0.5):
    """Fraction of busy worker thread-ticks (run >= busy) whose last CPU is shared with another busy worker."""
    run, cpu = d["run"][v], d["cpu"][v]
    tot = shared = 0
    for r, c in zip(run, cpu):
        b = c[r >= busy]
        if len(b) == 0:
            continue
        _, inv, cnt = np.unique(b, return_inverse=True, return_counts=True)
        tot += len(b); shared += int((cnt[inv] > 1).sum())
    return shared, tot


def controller_cost(path):
    if not os.path.exists(path):
        return None
    tot = {"read": 0.0, "infer": 0.0, "act": 0.0}; n = 0
    for line in open(path, errors="replace"):
        m = re.search(r"ticks=(\d+) mean_us read=(\d+) infer=(\d+) cpuset=(\d+) act=(\d+)", line)
        if not m or int(m.group(2)) > 20000:  # skip the first tick (opens every fd)
            continue
        k = int(m.group(1)); n += k
        tot["read"] += k * int(m.group(2)); tot["infer"] += k * int(m.group(3)); tot["act"] += k * int(m.group(5))
    return {x: y / n for x, y in tot.items()} | {"ticks": n} if n else None


def evaluate(run):
    out = {}
    for c in cases(run):
        d = load_case(os.path.join(run, c))
        # last CPU per worker per tick, for stacking
        recs = [json.loads(l) for l in open(os.path.join(run, c, "trace.jsonl"))]
        recs = [r for r in recs if r["interval_ns"] > 0]
        d["cpu"] = np.array([[next((t["cpu"] for t in r["threads"] if t["tid"] == k), -1) for k in d["tids"]] for r in recs])
        v = d["valid"]
        t, req, alloc = d["t"][v], d["req_ffd"][v], d["cpus"][v]
        dt = float(np.median(np.diff(t)))
        ups, downs, mu, md = lag_stats(t, req, alloc)
        sh, tot = stacking(d, v)
        changes = int((np.diff(d["cpus"]) != 0).sum())
        dem = d["dem"][v]
        bogo = None
        rp = os.path.join(run, c, "result.json")
        if os.path.exists(rp):  # stress-ng: bogo-ops per allocated core-second over the stressor's run
            st = json.load(open(rp))["steps"]
            ts = np.array([r["ts_ns"] for r in recs]); a0, a1 = st[0]["start_ns"], st[-1]["end_ns"]
            m = (ts > a0) & (ts <= a1)
            ops = sum(x["bogo_ops"] or 0 for x in st)
            bogo = (ops, float((d["cpus"][m] * np.array([r["interval_ns"] for r in recs])[m]).sum() / 1e9))
        out[c] = {"bogo": bogo, "t": t, "req": req, "alloc": alloc, "dem_sum": dem.sum(1), "run_sum": d["run"][v].sum(1),
                  "ticks": int(len(t)), "under": int((alloc < req).sum()), "over": int((alloc > req).sum()),
                  "deficit": float(np.clip(req - alloc, 0, None).sum()), "excess": float(np.clip(alloc - req, 0, None).sum()),
                  "core_s_req": float(req.sum() * dt), "core_s_alloc": float(alloc.sum() * dt),
                  "ups": ups, "downs": downs, "miss_up": len(mu), "miss_down": len(md),
                  "stack_shared": sh, "stack_total": tot,
                  "run": float(d["run"][v].sum()), "demand": float(dem.sum()),
                  "changes": changes, "duration": float(d["t"][-1] - d["t"][0]) if len(d["t"]) > 1 else 0.0,
                  "cost": controller_cost(os.path.join(run, c, "controller.log"))}
    return out


def summarize(res):
    S = lambda k: sum(r[k] for r in res.values())
    ticks = S("ticks")
    ups = [x for r in res.values() for x in r["ups"]]; downs = [x for r in res.values() for x in r["downs"]]
    costs = [r["cost"] for r in res.values() if r["cost"]]
    q = lambda x, p: float(np.percentile(x, p)) if x else float("nan")
    s = {"cases": len(res), "ticks": ticks, "under_%": 100 * S("under") / ticks, "over_%": 100 * S("over") / ticks,
         "deficit_mean": S("deficit") / ticks, "excess_mean": S("excess") / ticks,
         "core_s_alloc/req": S("core_s_alloc") / S("core_s_req"),
         "up_lag_med_s": q(ups, 50), "up_lag_p90_s": q(ups, 90), "up_never": S("miss_up"), "up_n": len(ups),
         "down_lag_med_s": q(downs, 50), "down_lag_p90_s": q(downs, 90), "down_never": S("miss_down"), "down_n": len(downs),
         "stacked_busy_%": 100 * S("stack_shared") / max(1, S("stack_total")),
         "demand_met_%": 100 * S("run") / S("demand"), "changes_per_s": S("changes") / S("duration")}
    bg = [r["bogo"] for r in res.values() if r.get("bogo")]
    if bg:
        s["bogo_ops_per_core_s"] = sum(b[0] for b in bg) / sum(b[1] for b in bg)
    if costs:
        n = sum(c["ticks"] for c in costs)
        for k in ("read", "infer", "act"):
            s[f"ctl_{k}_us"] = sum(c[k] * c["ticks"] for c in costs) / n
    return s


def main():
    out = sys.argv[1]; os.makedirs(out, exist_ok=True)
    runs = dict(a.split("=", 1) for a in sys.argv[2:])
    res = {n: evaluate(r) for n, r in runs.items()}
    summ = {n: summarize(r) for n, r in res.items()}
    keys = list(next(iter(summ.values())).keys() | set().union(*[s.keys() for s in summ.values()]))
    order = ["cases", "ticks", "under_%", "over_%", "deficit_mean", "excess_mean", "core_s_alloc/req", "up_lag_med_s", "up_lag_p90_s",
             "up_never", "up_n", "down_lag_med_s", "down_lag_p90_s", "down_never", "down_n", "stacked_busy_%", "demand_met_%",
             "changes_per_s", "bogo_ops_per_core_s", "ctl_read_us", "ctl_infer_us", "ctl_act_us"]
    keys = [k for k in order if k in keys]
    lines = ["| metric | " + " | ".join(summ) + " |", "|---|" + "---|" * len(summ)]
    for k in keys:
        cell = lambda s: (f"{s[k]:.3f}" if isinstance(s[k], float) else str(s[k])) if k in s else "n/a"
        lines.append(f"| {k} | " + " | ".join(cell(summ[n]) for n in summ) + " |")
    open(os.path.join(out, "table.md"), "w").write("\n".join(lines) + "\n")
    print("\n".join(lines))
    json.dump(summ, open(os.path.join(out, "ab.json"), "w"), indent=1)
    names = list(res)
    common = sorted(set.intersection(*[set(r) for r in res.values()]))
    for c in common:
        fig, ax = plt.subplots(len(names) + 1, 1, figsize=(11, 3 + 2.4 * len(names)), sharex=True)
        r0 = res[names[0]][c]
        ax[0].step(r0["t"], r0["req"], where="post", color="k", lw=2, label="required (FFD of labels)")
        ax[0].plot(r0["t"], r0["dem_sum"], color="gray", lw=0.8, label="sum demand")
        ax[0].set_title(f"{c}: required cores from ground truth"); ax[0].legend(fontsize=8, loc="upper right")
        for i, n in enumerate(names):
            r = res[n][c]
            ax[i + 1].step(r["t"], r["req"], where="post", color="k", lw=1, ls="--", label="required (this run)")
            ax[i + 1].step(r["t"], r["alloc"], where="post", color=f"C{i+3}", lw=2, label=f"{n} cpuset")
            ax[i + 1].plot(r["t"], r["run_sum"], color="C0", lw=0.7, label="sum run")
            ax[i + 1].set_ylabel("cores"); ax[i + 1].legend(fontsize=8, loc="upper right")
            ax[i + 1].set_title(f"{n}: under {100*r['under']/r['ticks']:.0f}% over {100*r['over']/r['ticks']:.0f}% "
                                f"met {100*r['run']/r['demand']:.0f}% changes/s {r['changes']/max(r['duration'],1e-9):.2f}", fontsize=9)
        ax[-1].set_xlabel("time (s), 10 ms ticks")
        fig.tight_layout(); fig.savefig(os.path.join(out, f"{c}.png"), dpi=80); plt.close(fig)
    fig, ax = plt.subplots(2, 4, figsize=(18, 7))
    for a, (k, title) in zip(ax.ravel(), [("under_%", "under-allocated ticks %"), ("over_%", "over-allocated ticks %"),
                                          ("core_s_alloc/req", "core-s allocated / required"), ("up_lag_p90_s", "scale-up lag p90 (s)"),
                                          ("down_lag_p90_s", "scale-down lag p90 (s)"), ("stacked_busy_%", "busy threads sharing a CPU %"),
                                          ("demand_met_%", "demand met % (sum run / sum demand)"), ("changes_per_s", "cpuset changes / s")]):
        a.bar(names, [summ[n].get(k, np.nan) for n in names], color=[f"C{i+3}" for i in range(len(names))]); a.set_title(title)
    fig.suptitle(f"closed loop A/B, {len(common)} cases"); fig.tight_layout(); fig.savefig(os.path.join(out, "summary.png"), dpi=90); plt.close(fig)


if __name__ == "__main__":
    main()
