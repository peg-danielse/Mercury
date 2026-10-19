"""gnn-slo-v1 design v3 labeller (DESIGN.md): per-service sweep + Jaeger critical-path attribution, median G reference.
python slo/slo_ref3.py hotel <sweep_dir> <out_dir> [--margin 0.20] [--wmin 0.02]
<sweep_dir>/<name>/<ts>/1-saturate per run (operate.out, stamps.tsv, sample.jsonl, traces.csv): G-p<i>-<x> (generous, reference),
<svc>-k<k>-p<i> (svc alone at k), light-k1-p<i>; warmup and ref-* are ignored.
rel(run, L, r) = median p95 over the level's windows / ref(L, r) (median over all G windows of level L).
w(run, L, r, d) = d's tail critical-path share (crit_share_tail, requests >= p90) on route r at level L in that run (G median if
the run has no sample). score(run, d, L) = sum_r w rel / sum_r w over routes with w >= wmin: a route's p95 rise is charged to the
daemons on its critical path, in proportion to their tail share. d never on a critical path at L: score = 1 (p95 not affected).
C*(d, L) = smallest k whose median score over the passes <= 1 + margin (G: median over the G runs); else unlabelled."""
import collections, csv, glob, json, os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from slo_ref import alloc
from slo_ref2 import levels, reference, med, HEAVY, G_K


def cp(d):
    """{(level, route, daemon): median crit_share_tail over the level's windows}, sample-weighted."""
    f = os.path.join(d, "traces.csv"); acc = collections.defaultdict(lambda: [0.0, 0])
    if not os.path.exists(f):
        return {}
    for row in csv.DictReader(open(f)):
        n = int(row["sample_count"])
        if n == 0:
            continue
        k = (int(row["level"]), row["route"], row["daemon"]); acc[k][0] += float(row["crit_share_tail"]) * n; acc[k][1] += n
    return {k: s / n for k, (s, n) in acc.items()}


def main():
    bench, root, out = sys.argv[1:4]; os.makedirs(out, exist_ok=True)
    arg = lambda k, dflt: float(sys.argv[sys.argv.index(k) + 1]) if k in sys.argv else dflt
    margin, wmin = arg("--margin", 0.20), arg("--wmin", 0.02)
    runs = {os.path.relpath(p, root).split(os.sep)[0]: os.path.dirname(p)
            for p in glob.glob(os.path.join(root, "*", "*", "1-saturate", "stamps.tsv"))}
    runs = {n: d for n, d in runs.items() if n != "warmup" and not n.startswith("ref-")}
    L = {n: levels(d) for n, d in runs.items()}; W = {n: cp(d) for n, d in runs.items()}
    G = sorted(n for n in runs if n.startswith("G-"))
    ref = reference(L, G); P = {n: med(L, n) for n in runs}
    routes = sorted({r for _, r in ref}); lvls = sorted({lv for lv, _ in ref})
    allocs = {n: alloc(d) for n, d in runs.items()}
    daemons = sorted({d for ts, A in allocs.values() for d in A if not d.endswith("loadgen")})
    # G critical-path shares (median over G runs), the fallback when a run has no sample at a level/route
    WG = {}
    for k in {k for g in G for k in W[g]}:
        WG[k] = float(np.median([W[g].get(k, 0.0) for g in G]))

    SAMPLED = {n: {k[:2] for k in W[n]} for n in runs}  # (level, route) with sampled requests in that run

    def weights(n, d, lv):
        prog = d.split("-", 1)[1]; w = {}
        for r in routes:
            v = W[n].get((lv, r, prog), 0.0) if (lv, r) in SAMPLED[n] else WG.get((lv, r, prog), 0.0)
            if v >= wmin and (lv, r) in P[n] and (lv, r) in ref:
                w[r] = v
        return w

    def score(n, d, lv):
        w = weights(n, d, lv)
        if not w:
            return 1.0
        return float(sum(v * P[n][(lv, r)] / ref[(lv, r)] for r, v in w.items()) / sum(w.values()))

    cstar, cand_tab = {}, {}
    for d in daemons:
        s = d.split("-", 1)[1]; heavy = s in HEAVY[bench]
        pre = [(k, f"{s}-k{k}-p") for k in (1, 2, 4)] if heavy else [(1, "light-k1-p")]
        cands = sorted([(k, [n for n in runs if n.startswith(p)]) for k, p in pre] + [(G_K[heavy], G)])
        for lv in lvls:
            row = [(k, float(np.median([score(n, d, lv) for n in ns])), len(ns)) for k, ns in cands if ns]
            cand_tab[(d, lv)] = row
            ok = [k for k, sc, _ in row if sc <= 1 + margin]
            if ok:
                cstar[(d, lv)] = min(ok)
    for n, dd in runs.items():
        ts, A = allocs[n]; res = {}
        for d in daemons:
            rows = []
            for lv, rr in L[n].items():
                if (d, lv) not in cstar:
                    continue
                ends = sorted(int(np.median([w[i][1] for w in rr.values() if len(w) > i])) for i in range(max(map(len, rr.values()))))
                sc = score(n, d, lv)
                for e in ends:
                    m = (ts > e - 1e9) & (ts <= e)
                    a = float(np.nanmean(A[d][m])) if d in A and m.any() else np.nan
                    rows.append([e - 10**9, e, cstar[(d, lv)], sc, a])
            res[d] = rows
        json.dump(res, open(os.path.join(out, f"{n}.windows.json"), "w"))
    clients = {lv: L[G[0]][lv][routes[0]][0][2] for lv in lvls}
    scat = {}
    for n in G:  # G-vs-G: each G run against the median of the other G runs
        r2 = reference(L, [g for g in G if g != n])
        x = [P[n][k] / r2[k] for k in P[n] if k in r2]; scat[n] = [round(float(np.median(x)), 3), round(float(np.mean(np.array(x) <= 1 + margin) * 100), 1)]
    # attribution check: in each sweep run, the swept daemon's tail critical-path share vs G (mean over levels/routes with w>=wmin in G)
    attr = {}
    for n in runs:
        if n.startswith(("G-", "light")):
            continue
        prog = n.rsplit("-k", 1)[0]
        a = [(W[n].get(k, 0.0), v) for k, v in WG.items() if k[2] == prog and v >= wmin]
        attr[n] = [round(float(np.mean([x for x, _ in a])), 3), round(float(np.mean([y for _, y in a])), 3)] if a else None
    lab = np.array(list(cstar.values()), float); n_all = len(daemons) * len(lvls)
    summ = {"labelled_%": 100 * len(cstar) / n_all, "mean_label": float(lab.mean()) if len(lab) else None,
            "label_hist": dict(collections.Counter(int(x) for x in lab)), "G_loo_median_rel_and_within_%": scat,
            "per_daemon_mean_label": {d: (float(np.mean(v)) if (v := [cstar[(d, lv)] for lv in lvls if (d, lv) in cstar]) else None) for d in daemons},
            "per_daemon_unlabelled_levels": {d: [lv for lv in lvls if (d, lv) not in cstar] for d in daemons},
            "swept_daemon_tail_cp_share_run_vs_G": attr}
    json.dump({"bench": bench, "margin": margin, "wmin": wmin, "G": G, "runs": runs, "clients": clients,
               "ref_p95_us": {f"{lv}/{r}": v for (lv, r), v in ref.items()},
               "cp_share_tail_G": {f"{lv}/{r}/{d}": v for (lv, r, d), v in WG.items()},
               "candidates": {f"{d}|{lv}": v for (d, lv), v in cand_tab.items()},
               "cstar": {f"{d}|{lv}": v for (d, lv), v in cstar.items()}, "summary": summ}, open(os.path.join(out, "ref.json"), "w"), indent=1)
    print(json.dumps(summ, indent=1))


if __name__ == "__main__":
    main()
