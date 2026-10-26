# Mercury learning code

Training scripts for the GNN policy, the sequence models (MLP, GRU, memory transformer) and the distilled transformer.
Copies are from `gnn-example` at its HEAD (`mercury/`), and `distill/` was moved here from `tools/distill/`.

## Layout and imports

The layout matters: `slo/` scripts find `train_gnn` and `mdata` by going one directory up (`sys.path` insert of `..`).
Keep this tree intact when running from a copy.

```
train_gnn.py          GNN policy: per-thread demand regression by imitation of ground truth
mdata.py              dataset loader for train_gnn (threadtrace trace.jsonl + gnnload labels.csv)
slo/train_slo.py      GNN for the SLO allocation target (fine-tunes v5 on DSB traces); imports train_gnn
slo/seq_models.py     MLP, GRU and memory transformer over thread tokens (daemon-level P(t))
slo/train_seq.py      trains and compares seq_models against the GNN on the same cases; imports train_gnn, mdata
distill/train.py      small transformer distilled from the llm controller's decisions (needs torch)
distill/reconstruct.py  builds episodes.jsonl for distill/train.py from recorded llmctl runs
```

## Usage (from each script's docstring)

- GNN: `python train_gnn.py <run_dir> [--every N] [--epochs E] [--out results/<name>]`
- SLO GNN: `python slo/train_slo.py <out_dir> --init <weights.json> --lambdas 0.03,0.1,0.3,1 --epochs E case=<trace.jsonl>@<windows.json>@<daemon>[@test] ...`
- Sequence models: `python slo/train_seq.py <out_dir> --model mlp|gru|tf [--gnn <weights.json> ...] [options] case=... | --cases FILE`
- Distilled transformer: `python distill/train.py episodes.jsonl OUT [--d 64] [--layers 3] [--steps 4000] [--test-frac 0.15] [--seed 0]`

## Dependencies

- `numpy`, and `torch` for everything except `mdata.py`. torch is not installed on this workstation. `distill/train.py` says it runs on pd-gpu; the GNN scripts reference dtn-01's distances, so they were most likely run there.
- The labelled data (threadtrace runs, gnnload cases, DSB traces) is not in this repository. The paths and run folders are in each script's docstring and in `data/gnn/` and `data/analysis/`.

## Versions

- `train_gnn.py`, `mdata.py`, `slo/train_slo.py`, `slo/seq_models.py`: match gnn-example HEAD. `slo/train_seq.py` at HEAD (`92b7d03`) is the committed version.
- `slo/train_seq.py` on dtn-01 (`~/gnn-example`) is a different, uncommitted file (md5 `cfe02d1c`, last changed 2026-10-20). That version is not here. If the results you need came from it, copy it from dtn-01 and commit it first.
- `distill/` was moved from `tools/distill/` (`git mv`). `tools/llmctl.py` now refers to `tools/learn/distill/train.py`.

## Not included

I did not find a standalone GRU policy trainer; the GRU appears only as a model in `seq_models.py`. The dataset-collection scripts stay in `data/gnn/v4` and `data/gnn/v5`, and the old `gnn-example/mercury/results/v4/train_gnn.py` was not copied.
