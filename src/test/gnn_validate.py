#!/usr/bin/env python3
"""Compare per-thread task-clock deltas (threadtrace) with gnnload ground-truth demand.

gnn_validate.py <run_dir> [--json out.json]
For every case dir (trace.jsonl + labels.csv): for each non-fresh worker thread and each tick
fully inside the labelled span, expected = time-weighted label demand over the tick, measured =
task_clock_ns / interval_ns (also run_ns from schedstat). Reports mean absolute error (cores),
mean signed error, and the same restricted to uncontended ticks (wait_ns / interval < 0.02).
"""
import json, os, sys


def load_labels(path):
    rows, end = {}, None
    with open(path) as f:
        for line in f:
            if line.startswith("#") or line.startswith("phase_start"):
                continue
            ts, phase, thread, tid, dem = line.strip().split(",")
            ts, phase, tid, dem = int(ts), int(phase), int(tid), float(dem)
            if phase < 0:
                end = ts
                continue
            rows.setdefault(tid, []).append((ts, dem))
    segs = {}
    for tid, lst in rows.items():
        lst.sort()
        out = []
        for i, (ts, dem) in enumerate(lst):
            nxt = lst[i + 1][0] if i + 1 < len(lst) else (end if end else None)
            out.append((ts, nxt, dem))
        segs[tid] = out
    return segs


def expected(segs, a, b):
    if not segs or a < segs[0][0]:
        return None
    last = segs[-1]
    if last[1] is None:
        # open-ended: assume phases of equal length continue only within the last phase span
        span = segs[1][0] - segs[0][0] if len(segs) > 1 else None
        if span is None or b > last[0] + span:
            return None
        segs = segs[:-1] + [(last[0], last[0] + span, last[2])]
    elif b > last[1]:
        return None
    tot = 0.0
    for s, e, d in segs:
        lo, hi = max(a, s), min(b, e)
        if hi > lo:
            tot += d * (hi - lo)
    return tot / (b - a)


def stats(pairs):
    if not pairs:
        return {"n": 0}
    err = [m - e for e, m in pairs]
    return {"n": len(pairs), "mae": sum(abs(x) for x in err) / len(err), "bias": sum(err) / len(err),
            "max_abs": max(abs(x) for x in err), "mean_expected": sum(e for e, _ in pairs) / len(pairs)}


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    run = sys.argv[1]
    out_json = sys.argv[sys.argv.index("--json") + 1] if "--json" in sys.argv else os.path.join(run, "validation.json")
    result, allp, allq, allr = {}, [], [], []
    print(f"{'case':28s} {'n':>5s} {'MAE tc':>8s} {'bias tc':>8s} {'max':>6s} {'MAE run':>8s} {'n_unc':>6s} {'MAE unc':>8s} {'cpus':>5s}")
    for case in sorted(os.listdir(run)):
        d = os.path.join(run, case)
        tp, lp = os.path.join(d, "trace.jsonl"), os.path.join(d, "labels.csv")
        if not (os.path.isfile(tp) and os.path.isfile(lp)):
            continue
        segs = load_labels(lp)
        tc, rn, unc, cpus = [], [], [], []
        with open(tp) as f:
            for line in f:
                rec = json.loads(line)
                iv = rec["interval_ns"]
                if iv <= 0:
                    continue
                b, a = rec["ts_ns"], rec["ts_ns"] - iv
                cpus.append(rec["cgroup"]["cpuset_cpus"])
                for t in rec["threads"]:
                    if t["fresh"] or t["tid"] not in segs:
                        continue
                    e = expected(segs[t["tid"]], a, b)
                    if e is None:
                        continue
                    if t["perf"]:
                        m = t["task_clock_ns"] / iv
                        tc.append((e, m))
                        if t["wait_ns"] / iv < 0.02:
                            unc.append((e, m))
                    if t["run_ns"]:
                        rn.append((e, t["run_ns"] / iv))
        s_tc, s_rn, s_unc = stats(tc), stats(rn), stats(unc)
        result[case] = {"task_clock": s_tc, "schedstat_run": s_rn, "task_clock_uncontended": s_unc,
                        "mean_cpuset_cpus": sum(cpus) / len(cpus) if cpus else None}
        allp += tc; allq += unc; allr += rn
        f = lambda s, k: f"{s[k]:8.4f}" if s.get("n") else "     n/a"
        print(f"{case:28s} {s_tc['n']:5d} {f(s_tc,'mae')} {f(s_tc,'bias')} {s_tc.get('max_abs',0):6.3f} {f(s_rn,'mae')} {s_unc['n']:6d} {f(s_unc,'mae')} {result[case]['mean_cpuset_cpus'] or 0:5.1f}")
    result["_all"] = {"task_clock": stats(allp), "task_clock_uncontended": stats(allq), "schedstat_run": stats(allr)}
    a = result["_all"]
    if a["task_clock"]["n"]:
        print(f"ALL task-clock: n={a['task_clock']['n']} MAE={a['task_clock']['mae']:.4f} cores bias={a['task_clock']['bias']:+.4f} "
              f"max={a['task_clock']['max_abs']:.3f} (mean demand {a['task_clock']['mean_expected']:.3f})")
    if a["task_clock_uncontended"]["n"]:
        u = a["task_clock_uncontended"]
        print(f"ALL uncontended (wait<2%): n={u['n']} MAE={u['mae']:.4f} bias={u['bias']:+.4f} max={u['max_abs']:.3f}")
    if a["schedstat_run"]["n"]:
        r = a["schedstat_run"]
        print(f"ALL schedstat run: n={r['n']} MAE={r['mae']:.4f} bias={r['bias']:+.4f}")
    with open(out_json, "w") as f:
        json.dump(result, f, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
