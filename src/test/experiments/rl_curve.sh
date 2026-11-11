#!/bin/bash
# Learning curve for gnn-rl, gnn-imitation-rl, tf-rl, and tf-imitation-rl.
# The plant is tools/learn/rl/curve.py: a hot process needs a scale, a cold
# process needs a hold, and the score is step_reward. It does not start Mercury.
# OUT receives curve.jsonl (reward and performance at each scored step) and summary.json.
set -euo pipefail
M=$(cd "$(dirname "$0")/../../.." && pwd)
PY=${LLMCTL_PYTHON:-/home/paul/opt/torchenv/bin/python}
OUT=${OUT:-$M/data/learn/rl-curve}
mkdir -p "$OUT"
exec "$PY" "$M/tools/learn/rl/curve.py" \
  --out "$OUT" \
  --steps "${STEPS:-200}" \
  --eval-every "${EVAL_EVERY:-25}" \
  --seed "${SEED:-0}" \
  ${SETTINGS:+--settings "$SETTINGS"}
