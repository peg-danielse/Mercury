#!/bin/bash
# Train the MLP, the GRU and the memory transformer on one build.py dataset.
# Run this on a machine with CUDA. The dataset is built on the machine that holds
# the evaluation runs (build.py, numpy only) and copied across with the code.
set -euo pipefail
M=$(cd "$(dirname "$0")/../../.." && pwd)
PY=${PYTHON:-python3}
DATA=${DATA:-$M/data/learn/eval-20261101}
OUT=${OUT:-$M/results/eval-20261101}
STEPS=${STEPS:-2000}
BATCH=${BATCH:-32}
EVERY=${EVERY:-500}

if [[ ! -f "$DATA/manifest.json" ]]; then
  echo "missing $DATA/manifest.json" >&2
  echo "on the machine with the eval runs: python3 $M/tools/learn/evalset/build.py $DATA" >&2
  exit 1
fi
"$PY" - <<'PY'
import sys
import torch
if not torch.cuda.is_available():
    sys.exit("no CUDA device is visible; this script trains on a GPU")
print("gpu", torch.cuda.get_device_name(0), flush=True)
PY

COMMON=(--steps "$STEPS" --batch "$BATCH" --lr 3e-4 --lam 0.03 --lchg 0.1 --seed 0 --eval-every "$EVERY" --device cuda)
"$PY" "$M/tools/learn/evalset/train.py" "$DATA" "$OUT/mlp" --model mlp --d 64 "${COMMON[@]}"
"$PY" "$M/tools/learn/evalset/train.py" "$DATA" "$OUT/gru" --model gru --d 64 --layers 2 "${COMMON[@]}"
"$PY" "$M/tools/learn/evalset/train.py" "$DATA" "$OUT/tf" --model tf --ctx --d 64 --layers 2 --heads 4 \
  --fine 256 --mem 20 --bucket 20 --drop 0.1 "${COMMON[@]}"
"$PY" "$M/tools/learn/evalset/train.py" --summary "$OUT"
