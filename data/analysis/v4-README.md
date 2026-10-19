# GNN v4 (exponential per-thread demand) vs v3 vs saturate, 2026-10-18
Data on dtn-01 ~/Mercury/data/gnn/v4 and ~/Mercury/data/dsb/{hotel-v4,social-v4,social-v4-retry}.
- v4a: trained on d0-s0 + d0-s100 (saturate in control, patterns exp/expraw, threads 2/4/8/16, totals 1.5/3/6/10, 64 cases)
- v4b: + dagger-r0 (v4a in control, seed base 200, 24 cases)
- v4:  + dagger-r1 (v4b in control, seed base 300, 24 cases) -> ~/Mercury/models/gnn_v4.json
Same architecture as v3 (LocalGNN_DB, K=3, H=16, 1,633 params, --drop-cpuset, 100 epochs).
Evaluation: v4-offline-* (open loop, unseen seeds 1000/2000 and v3's held-out old-pattern cases), v4-closedloop-exp
(24 cases, seeds 1000/2000), v4-closedloop-oldpatterns (30-case v3 protocol; saturate and v3 are the 2026-10-17 runs),
v4-stressng-le16, v4-dsb-hotel. Social: every run except the first saturate one failed with
"loadgen error: compose-post: LOADING Redis is loading the dataset in memory" (all policies, retried), so there is no social result.
