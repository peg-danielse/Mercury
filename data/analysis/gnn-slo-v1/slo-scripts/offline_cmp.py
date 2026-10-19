"""Held-out comparison of models on gnn-slo-v1 test cases. python slo/offline_cmp.py <out.json> name=<weights.json> ... case=..."""
import json, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import torch, train_slo as ts
ts.tg.DIST = ts.tg.dtn01_distance()
cs = [ts.build(a.split("=", 1)[1]) for a in sys.argv[2:] if a.startswith("case=") and a.endswith("@test")]
res = {}
for a in sys.argv[2:]:
    if a.startswith("case="):
        continue
    n, w = a.split("=", 1); W = json.load(open(w))
    m = ts.tg.LocalGNNDB(len(ts.tg.F_NAMES)); m.load_state_dict({k: torch.tensor(v) for k, v in W.items()}); m.eval()
    res[n] = ts.evaluate(m, cs); print(n, res[n], flush=True)
json.dump(res, open(sys.argv[1], "w"), indent=1)
