"""Combined gnn-slo-v1 evaluation table and plot. python slo/summary.py <eval_root> (contains <setting>/dsb.json and slo.json)"""
import json, os, sys
import matplotlib; matplotlib.use("Agg"); import matplotlib.pyplot as plt
root = sys.argv[1]; S = [s for s in ("hotel-eval", "hotel-1024", "social-eval", "social-1024") if os.path.exists(os.path.join(root, s, "slo.json"))]
P = ["saturate", "gnn-nopin-gnn_v3", "gnn-nopin-gnn_slo_v1"]; N = {"saturate": "saturate", "gnn-nopin-gnn_v3": "v3", "gnn-nopin-gnn_slo_v1": "gnn-slo-v1"}
M = [("throughput_req_s", "throughput req/s", "d"), ("p95_med_ms", "p95 ms (median)", "d"), ("p95_rel_median", "p95/ref median", "s"),
     ("p95_within_1.2_%", "% (level,route) p95 <= 1.2 ref", "s"), ("p95_rel_median_below_knee", "p95/ref below knee", "s"), ("p99_med_ms", "p99 ms (median)", "d"),
     ("p99_peak_med_ms", "p99 ms at peak users", "d"), ("core_s_alloc", "core-s allocated", "d"), ("core_s_used", "core-s used", "d"),
     ("changes_per_s", "allocation changes/s", "d"), ("errors", "errors / run", "d")]
lines, data = [], {}
for s in S:
    d = json.load(open(os.path.join(root, s, "dsb.json"))); e = json.load(open(os.path.join(root, s, "slo.json")))["table"]
    data[s] = {p: {k: (d if src == "d" else e)[p][k] for k, _, src in M} for p in P}
    lines += [f"### {s}", "", "| metric | " + " | ".join(N[p] for p in P) + " |", "|---|---|---|---|"]
    for k, lab, _ in M:
        lines.append(f"| {lab} | " + " | ".join(f"{data[s][p][k]:.3f}" if isinstance(data[s][p][k], float) else str(data[s][p][k]) for p in P) + " |")
    lines.append("")
open(os.path.join(root, "summary.md"), "w").write("\n".join(lines)); json.dump(data, open(os.path.join(root, "summary.json"), "w"), indent=1)
print("\n".join(lines))
keys = [("throughput_req_s", "throughput req/s"), ("p95_rel_median", "p95 / G ref (median)"), ("p99_peak_med_ms", "p99 at peak (ms)"),
        ("core_s_alloc", "core-s allocated"), ("core_s_used", "core-s used"), ("changes_per_s", "changes/s")]
fig, ax = plt.subplots(len(keys), 1, figsize=(10, 2.3 * len(keys)))
for i, (k, lab) in enumerate(keys):
    for j, p in enumerate(P):
        ax[i].bar([x + (j - 1) * 0.27 for x in range(len(S))], [data[s][p][k] for s in S], 0.27, label=N[p], color=f"C{j}")
    ax[i].set_xticks(range(len(S))); ax[i].set_xticklabels(S); ax[i].set_ylabel(lab, fontsize=8)
ax[0].legend(ncol=3, fontsize=8); ax[0].set_title("gnn-slo-v1 evaluation (no pin, least_loaded, 2 repeats, Jaeger 2 % on; social unseen in training)")
fig.tight_layout(); fig.savefig(os.path.join(root, "summary.png"), dpi=90)
