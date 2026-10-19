"""gnn-slo-v1 design v2 labeller (DESIGN.md): one-service-at-a-time sweep, median reference from the generous runs G.
python slo/slo_ref2.py <hotel|social> <sweep_dir> <out_dir> [--margin 0.20]
<sweep_dir>/<name>/<ts>/1-saturate per run: G-r<i> (generous, reference), <svc>-k<k> (svc alone at k), light-k1.
p(run, level, route) = median p95 over the level's 4 windows; ref(level, route) = median over all G windows.
score(run, d, level) = mean over routes R(d) of p / ref. C*(d, level) = smallest k whose run scores <= 1 + margin (G: median
over the G runs), else unlabelled (omitted). Writes ref.json and <run>.windows.json ({daemon: [[start_ns, end_ns, C*, score, alloc]]})."""
import collections, glob, json, os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from slo_ref import RX, routes_of, alloc

HEAVY = {"hotel": "attractions geo mongod profile rate recommendation reservation review search user".split(),
         "social": "PostStorageService SocialGraphService HomeTimelineService rabbitmq-server mongod".split()}
G_K = {True: 3, False: 2}  # G allocation: heavy 3, light 2


def levels(d):
    """{level: {route: [(p95_us, mono_ns, clients)]}} from stamps.tsv; level = index of the client step in the ramp."""
    out, level, last_c = collections.defaultdict(lambda: collections.defaultdict(list)), -1, None
    for l in open(os.path.join(d, "stamps.tsv")):
        ts, l = l.rstrip("\n").split("\t", 1)
        m = RX.match(l)
        if not m:
            continue
        c = int(m.group(2))
        if c != last_c:
            level += 1; last_c = c
        out[level][m.group(1)].append((float(m.group(6)), int(ts), c))
    return out


def run_dirs(root):
    return {os.path.relpath(p, root).split(os.sep)[0]: os.path.dirname(p)
            for p in glob.glob(os.path.join(root, "*", "*", "1-saturate", "stamps.tsv"))
            if not os.path.relpath(p, root).startswith("ref-")}


def reference(L, gnames):
    pool = collections.defaultdict(list)
    for n in gnames:
        for lv, rr in L[n].items():
            for r, w in rr.items():
                pool[(lv, r)] += [p for p, _, _ in w]
    return {k: float(np.median(v)) for k, v in pool.items()}


def med(L, n):
    return {(lv, r): float(np.median([p for p, _, _ in w])) for lv, rr in L[n].items() for r, w in rr.items()}


def main():
    bench, root, out = sys.argv[1:4]; os.makedirs(out, exist_ok=True)
    margin = float(sys.argv[sys.argv.index("--margin") + 1]) if "--margin" in sys.argv else 0.20
    runs = run_dirs(root); L = {n: levels(d) for n, d in runs.items()}
    G = sorted(n for n in runs if n.startswith("G-r"))
    ref = reference(L, G); P = {n: med(L, n) for n in runs}
    routes = sorted({r for _, r in ref}); lvls = sorted({lv for lv, _ in ref})
    allocs = {n: alloc(d) for n, d in runs.items()}
    daemons = sorted({d for ts, A in allocs.values() for d in A if not d.endswith("loadgen")})

    def score(n, d, lv):
        x = [P[n][(lv, r)] / ref[(lv, r)] for r in routes_of(bench, d, routes) if (lv, r) in P[n] and (lv, r) in ref]
        return float(np.mean(x)) if x else np.nan

    cstar, cand_tab = {}, {}
    for d in daemons:
        s = d.split("-", 1)[1]; heavy = s in HEAVY[bench]
        cands = ([(k, [f"{s}-k{k}"]) for k in (1, 2, 4)] if heavy else [(1, ["light-k1"])]) + [(G_K[heavy], G)]
        cands = sorted((k, [n for n in ns if n in runs]) for k, ns in cands)
        for lv in lvls:
            row = [(k, float(np.median([score(n, d, lv) for n in ns]))) for k, ns in cands if ns]
            cand_tab[(d, lv)] = row
            ok = [k for k, sc in row if np.isfinite(sc) and sc <= 1 + margin]
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
    # G-vs-G scatter: each G run against the median of the other G runs
    scat = {}
    for n in G:
        r2 = reference(L, [g for g in G if g != n])
        x = [P[n][k] / r2[k] for k in P[n] if k in r2]; scat[n] = [float(np.median(x)), float(np.mean(np.array(x) <= 1 + margin) * 100)]
    lab = np.array(list(cstar.values()), float); n_all = len(daemons) * len(lvls)
    summ = {"labelled_%": 100 * len(cstar) / n_all, "mean_label": float(lab.mean()) if len(lab) else None,
            "label_hist": dict(collections.Counter(int(x) for x in lab)), "G_loo_median_rel_and_within_%": scat,
            "per_daemon_mean_label": {d: float(np.mean([cstar[(d, lv)] for lv in lvls if (d, lv) in cstar])) if any((d, lv) in cstar for lv in lvls) else None for d in daemons},
            "per_daemon_unlabelled_levels": {d: [lv for lv in lvls if (d, lv) not in cstar] for d in daemons}}
    json.dump({"bench": bench, "margin": margin, "G": G, "runs": runs, "clients": clients,
               "ref_p95_us": {f"{lv}/{r}": v for (lv, r), v in ref.items()},
               "candidates": {f"{d}|{lv}": v for (d, lv), v in cand_tab.items()},
               "cstar": {f"{d}|{lv}": v for (d, lv), v in cstar.items()}, "summary": summ}, open(os.path.join(out, "ref.json"), "w"), indent=1)
    print(json.dumps(summ, indent=1))


if __name__ == "__main__":
    main()
