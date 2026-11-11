#!/bin/bash
# Run an action checkpoint on Mercury, score each of its decisions by the
# reward change that followed (against decisions in the same load phase),
# clone the better ones, avoid the worse and refused ones, and train from the
# best round's weights on half teacher rows, half student rows.
# The student runs at the teacher's cadence: 0.5 s checks, a 1000 ms window,
# and the trouble gate. The loop stops when p95 and the error rate are both
# within the bar, or after ROUNDS. Each round's record goes to cycle.jsonl.
#
# INIT is an action checkpoint (kind "action"). TEACHER is the Luna campaign
# directory; repeat 1 is the anchor. One round is compare_systems.sh with
# SYSTEMS=llm40-cycle. INTERFERE defaults to 0 and DB to seed. KEEP is the
# number of recent rounds trained on.
#
#   INIT=data/learn/luna-action-context-20261108/tf-action.pt \
#     APP=hotel LEVELS=1500,3100,5300 ROUNDS=8 \
#     src/test/experiments/action_cycle.sh
#
# The from-zero baseline: random weights, no teacher rows in any update.
#
#   INIT=scratch-tf TEACHER_SHARE=0 APP=hotel LEVELS=1500,3100,5300 ROUNDS=8 \
#     src/test/experiments/action_cycle.sh
set -euo pipefail
M=$(cd "$(dirname "$0")/../../.." && pwd)
PY=${LLMCTL_PYTHON:-/home/paul/opt/torchenv/bin/python}
INIT=${INIT:?set INIT to an action checkpoint}
OUT=${OUT:-$M/data/learn/action-cycle}
TEACHER=${TEACHER:-$M/data/dsb/compare/luna-rl-20261105}
mkdir -p "$OUT"
exec "$PY" "$M/tools/learn/rl/cycle.py" \
  --init "$INIT" \
  --teacher "$TEACHER" \
  --teacher-share "${TEACHER_SHARE:-0.5}" \
  --out "$OUT" \
  --app "${APP:-hotel}" \
  --rounds "${ROUNDS:-8}" \
  --steps "${STEPS:-200}" \
  --keep "${KEEP:-3}" \
  --max-p95-ms "${MAX_P95:-100}" \
  --max-error-pct "${MAX_ERR:-1}" \
  --seed "${SEED:-0}"
