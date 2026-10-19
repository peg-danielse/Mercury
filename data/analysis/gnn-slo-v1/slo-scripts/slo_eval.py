"""p95 relative to the generous reference G for gnn-slo-v1 evaluation runs (DESIGN.md v3).
python slo/slo_eval.py <eval_dir> <out_dir> <knee_clients> <G_run_dir> [<G_run_dir> ...]
eval_dir/<ts>/<rep>-<policy>/operate.out (dsb_ab.sh); each G_run_dir contains */1-saturate/operate.out at the same load.
ref(L, r) = median p95 over the G windows at level L. Per policy: median / mean over (rep, level, route) of
median-window p95 / ref, % within 1.2, the same for levels with clients <= knee, and p99 / ref. Writes slo.json, slo.md, p95rel.png."""
import collections, glob, json, os, re, sys
import numpy as np
RX = re.compile(r"^route (\S+) clients (\d+) arrival_rate (\d+) service_rate (\d+) avg_us (\S+) p95_us (\S+) p99_us (\S+) errors (\d+)")


def lv(f):
    out, level, last = collections.defaultdict(list), -1, None
    for l in open(f, errors="replace"):
        m = RX.match(l)
        if not m:
            continue
        c = int(m.group(2))
        if c != last:
            level += 1; last = c
        out[(level, m.group(1))].append((float(m.group(6)), float(m.group(7)), c))
    return out


ev, out, knee = sys.argv[1], sys.argv[2], int(sys.argv[3]); os.makedirs(out, exist_ok=True)
G = [lv(f) for g in sys.argv[4:] for f in glob.glob(os.path.join(g, "*", "1-saturate", "operate.out"))]
ref95, ref99 = collections.defaultdict(list), collections.defaultdict(list)
for g in G:
    for k, w in g.items():
        ref95[k] += [x[0] for x in w]; ref99[k] += [x[1] for x in w]
ref95 = {k: np.median(v) for k, v in ref95.items()}; ref99 = {k: np.median(v) for k, v in ref99.items()}
res = collections.defaultdict(list)
for d in sorted(glob.glob(os.path.join(ev, "*", "*-*", "operate.out"))):
    res[os.path.basename(os.path.dirname(d)).split("-", 1)[1]].append(lv(d))
tab, curves = {}, {}
for pol, L in sorted(res.items()):
    r95, r99, below, bylev = [], [], [], collections.defaultdict(list)
    for x in L:
        for k, w in x.items():
            if k not in ref95:
                continue
            a = np.median([y[0] for y in w]) / ref95[k]; b = np.median([y[1] for y in w]) / ref99[k]
            r95.append(a); r99.append(b); bylev[k[0]].append(a)
            if w[0][2] <= knee:
                below.append(a)
    r95, r99, below = map(np.array, (r95, r99, below))
    tab[pol] = {"repeats": len(L), "p95_rel_median": float(np.median(r95)), "p95_rel_mean": float(r95.mean()),
                "p95_within_1.2_%": float((r95 <= 1.2).mean() * 100), "p95_rel_median_below_knee": float(np.median(below)),
                "p95_within_1.2_below_knee_%": float((below <= 1.2).mean() * 100), "p99_rel_median": float(np.median(r99)), "n": int(len(r95))}
    curves[pol] = {int(k): float(np.median(v)) for k, v in sorted(bylev.items())}
clients = {}
for k, w in (G[0] if G else {}).items():
    clients[k[0]] = w[0][2]
keys = list(next(iter(tab.values())))
lines = ["| metric | " + " | ".join(tab) + " |", "|---|" + "---|" * len(tab)]
for k in keys:
    lines.append(f"| {k} | " + " | ".join((f"{tab[p][k]:.3f}" if isinstance(tab[p][k], float) else str(tab[p][k])) for p in tab) + " |")
open(os.path.join(out, "slo.md"), "w").write("\n".join(lines) + "\n")
json.dump({"ref_runs": sys.argv[4:], "knee_clients": knee, "table": tab, "p95_rel_by_level": curves, "clients": clients}, open(os.path.join(out, "slo.json"), "w"), indent=1)
print("\n".join(lines))
import matplotlib; matplotlib.use("Agg"); import matplotlib.pyplot as plt
fig, ax = plt.subplots(figsize=(9, 4))
for pol, c in curves.items():
    ax.plot(list(c), list(c.values()), "o-", label=pol)
ax.axhline(1.2, ls="--", c="k", lw=0.8, label="margin 1.2")
ax.set_xticks(list(clients)); ax.set_xticklabels([clients[i] for i in clients], fontsize=7)
ax.set_xlabel("clients per route (ramp step)"); ax.set_ylabel("p95 / G reference (median over routes)"); ax.legend(); ax.set_yscale("log")
ax.set_title(os.path.basename(out.rstrip("/"))); fig.tight_layout(); fig.savefig(os.path.join(out, "p95rel.png"), dpi=90)
