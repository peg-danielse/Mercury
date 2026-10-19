"""Plots for the gnn-slo-v1 labels (slo_ref3.py ref.json): C* per daemon and level, candidate scores, tail critical-path shares.
python slo/plot_labels.py <labels_dir> <out_dir>"""
import collections, json, os, sys
import numpy as np
import matplotlib; matplotlib.use("Agg"); import matplotlib.pyplot as plt
d = json.load(open(os.path.join(sys.argv[1], "ref.json"))); out = sys.argv[2]; os.makedirs(out, exist_ok=True)
cl = {int(k): v for k, v in d["clients"].items()}; lv = sorted(cl)
dm = sorted({k.split("|")[0] for k in d["candidates"]})
C = np.full((len(dm), len(lv)), np.nan)
for k, v in d["cstar"].items():
    a, b = k.split("|"); C[dm.index(a), lv.index(int(b))] = v
fig, ax = plt.subplots(1, 2, figsize=(16, 6))
im = ax[0].imshow(C, aspect="auto", cmap="viridis", vmin=1, vmax=4)
for i in range(len(dm)):
    for j in range(len(lv)):
        ax[0].text(j, i, "-" if np.isnan(C[i, j]) else int(C[i, j]), ha="center", va="center", color="w", fontsize=7)
ax[0].set_yticks(range(len(dm))); ax[0].set_yticklabels([x.split("-", 1)[1] for x in dm]); ax[0].set_xticks(range(len(lv)))
ax[0].set_xticklabels([cl[i] for i in lv]); ax[0].set_xlabel("clients per route (ramp step)")
ax[0].set_title(f"label C* (cores), margin {d['margin']:.0%}, mean {d['summary']['mean_label']:.2f}"); fig.colorbar(im, ax=ax[0])
# candidate score (median over levels) per k
for i, x in enumerate(dm):
    ks = collections.defaultdict(list)
    for j in lv:
        for k, s, n in d["candidates"].get(f"{x}|{j}", []):
            ks[k].append(s)
    for k, v in ks.items():
        ax[1].scatter(i + (k - 2.5) * 0.12, np.max(v), c=f"C{k}", marker="v", s=18)
        ax[1].scatter(i + (k - 2.5) * 0.12, np.median(v), c=f"C{k}", s=24)
ax[1].axhline(1 + d["margin"], ls="--", c="k", lw=0.8); ax[1].set_xticks(range(len(dm))); ax[1].set_xticklabels([x.split("-", 1)[1] for x in dm], rotation=45)
ax[1].set_ylabel("score (CP-weighted p95/ref, drift-corrected): median (o) and max (v) over levels"); ax[1].legend(handles=[plt.Line2D([], [], ls="", marker="o", c=f"C{k}", label=f"k={k}") for k in (1, 2, 3, 4)], fontsize=7)
ax[1].set_title("per-service sweep (median over 2 passes; k=3 for heavy / 2 for light = G)")
fig.tight_layout(); fig.savefig(os.path.join(out, "labels.png"), dpi=90)
# tail critical-path share per route (G, pooled over levels)
cp = collections.defaultdict(list)
for k, v in d["cp_share_tail_G"].items():
    l, r, x = k.split("/"); cp[(r, x)].append(v)
routes = sorted({r for r, _ in cp}); progs = sorted({x for _, x in cp})
M = np.array([[np.mean(cp.get((r, x), [0])) for x in progs] for r in routes])
fig, ax = plt.subplots(figsize=(12, 5)); im = ax.imshow(M, aspect="auto", cmap="magma")
ax.set_xticks(range(len(progs))); ax.set_xticklabels(progs, rotation=45); ax.set_yticks(range(len(routes))); ax.set_yticklabels(routes)
for i in range(len(routes)):
    for j in range(len(progs)):
        if M[i, j] >= 0.02:
            ax.text(j, i, f"{M[i, j]:.2f}", ha="center", va="center", color="c", fontsize=7)
ax.set_title("Jaeger tail (>=p90) critical-path share per route and daemon, G runs, mean over levels"); fig.colorbar(im)
fig.tight_layout(); fig.savefig(os.path.join(out, "cp_share.png"), dpi=90)
print("ok")
