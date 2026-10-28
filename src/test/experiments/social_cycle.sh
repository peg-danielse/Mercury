#!/bin/bash
# Social, one repeat, the 100/120/150 req/s cycle (compare_systems.sh default when RATE is unset).
# Order: fixed pool of 8, fixed pool of 60, then l3, l3+shift, gpt-luna and the distilled transformer
# on the 8-CPU pool. The two fixed-pool runs cannot share an output directory (both are named saturate).
# Resumable: a run with DONE is skipped.
set -u
M=$(cd "$(dirname "$0")/../../.." && pwd)
export OPENROUTER_API_KEY=$(cat "$HOME/.config/mercury/openrouter.key")
OUT=$M/data/dsb/compare/social-cycle-20261028
COMMON="REPEATS=1 DB=seed SEED_N=1 PHASES=12 OFF=20 ON=30 LLM_KEY_VARS=OPENROUTER_API_KEY"
COMMON="$COMMON LLMCTL_PYTHON=$M/tools/llmpy-bin/llmpy"
COMMON="$COMMON LLM_MODELS=luna=openai/gpt-6-luna@openrouter:low,distill=$M/models/distill-v2-noshed.pt@distilled"

echo "$(date -Is) fixed pool of 8"
env $COMMON POOL_START=8 SYSTEMS="saturate" \
  "$M/src/test/compare_systems.sh" "$OUT" social

echo "$(date -Is) fixed pool of 60"
env $COMMON POOL_START=60 SYSTEMS="saturate" \
  "$M/src/test/compare_systems.sh" "$OUT-p60" social

echo "$(date -Is) l3, l3+shift, gpt-luna, distilled"
env $COMMON POOL_START=8 SYSTEMS="l3 l3shift llm25-luna llm25-distill" \
  "$M/src/test/compare_systems.sh" "$OUT" social

echo "$(date -Is) social cycle series finished"
