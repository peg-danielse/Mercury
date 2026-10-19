"""Lowest-p95 reference and per-daemon allocation labels from a DSB allocation sweep (gnn-slo-v1, DESIGN.md).
python slo/slo_ref.py <bench hotel|social> <out_dir> [--margin 0.10] [--window-ms 1000] [--ref names] name=<run_subdir> ... fixed=<names,comma>
Each run_subdir is one dsb_ab.sh run (operate.out, stamps.tsv, sample.jsonl). Window key = (level index, window index).
Writes <out_dir>/ref.json (p95* per key and route, per-run scores) and <out_dir>/<name>.windows.json
({daemon: [[start_ns, end_ns, Cstar, score, alloc], ...]})."""
import collections, json, os, re, sys
import numpy as np
RX = re.compile(r"^route (\S+) clients (\d+) arrival_rate (\d+) service_rate (\d+) avg_us (\S+) p95_us (\S+) p99_us (\S+) errors (\d+)")
HOTEL_R = {"geo": ["geo", "search", "frontend"], "rate": ["rate", "search", "frontend"], "search": ["search", "frontend"], "frontend": ["frontend"]}
SOCIAL_R = {"ComposePostService": ["compose-post"], "UniqueIdService": ["compose-post"], "MediaService": ["compose-post"],
            "TextService": ["compose-post"], "UserMentionService": ["compose-post"], "UrlShortenService": ["compose-post"],
            "UserService": ["compose-post", "follow"], "PostStorageService": ["compose-post", "read-home-timeline", "read-user-timeline"],
            "UserTimelineService": ["compose-post", "read-user-timeline"], "WriteHomeTimelineService": ["compose-post"],
            "HomeTimelineService": ["compose-post", "read-home-timeline"], "rabbitmq-server": ["compose-post"], "SocialGraphService": ["follow"]}


def routes_of(bench, daemon, all_routes):
    s = daemon.split("-", 1)[1]
    if bench == "hotel":
        return HOTEL_R.get(s, [s, "frontend"] if s in all_routes else all_routes)
    return SOCIAL_R.get(s, all_routes)


def windows(d, window_ms):
    """[(key, route, p95_us, end_ns)] from stamps.tsv (falls back to operate.out without times)."""
    f = os.path.join(d, "stamps.tsv")
    lines = [l.rstrip("\n").split("\t", 1) for l in open(f)] if os.path.exists(f) else [(0, l) for l in open(os.path.join(d, "operate.out"))]
    out, level, last_c, seen = [], -1, None, collections.Counter()
    for ts, l in lines:
        m = RX.match(l)
        if not m:
            continue
        r, c = m.group(1), int(m.group(2))
        if c != last_c:
            level += 1; last_c = c; seen = collections.Counter()
        w = seen[r]; seen[r] += 1
        out.append(((level, w), r, float(m.group(6)), int(ts), c))
    return out


def alloc(d):
    S = [json.loads(l) for l in open(os.path.join(d, "sample.jsonl"))]
    ts = np.array([s["ts_ns"] for s in S])
    names = sorted({k for s in S for k in s["daemons"]})
    A = {n: np.array([s["daemons"].get(n, {}).get("cores", np.nan) for s in S], float) for n in names}
    return ts, A


def main():
    bench, out = sys.argv[1], sys.argv[2]; os.makedirs(out, exist_ok=True)
    arg = lambda k, d: type(d)(sys.argv[sys.argv.index(k) + 1]) if k in sys.argv else d
    margin, wms = arg("--margin", 0.10), arg("--window-ms", 1000)
    runs = dict(a.split("=", 1) for a in sys.argv[3:] if "=" in a and not a.startswith(("fixed=", "--")))
    fixed = [a.split("=", 1)[1].split(",") for a in sys.argv[3:] if a.startswith("fixed=")][0]
    W = {n: windows(p, wms) for n, p in runs.items()}
    all_routes = sorted({r for v in W.values() for _, r, _, _, _ in v})
    refn = arg("--ref", ",".join(runs)).split(",")  # runs that define p95* (default: all runs)
    best = collections.defaultdict(lambda: np.inf)
    for n0, v in W.items():
        if n0 not in refn:
            continue
        for k, r, p, _, _ in v:
            best[(k, r)] = min(best[(k, r)], p)
    # per run: p95 / p95* per (key, route); window times = median end stamp over routes
    rel, times, clients = {}, {}, {}
    for n, v in W.items():
        rel[n] = {(k, r): p / best[(k, r)] for k, r, p, _, _ in v if np.isfinite(best[(k, r)])}
        tt = collections.defaultdict(list)
        for k, r, p, ts, c in v:
            tt[k].append(ts); clients[k] = c
        times[n] = {k: int(np.median(x)) for k, x in tt.items()}
    keys = sorted({k for v in W.values() for k, *_ in v})
    allocs = {n: alloc(p) for n, p in runs.items()}
    daemons = sorted({d for _, (ts, A) in allocs.items() for d in A if not d.endswith("loadgen")})

    def mean_alloc(n, d, k):
        ts, A = allocs[n]
        if d not in A or k not in times[n]:
            return np.nan
        e = times[n][k]; m = (ts > e - wms * 1e6) & (ts <= e)
        return float(np.nanmean(A[d][m])) if m.any() else np.nan

    def score(n, d, k):
        rs = routes_of(bench, d, all_routes)
        x = [rel[n].get((k, r)) for r in rs]; x = [a for a in x if a is not None]
        return float(np.mean(x)) if x else np.nan

    cstar = {}
    for d in daemons:
        for k in keys:
            cand = [(mean_alloc(n, d, k), score(n, d, k)) for n in fixed if n in runs]
            cand = [(a, s) for a, s in cand if np.isfinite(a) and np.isfinite(s)]
            if not cand:
                continue
            ok = [a for a, s in cand if s <= 1 + margin]
            cstar[(d, k)] = min(ok) if ok else max(a for a, _ in cand)
    for n in runs:
        res = {}
        for d in daemons:
            rows = []
            for k in keys:
                if (d, k) in cstar and k in times[n]:
                    e = times[n][k]
                    rows.append([e - int(wms * 1e6), e, cstar[(d, k)], score(n, d, k), mean_alloc(n, d, k)])
            res[d] = rows
        json.dump(res, open(os.path.join(out, f"{n}.windows.json"), "w"))
    summ = {}
    for n in runs:  # benchmark-level p95 / p95*: mean over routes and windows
        x = list(rel[n].values()); summ[n] = {"p95_rel_mean": float(np.mean(x)), "p95_rel_median": float(np.median(x)),
                                              "windows_within_margin_%": float(np.mean(np.array(x) <= 1 + margin) * 100)}
    json.dump({"bench": bench, "margin": margin, "runs": runs, "fixed": fixed,
               "p95_best_us": {f"{k[0][0]}/{k[0][1]}/{k[1]}": v for k, v in best.items()},
               "clients": {f"{k[0]}/{k[1]}": c for k, c in clients.items()},
               "cstar": {f"{d}|{k[0]}/{k[1]}": v for (d, k), v in cstar.items()}, "summary": summ}, open(os.path.join(out, "ref.json"), "w"), indent=1)
    print(json.dumps(summ, indent=1))


if __name__ == "__main__":
    main()
