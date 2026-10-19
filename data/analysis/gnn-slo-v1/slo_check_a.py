import json, glob, collections, sys, numpy as np
R = "/home/paul/Mercury/data/dsb/slo-v1"
for m in ("0.10", "0.05"):
    ref = json.load(open(f"/tmp/sloref-{m}/ref.json"))
    cs = ref["cstar"]
    # used cores per daemon per window from k3 run sample.jsonl
    d3 = glob.glob(f"{R}/train-hotel-k3/*/1-saturate")[0]
    S = [json.loads(l) for l in open(d3 + "/sample.jsonl")]
    used = collections.defaultdict(list)
    prev = {}
    for s in S:
        for d, v in s["daemons"].items():
            if d in prev:
                used[d].append((v["usage_usec"] - prev[d]) / (s["iv_ns"] / 1e3))
            prev[d] = v["usage_usec"]
    by = collections.defaultdict(list)
    for k, v in cs.items():
        by[k.split("|")[0]].append(v)
    allv = np.array(list(cs.values()))
    print(f"margin {m}: windows x daemons {len(allv)}, mean label {allv.mean():.2f} cores, fallback to largest (k=3) {np.mean(allv >= 2.95)*100:.1f} %, label 1: {np.mean(allv < 1.5)*100:.1f} %, label 2: {np.mean((allv>=1.5)&(allv<2.5))*100:.1f} %")
    if m == "0.10":
        print(f"{'daemon':28s} {'mean label':>10s} {'%at k3':>7s} {'used mean':>9s} {'used p95':>8s}")
        for d in sorted(by):
            v = np.array(by[d]); u = np.array(used.get(d, [0]))
            print(f"{d:28s} {v.mean():10.2f} {np.mean(v>=2.95)*100:7.1f} {u.mean():9.3f} {np.percentile(u,95):8.3f}")
