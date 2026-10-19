# gnn-slo-v1 results (2026-10-19)

Design as used: DESIGN.md (v3: hotel-only, below-knee per-service sweep with Jaeger critical-path attribution and drift
control, median-of-G reference, 20 % margin, no fallback). Estimate: ESTIMATE.txt.

- `labels/`: labels.png (C* per daemon and level, sweep scores per k), cp_share.png (Jaeger tail critical-path shares),
  ref.json (labels, candidates, reference p95, CP shares; drift control on), ref-nocontrol.json.
- `train/`: lambda.png, results.json (lambda grid), weights.json (= dtn-01 `models/gnn_slo_v1.json`, md5 98fd262b…),
  cases.txt, heldout_cmp.json (v3 / v5 / gnn-slo-v1 on the held-out ticks), train.log.
- `eval/`: summary.md / summary.json / summary.png, and per setting (hotel-eval = -users 24, hotel-1024, social-eval =
  -users 12, social-1024) the dsb_eval.py table, latency/cores plots, slo.md / slo.json / p95rel.png (p95 vs G reference).
- scripts: `mercury/slo/` (stamp_windows, slo_ref3, cases3, train_slo, offline_cmp, slo_eval, summary, plot_labels),
  collect3.sh, evaluate3.sh.

Held-out (hotel, 87k ticks, label core-s 1018, used 615):

| model | under % | over % | core-s | requested-core changes/s |
|---|---|---|---|---|
| v3 | 6.74 | 41.1 | 1920 | 32.3 |
| v5 (init) | 7.92 | 54.5 | 2062 | 30.7 |
| gnn-slo-v1 | 1.36 | 52.0 | 1866 | 4.5 |

See eval/summary.md for the closed-loop tables.
