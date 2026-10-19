"""Offline per-thread / per-tick error of several exported models on the same cases.
  offline_eval.py OUTDIR CASES_FROM_RESULTS_JSON name=model.pt[:drop] ...
Uses the test_cases list of a results.json (held-out daemons) and train_gnn's metrics."""
import json, os, sys
import numpy as np, torch
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import train_gnn as tg
from mdata import load_case, ffd_cores
out, rj = sys.argv[1], sys.argv[2]; os.makedirs(out, exist_ok=True)
models = {}
for a in sys.argv[3:]:
    name, p = a.split("=", 1); drop = p.endswith(":drop"); p = p.removesuffix(":drop")
    m = tg.LocalGNNDB(len(tg.F_NAMES)); m.load_state_dict(torch.load(p)); m.eval(); models[name] = (m, drop)
tg.DIST = tg.dtn01_distance()
cases = json.load(open(rj))["test_cases"]
acc = {n: {"thr": [], "thr_cont": [], "thr_busy": [], "cores_ffd": [], "cores_sum": []} for n in list(models) + ["run+wait", "saturate"]}
per = {}
with torch.no_grad():
    for c in cases:
        d = load_case(c); X, S, y, v = tg.case_tensors(d); vv = v.numpy()[:, None] & (d["dem"] >= 0)
        dem = d["dem"]; req = d["req_ffd"]; cont = (d["wait"] >= 0.02) & vv; busy = (dem >= 0.1) & vv
        preds = {}
        for n, (m, drop) in models.items():
            Xn = X.clone()
            if drop: Xn[:, :, 7] = 0
            preds[n] = np.clip(m(Xn, S).numpy()[:, :-1], 0, None)
        preds["run+wait"] = np.clip(d["run"] + d["wait"], 0, 1)
        per[c] = {}
        for n, p in preds.items():
            e = np.abs(p - dem)
            acc[n]["thr"].append(e[vv]); acc[n]["thr_cont"].append(e[cont]); acc[n]["thr_busy"].append(e[busy])
            cf = np.array([ffd_cores(r) for r in p])
            acc[n]["cores_ffd"].append(np.abs(cf - req)[v.numpy()])
            acc[n]["cores_sum"].append(np.abs(p.sum(1) - np.clip(dem, 0, 1).sum(1))[v.numpy()])
            per[c][n] = {"thr": float(e[vv].mean()), "cores_ffd": float(np.abs(cf - req)[v.numpy()].mean()), "pred_sum_mean": float(p.sum(1).mean()), "label_sum_mean": float(np.clip(dem, 0, 1).sum(1).mean())}
        acc["saturate"]["cores_ffd"].append(np.abs(d["cpus"] - req)[v.numpy()])
res = {"cases": cases, "mae": {n: {k: float(np.concatenate(x).mean()) for k, x in a.items() if x} for n, a in acc.items()}, "per_case": per,
       "n_ticks": int(sum(len(x) for x in acc["run+wait"]["cores_ffd"]))}
json.dump(res, open(os.path.join(out, "offline.json"), "w"), indent=1)
with open(os.path.join(out, "offline.md"), "w") as f:
    f.write("| model | thread MAE | contended thread MAE | busy (label>=0.1) thread MAE | cores (FFD) MAE | sum-demand MAE |\n|---|---|---|---|---|---|\n")
    for n, a in res["mae"].items():
        f.write(f"| {n} | " + " | ".join(f"{a[k]:.4f}" if k in a else "-" for k in ("thr", "thr_cont", "thr_busy", "cores_ffd", "cores_sum")) + " |\n")
    f.write(f"\nheld-out cases: {', '.join(os.path.basename(c) for c in cases)}; {res['n_ticks']} ticks\n")
print(open(os.path.join(out, "offline.md")).read())
