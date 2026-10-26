# Mercury plotting code

All plotting code from the repository, gathered in one place. The top-level `plot_runs.py` and `extract_runs.py` and `rings/plot_rings.py` are the tracked pipeline files. The `analysis/` and `gnn-slo/` scripts are verbatim copies (byte-identical, checked by md5) of the scripts in the run folders under `data/analysis/` and `data/gnn/`. Those originals stay where they are, so the run folders keep working unchanged.

## Latest pipeline: paper figures (`plot_runs.py`, `extract_runs.py`)

The newest data is the zeroctl comparison (`src/test/compare_systems.sh`, last changed in `a67e935`). It flows through two steps:

```
compare_systems.sh OUT/            one directory per system, OUT/<system>/r<n>/DONE per run
  -> extract_runs.py OUT [...] -o runs.json [--bin-s 5]        stdlib only; runs on the experiment host
  -> plot_runs.py runs.json[:renames] -d figs/ [--width 3.4] [--critical hotels,reservation]
                     [--summary p95:hotels,err:reservation,err:optional,pool] [--latency hotels]
```

- `extract_runs.py` reduces the raw results to one JSON: per-route latency histograms, antagonist phases, 5 s bins, pool size, and core-seconds.
- `plot_runs.py` writes vector PDFs: `summary.pdf`, `cost.pdf`, `phases.pdf`, `timeline.pdf`. Percentiles come from the merged histograms. Failed requests are excluded unless `--errors-as-ms` is given.
- `plot_runs.py` needs matplotlib. Run it on a workstation, not on the experiment host.

## Other plotting scripts

| Script | Purpose | Usage (from its docstring) |
|---|---|---|
| `rings/plot_rings.py` | Allocation against use per daemon, from Mercury's 10 ms cgroup ring archives (`rings/*.mring`) | `plot_rings.py RINGS_DIR [--prefix hotel-] [--from S] [--to S] [--smooth MS] [--out FILE] [--show]` |
| `analysis/offline_eval.py` | Open-loop core-count evaluation of exported GNN models on recorded cases | `offline_eval.py <out_dir> <cases.txt\|run_dir,...> name=weights.json ...` → `offline.json`, `table.md`, `offline.png` |
| `analysis/eval_closed_loop.py` | Closed-loop A/B of policies on gnnload cases against ground-truth labels | `eval_closed_loop.py <out_dir> name1=<run_dir1> ...` → `table.md`, `ab.json`, `summary.png`, one PNG per case |
| `analysis/dsb_eval_ab.py` | DSB closed-loop A/B, two policies (`src/test/dsb_ab.sh` runs) | `dsb_eval_ab.py <run_dir> <out_dir> [label]` → `table.md`, `dsb.json`, plots |
| `analysis/dsb_eval_cmp.py` | Same as above, for any number of policies | `dsb_eval_cmp.py <run_dir> <out_dir> [label]` |
| `analysis/dsb_diag.py` | Diagnosis of DSB gnn vs saturate from recorded data only | `dsb_diag.py <sys> <run_dir> <out_dir>` |
| `analysis/pin_eval.py` | No-pinning check (social, 1→64→1 users): throughput, p99, run-queue wait | `pin_eval.py RUNDIR OUTDIR` |
| `gnn-slo/train_slo.py` (now `tools/learn/slo/train_slo.py`) | Fine-tunes v5 on DSB traces with SLO-based allocation labels (gnn-slo-v1) | `train_slo.py <out_dir> --init <weights.json> --lambdas ... --epochs E case=<trace>@<windows>@<daemon>[@test] ...` |
| `gnn-slo/slo_eval.py` | p95 relative to a generous reference run for gnn-slo-v1 evaluations | `slo_eval.py <eval_dir> <out_dir> <knee_clients> <G_run_dir> ...` |
| `gnn-slo/summary.py` | Combined gnn-slo-v1 evaluation table and plot | `summary.py <eval_root>` |
| `gnn-slo/plot_labels.py` | Plots of the gnn-slo-v1 labels (C* per daemon and level) | `plot_labels.py <labels_dir> <out_dir>` |

## Where each copy came from

Each script is listed with the original paths it was copied from. A copy in a run folder is identical to the one in `plots/`.

- `plot_runs.py`, `extract_runs.py` (top level): tracked since `a67e935`. `extract_runs.py` has no plotting; it's included because the figures depend on it.
- `rings/plot_rings.py`: moved from `tools/plot_rings.py` (`git mv`, history kept).
- `analysis/offline_eval.py` ← `data/analysis/{v4,v5}-{offline-exp,offline-oldpatterns,closedloop-exp,closedloop-oldpatterns,dsb-hotel,stressng-le16}/`
- `analysis/eval_closed_loop.py` ← `data/analysis/{stressng-ab,stressng-ab-le16}/` and `data/analysis/{v4,v5}-{offline-exp,offline-oldpatterns,closedloop-exp,closedloop-oldpatterns,dsb-hotel,stressng-le16}/`
- `analysis/dsb_eval_ab.py` ← `data/analysis/dsb-ab-hotel/`, `data/analysis/dsb-ab-social/`
- `analysis/dsb_eval_cmp.py` ← `data/analysis/{dsb-hotel-cmp,dsb-social-cmp,v4-closedloop-exp,v4-closedloop-oldpatterns,v4-dsb-hotel,v4-offline-exp,v4-offline-oldpatterns,v4-stressng-le16,v5-closedloop-exp,v5-closedloop-oldpatterns,v5-dsb-hotel,v5-dsb-social,v5-offline-exp,v5-offline-oldpatterns,v5-stressng-le16}/`
- `analysis/dsb_diag.py` ← `data/analysis/dsb-diagnosis/`
- `analysis/pin_eval.py` ← `data/analysis/dsb-pin-check/`
- `train_slo.py`: now in `tools/learn/slo/` (moved there from `gnn-slo/`); originals in `data/gnn/slo-v1/` and `data/analysis/gnn-slo-v1/slo-scripts/`
- `gnn-slo/slo_eval.py`, `summary.py`, `plot_labels.py` ← `data/analysis/gnn-slo-v1/slo-scripts/`

The 52 files in the repository that import matplotlib reduce to 12 distinct contents. `extract_runs.py` is added because the paper figures depend on it.

## Dependencies not in this repository

Several scripts import helpers that are not in this checkout:

- `offline_eval.py`, `eval_closed_loop.py` import `train_gnn` and `mdata`.
- `train_slo.py` imports `train_gnn`; it lives in `tools/learn/`, so the README there covers it.
- `dsb_eval_*.py` read the run folders written by `src/test/dsb_ab.sh` and `src/test/dsb_sample.py`.

Copy those modules next to the scripts before running them. The scripts can run unchanged only from the directory where those helpers live.
