"""Offline core-count evaluation of exported models on recorded cases (open loop).
python offline_eval.py <out_dir> <cases.txt|run_dir,run_dir> name=weights.json ...
Per model: per-thread demand MAE, required-cores MAE (FFD of predictions vs FFD of labels), too-few %
(predicted < required), too-many % (predicted > required); plus saturate's recorded cpuset for reference.
Writes offline.json, table.md and offline.png."""
import json, os, sys
import numpy as np, torch
import matplotlib; matplotlib.use("Agg"); import matplotlib.pyplot as plt
import train_gnn as tg
from mdata import cases, ffd_cores, load_case

tg.DIST = tg.dtn01_distance()
out, src = sys.argv[1], sys.argv[2]; os.makedirs(out, exist_ok=True)
names = [l.strip() for l in open(src) if l.strip()] if src.endswith(".txt") else [os.path.join(r, c) for r in src.split(",") for c in cases(r)]
models = {}
for a in sys.argv[3:]:
    k, w = a.split("=", 1); W = json.load(open(w))
    m = tg.LocalGNNDB(len(tg.F_NAMES), H=len(W["g1.b"]), K=len(W["g1.W"])); m.load_state_dict({kk: torch.tensor(v) for kk, v in W.items()}); m.eval(); models[k] = m
acc = {k: {"thr": [], "err": [], "d": []} for k in list(models) + ["recorded_cpuset"]}
for c in names:
    d = load_case(c); X, S, y, v = tg.case_tensors(d); X[:, :, 7] = 0
    v = v.numpy(); vv = v[:, None] & (d["dem"] >= 0); req = d["req_ffd"][v]
    for k, m in models.items():
        with torch.no_grad(): p = m(X, S).numpy()[:, :-1]
        est = np.array([ffd_cores(r) for r in p])[v]
        acc[k]["thr"].append(np.abs(p - d["dem"])[vv]); acc[k]["d"].append(est - req)
    acc["recorded_cpuset"]["d"].append(d["cpus"][v] - req)
res = {"cases": len(names)}
for k, a in acc.items():
    dd = np.concatenate(a["d"])
    res[k] = {"ticks": int(len(dd)), "cores_mae": float(np.abs(dd).mean()), "too_few_%": float((dd < 0).mean() * 100),
              "too_many_%": float((dd > 0).mean() * 100), "exact_%": float((dd == 0).mean() * 100), "bias": float(dd.mean())}
    if a["thr"]: res[k]["thread_demand_mae"] = float(np.concatenate(a["thr"]).mean())
json.dump(res, open(os.path.join(out, "offline.json"), "w"), indent=1)
ks = [k for k in acc]; mets = ["thread_demand_mae", "cores_mae", "too_few_%", "too_many_%", "exact_%", "bias"]
with open(os.path.join(out, "table.md"), "w") as f:
    f.write(f"cases: {len(names)} (source {src})\n\n| metric | " + " | ".join(ks) + " |\n|---|" + "---|" * len(ks) + "\n")
    for m in mets: f.write(f"| {m} | " + " | ".join(f"{res[k][m]:.3f}" if m in res[k] else "n/a" for k in ks) + " |\n")
fig, ax = plt.subplots(1, 3, figsize=(13, 4))
for i, m in enumerate(["cores_mae", "too_few_%", "too_many_%"]):
    ax[i].bar(ks, [res[k][m] for k in ks], color=[f"C{j}" for j in range(len(ks))]); ax[i].set_title(m); ax[i].tick_params(axis="x", rotation=20)
fig.suptitle(f"offline core-count error, {len(names)} cases"); fig.tight_layout(); fig.savefig(os.path.join(out, "offline.png"), dpi=90)
print(open(os.path.join(out, "table.md")).read())
