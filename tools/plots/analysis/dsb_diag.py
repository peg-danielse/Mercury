"""Diagnosis of the DSB gnn vs saturate A/B from recorded data only. python diag/dsb_diag.py <sys> <run_dir> <out_dir>"""
import collections, glob, json, os, re, sys
import numpy as np
import matplotlib; matplotlib.use("Agg"); import matplotlib.pyplot as plt
sysname, run, out = sys.argv[1:4]; os.makedirs(out, exist_ok=True)
RX = re.compile(r"^route (\S+) clients (\d+) arrival_rate (\d+) service_rate (\d+) avg_us (\S+) p95_us (\S+) p99_us (\S+) errors (\d+) cpus (.*?) gen_cpus")
GX = re.compile(r"^gnn daemon_id=(\S+) cores=(\d+) required=(\d+) target=(\d+) demand=(\S+) pred=(\S+) breaks=(\d+) nodes=(\d+) threads=(\d+) ticks=(\d+) mean_us read=(\d+) infer=(\d+) cpuset=(\d+) act=(\d+)")
WIN = 2.5

def load(d):
    S = [json.loads(l) for l in open(f"{d}/sample.jsonl")]
    S = [s for s in S if s["daemons"]]
    gen = [i for i, s in enumerate(S) if any(k.endswith("loadgen") for k in s["daemons"])]
    t0 = S[gen[0]]["ts_ns"] if gen else S[0]["ts_ns"]
    names = sorted({k for s in S for k in s["daemons"] if not k.endswith("loadgen")})
    t = np.array([(s["ts_ns"] - t0) / 1e9 for s in S]); iv = np.array([s["iv_ns"] / 1e9 for s in S])
    cores = np.array([[s["daemons"].get(n, {}).get("cores", 0) for n in names] for s in S], float)
    use = np.array([[s["daemons"].get(n, {}).get("usage_usec", np.nan) for n in names] for s in S], float)
    rate = np.vstack([np.zeros(len(names)), np.nan_to_num(np.diff(use, axis=0)).clip(0) / 1e6 / iv[1:, None]])
    busy = np.array([[s["daemons"].get(n, {}).get("busy", 0) for n in names] for s in S], float)
    cpu0 = np.array([s["cpu0_util"] for s in S])
    win = []  # per window: route -> (clients, served, p99)
    cur = {}
    for l in open(f"{d}/operate.out", errors="replace"):
        m = RX.match(l)
        if not m: continue
        r = m.group(1)
        if r in cur: win.append(cur); cur = {}
        cur[r] = (int(m.group(2)), int(m.group(4)), float(m.group(7)) / 1e3, float(m.group(6)) / 1e3)
    if cur: win.append(cur)
    g = collections.defaultdict(list)
    for l in open(f"{d}/mercury.log", errors="replace"):
        m = GX.match(l)
        if m: g[m.group(1)].append(tuple(float(x) for x in m.groups()[1:]))
    return dict(t=t, iv=iv, names=names, cores=cores, rate=rate, busy=busy, cpu0=cpu0, win=win, gnn=g)

R = collections.defaultdict(list)
for d in sorted(glob.glob(f"{run}/*-*")):
    R[os.path.basename(d).split("-", 1)[1]].append(load(d))
pols = ["saturate", "gnn"]
rep = {}
names = R["saturate"][0]["names"]
short = lambda n: n.split("-", 1)[1]
# --- per daemon table
rows = []
for n in names:
    row = {"daemon": short(n)}
    for p in pols:
        A = U = Sat = Ch = Dur = 0.0
        for x in R[p]:
            j = x["names"].index(n); m = x["t"] > 0
            c, u, iv = x["cores"][m, j], x["rate"][m, j], x["iv"][m]
            A += (c * iv).sum(); U += (u * iv).sum()
            Sat += (iv * (u >= 0.9 * c) * (c > 0)).sum(); Dur += iv[c > 0].sum()
            Ch += ((np.diff(c) != 0) & (c[1:] > 0) & (c[:-1] > 0)).sum()
        k = len(R[p])
        row[p] = {"alloc_cs": A / k, "used_cs": U / k, "util": U / max(A, 1e-9), "sat_%": 100 * Sat / max(Dur, 1e-9), "chg_s": Ch / max(Dur, 1e-9)}
    rows.append(row)
rep["per_daemon"] = rows
# --- per route per step (median over windows and repeats)
steps = collections.defaultdict(lambda: collections.defaultdict(list))
for p in pols:
    for x in R[p]:
        prev = None; leg = "up"
        for w in x["win"]:
            c = next(iter(w.values()))[0]
            if prev is not None and c < prev: leg = "down"
            prev = c
            for r, (cl, sv, p99, p95) in w.items():
                steps[(p, r)][(leg, cl)].append((sv, p99))
routes = sorted({r for (_, r) in steps})
rt = {}
for r in routes:
    rt[r] = {}
    for p in pols:
        allw = [v for k in steps[(p, r)].values() for v in k]
        rt[r][p] = {"served_mean": float(np.mean([a for a, _ in allw])), "p99_med": float(np.median([b for _, b in allw]))}
rep["per_route"] = rt
# by user level, total throughput ratio
lv = collections.defaultdict(dict)
for p in pols:
    tot = collections.defaultdict(list)
    for r in routes:
        for k, v in steps[(p, r)].items():
            tot[k].append(np.mean([a for a, _ in v]))
    for k, v in tot.items(): lv[k][p] = float(np.sum(v))
rep["throughput_by_step"] = {f"{k[0]}-{k[1]}": v for k, v in sorted(lv.items(), key=lambda z: (z[0][0] != "up", z[0][1] if z[0][0] == "up" else -z[0][1]))}
# --- gnn log: pred vs used, breaks, tick rate (ticks per logged ~1 s line), act cost by daemon
gl = {}
for n in names:
    L = [e for x in R["gnn"] for e in x["gnn"].get(n, [])]
    L = [e for e in L if e[9] < 20000]  # skip first tick
    if not L: continue
    tk = np.array([e[8] for e in L]); pred = np.array([e[4] for e in L]); req = np.array([e[1] for e in L]); cores = np.array([e[0] for e in L])
    gl[short(n)] = {"lines": len(L), "pred_mean": float(pred.mean()), "required_mean": float(req.mean()), "cores_mean": float(cores.mean()),
                    "breaks": int(sum(e[5] for e in L)), "read_us": float(np.average([e[9] for e in L], weights=tk)),
                    "act_us": float(np.average([e[12] for e in L], weights=tk)), "cpuset_us": float(np.average([e[11] for e in L], weights=tk)),
                    "threads_mean": float(np.mean([e[7] for e in L]))}
rep["gnn_log"] = gl
# tick lateness: per daemon, ticks/s from consecutive periodic lines is not exact; use total ticks logged / attached time
tl = {}
for n in names:
    T = []
    for x in R["gnn"]:
        L = x["gnn"].get(n, [])
        if L: T.append(sum(e[8] for e in L))
    if T: tl[short(n)] = float(np.mean(T))
rep["gnn_ticks_total_per_run"] = tl
rep["cpu0_util_%"] = {p: float(100 * np.mean([x["cpu0"].mean() for x in R[p]])) for p in pols}
rep["cpu0_p_ge95_%"] = {p: float(100 * np.mean([np.mean(x["cpu0"] >= 0.95) for x in R[p]])) for p in pols}
# --- throughput dips vs cpuset changes (gnn and saturate): per window total served vs total cpuset changes in window
corr = {}
pts = {}
for p in pols:
    xs, ys = [], []
    for x in R[p]:
        ch = np.r_[0, (np.diff(x["cores"], axis=0) != 0).sum(1)]
        for k, w in enumerate(x["win"]):
            a, b = k * WIN, (k + 1) * WIN
            m = (x["t"] > a) & (x["t"] <= b)
            cl = next(iter(w.values()))[0]
            xs.append((cl, ch[m].sum())); ys.append(sum(v[1] for v in w.values()))
    # normalise throughput by the median for that user level -> dip
    byc = collections.defaultdict(list)
    for (cl, c), y in zip(xs, ys): byc[cl].append(y)
    med = {cl: np.median(v) for cl, v in byc.items()}
    rel = np.array([y / med[cl] for (cl, c), y in zip(xs, ys)]); chg = np.array([c for (_, c) in xs])
    corr[p] = float(np.corrcoef(chg, rel)[0, 1]) if chg.std() > 0 else float("nan")
    pts[p] = (chg, rel)
rep["corr_changes_vs_relative_throughput"] = corr
json.dump(rep, open(f"{out}/diag-{sysname}.json", "w"), indent=1)
# --- plots
crit = [n for n in names if any(k in n for k in ("ComposePost", "HomeTimeline", "UserTimeline", "PostStorage", "SocialGraph", "mongod", "11311", "redis", "rabbitmq", "frontend", "search", "geo", "profile", "rate", "11211"))][:9]
fig, ax = plt.subplots(len(crit), 2, figsize=(15, 2.0 * len(crit)), sharex=True)
for j, p in enumerate(pols):
    x = R[p][0]
    for i, n in enumerate(crit):
        k = x["names"].index(n)
        ax[i, j].step(x["t"], x["cores"][:, k], where="post", color="C3", lw=1, label="cpuset cores")
        ax[i, j].plot(x["t"], x["rate"][:, k], color="C0", lw=0.6, label="used (cpu.stat)")
        ax[i, j].set_ylabel(short(n), fontsize=7)
        if i == 0: ax[i, j].set_title(f"{sysname} {p} repeat 1"); ax[i, j].legend(fontsize=7)
for a in ax[-1]: a.set_xlabel("time since loadgen start (s)")
fig.tight_layout(); fig.savefig(f"{out}/{sysname}-per-daemon-cores-used.png", dpi=100)
fig, ax = plt.subplots(1, 3, figsize=(16, 4))
for i, p in enumerate(pols):
    ax[0].scatter(pts[p][0], pts[p][1], s=6, alpha=0.5, label=f"{p} r={corr[p]:.2f}")
ax[0].set_xlabel("cpuset changes in window (all daemons)"); ax[0].set_ylabel("window throughput / median at same users"); ax[0].legend()
for i, p in enumerate(pols):
    x = R[p][0]; served = [sum(v[1] for v in w.values()) for w in x["win"]]
    ax[1].plot(np.arange(len(served)) * WIN, served, label=f"{p} throughput", color=f"C{i}")
    ch = np.r_[0, (np.diff(x["cores"], axis=0) != 0).sum(1)]
    ax[2].plot(x["t"], np.convolve(ch, np.ones(10), "same"), color=f"C{i}", lw=0.7, label=f"{p} changes/s")
ax[1].set_xlabel("time (s)"); ax[1].set_ylabel("req/s, all routes (repeat 1)"); ax[1].legend()
ax[2].set_xlabel("time (s)"); ax[2].set_ylabel("cpuset changes per s (all daemons)"); ax[2].legend()
fig.tight_layout(); fig.savefig(f"{out}/{sysname}-throughput-vs-changes.png", dpi=100)
x = R["gnn"][0]; fig, ax = plt.subplots(figsize=(12, 3))
ax.plot(x["t"], x["cpu0"] * 100, lw=0.6, label="gnn"); y = R["saturate"][0]; ax.plot(y["t"], y["cpu0"] * 100, lw=0.6, label="saturate")
ax.set_ylabel("Mercury CPU 0 util %"); ax.set_xlabel("time (s)"); ax.legend(); fig.tight_layout(); fig.savefig(f"{out}/{sysname}-cpu0.png", dpi=100)
print(json.dumps({k: rep[k] for k in ("per_route", "cpu0_util_%", "cpu0_p_ge95_%", "corr_changes_vs_relative_throughput", "throughput_by_step")}, indent=0))
print("daemon | sat alloc/used util sat% chg/s | gnn alloc/used util sat% chg/s")
for r in rows:
    f = lambda d: f'{d["alloc_cs"]:.0f}/{d["used_cs"]:.0f} {d["util"]:.2f} {d["sat_%"]:.1f} {d["chg_s"]:.2f}'
    print(r["daemon"], "|", f(r["saturate"]), "|", f(r["gnn"]))
print("gnn log:"); [print(k, {a: round(b, 2) for a, b in v.items()}) for k, v in gl.items()]
print("ticks per run:", {k: int(v) for k, v in tl.items()})
