#!/bin/bash
# Three repeats of the distilled transformer on the social 100/120/150 cycle.
# Same antagonist schedule as social_cycle_gemini.sh: seed 1, 12 phases, off 20s, on 30s,
# warmup 50s, pool start 8, budget 25. Resumable: a run with DONE is skipped.
set -u
M=$(cd "$(dirname "$0")/../../.." && pwd)
export OPENROUTER_API_KEY=$(cat "$HOME/.config/mercury/openrouter.key")
OUT=$M/data/dsb/compare/social-cycle-distill-20261029
COMMON="REPEATS=3 DB=seed SEED_N=1 PHASES=12 OFF=20 ON=30 LLM_KEY_VARS=OPENROUTER_API_KEY"
COMMON="$COMMON LLMCTL_PYTHON=$M/tools/llmpy-bin/llmpy"
COMMON="$COMMON LLM_MODELS=distill=$M/models/distill-v2-noshed.pt@distilled"

echo "$(date -Is) distilled transformer, 3 repeats, pool start 8"
env $COMMON POOL_START=8 SYSTEMS="llm25-distill" \
  "$M/src/test/compare_systems.sh" "$OUT" social
echo "$(date -Is) distill series finished"
