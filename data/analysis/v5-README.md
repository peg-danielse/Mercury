# GNN v5 (v3 data + v4 exponential data, balanced) vs v4, v3, saturate. 2026-10-18
- v5a: 112 v3 training cases (stratified 22-23 per old pattern, never v3's held-out cases) + 112 v4 exp cases (v5/train_*_sources.txt)
- v5b: + dagger-r0 (v5a in control: 30 old cases [threads 4/8/16 x totals 2/4.5] + 30 exp cases [totals 2/3/4.5/6/10], seed base 400)
- v5:  + dagger-r1 (v5b in control, seed base 500) -> dtn-01 ~/Mercury/models/gnn_v5.json (not default)
Raw data: dtn-01 ~/Mercury/data/gnn/v5, ~/Mercury/data/dsb/hotel-v5. Old-pattern closed loop: saturate/v3 from 2026-10-17, v4/v5 re-run.
stress-ng saturate/v3/v4 from data/gnn/v4. Social skipped: startup failure is a Redis persistence problem (see report), not a wait.
