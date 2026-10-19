import json, glob, collections, re, numpy as np
R = "/home/paul/Mercury/data/dsb"
# (a) fallback fraction (no fixed run within margin)
for m in ("0.10", "0.05"):
    W = {k: json.load(open(f"/tmp/sloref-{m}/k{k}.windows.json")) for k in (1, 2, 3)}
    fb = tot = 0; proj = []
    for d in W[1]:
        for r1, r2, r3 in zip(W[1][d], W[2][d], W[3][d]):
            sc = [r1[3], r2[3], r3[3]]; tot += 1
            if all(s > 1 + float(m) for s in sc): fb += 1; proj.append(8)
            else: proj.append(r1[2])
    print(f"(a) margin {m}: no fixed-k run within margin (fallback to largest) in {fb/tot*100:.1f} % of daemon-windows; projected mean label if k=8 were the largest: {np.mean(proj):.2f}")
# p95 quantisation
RX = re.compile(r"^route (\S+) clients (\d+) arrival_rate (\d+) service_rate (\d+) avg_us (\S+) p95_us (\S+) p99_us (\S+) errors (\d+)")
vals = set()
for f in glob.glob(f"{R}/slo-v1/train-hotel-k*/*/1-saturate/operate.out"):
    for l in open(f):
        mm = RX.match(l)
        if mm: vals.add(float(mm.group(6)))
v = np.array(sorted(vals)); rat = v[1:] / v[:-1]
print(f"    p95 values: {len(v)} distinct over 3 runs; median ratio between neighbouring values {np.median(rat):.3f} (log-bucketed)")
# (b) knee
def levels(f):
    L = collections.OrderedDict(); last = None; i = -1
    for l in open(f):
        mm = RX.match(l)
        if not mm: continue
        c = int(mm.group(2))
        if c != last: i += 1; last = c
        L.setdefault((i, c), []).append((mm.group(1), int(mm.group(4)), float(mm.group(5)), float(mm.group(6))))
    return L
for name, f in [("hotel k3 768u", glob.glob(f"{R}/slo-v1/train-hotel-k3/*/1-saturate/operate.out")[0]),
                ("hotel k1 768u", glob.glob(f"{R}/slo-v1/train-hotel-k1/*/1-saturate/operate.out")[0]),
                ("hotel saturate 1024u", glob.glob(f"{R}/hotel-v5/*/1-saturate/operate.out")[0]),
                ("social saturate 1024u", glob.glob(f"{R}/social-v5/*/1-saturate/operate.out")[0])]:
    print(f"(b) {name}: clients/route -> throughput req/s (sum of routes, mean of 4 windows), median p95 ms, Little latency ms (pop/thr)")
    rows = []
    for (i, c), x in levels(f).items():
        nr = len({r for r, *_ in x}); thr = sum(s for _, s, _, _ in x) / 4
        rows.append(f"{c}:{thr/1000:.1f}k/{np.median([p for *_, p in x])/1000:.1f}/{c*nr/max(thr,1)*1000:.1f}")
        if i >= 10: break
    print("    " + "  ".join(rows))
# (c) wait per slice from dsb_sample per daemon: saturate vs gnn (hotel-v5 runs, 1024 users)
for sysn in ("hotel-v5", "social-v5"):
    for run in sorted(glob.glob(f"{R}/{sysn}/*/1-*")):
        S = [json.loads(l) for l in open(run + "/sample.jsonl")]
        agg = collections.defaultdict(lambda: [0, 0, 0.0, 0])
        for s in S:
            for d, v in s["daemons"].items():
                a = agg[d]; a[0] += v.get("wait_ns", 0); a[1] += v.get("slices", 0); a[2] += v["cores"]; a[3] += 1
        ws = [a[0] / max(a[1], 1) / 1e3 for d, a in agg.items() if not d.endswith("loadgen") and a[1] > 0]
        tw = sum(a[0] for d, a in agg.items() if not d.endswith("loadgen")); ts = sum(a[1] for d, a in agg.items() if not d.endswith("loadgen"))
        cores = sum(a[2] / a[3] for d, a in agg.items() if not d.endswith("loadgen"))
        print(f"(c) {sysn}/{run.split('/')[-1]:24s} mean cores (sum over daemons) {cores:5.1f}  wait/slice all daemons {tw/max(ts,1)/1e3:6.1f} us  median daemon {np.median(ws):6.1f} us")
