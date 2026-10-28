#!/bin/bash
# Social load cycle (100/120/150 req/s per route), repeated until 08:00 local tomorrow.
# Same order as social_cycle.sh, with the LLM replaced by Gemini 3.8 Flash (llm25, thinking low)
# through OpenRouter. The two fixed-pool runs cannot share an output directory (both are named saturate).
# Resumable: a run with DONE is skipped, and nothing starts that would finish after STOP_AT.
set -u
M=$(cd "$(dirname "$0")/../../.." && pwd)
export OPENROUTER_API_KEY=$(cat "$HOME/.config/mercury/openrouter.key")
OUT=$M/data/dsb/compare/social-cycle-gemini-20261028
STOP_AT=${STOP_AT:-"08:00 tomorrow"}
STOP=$(date -d "$STOP_AT" +%s)
# One run is the 680 s load plus about 90 s of start, seed and teardown (compare_systems.sh).
DUR=$((50 + 12 * 50 + 30))
MAX=${MAX:-16}
COMMON="DB=seed SEED_N=1 PHASES=12 OFF=20 ON=30 LLM_KEY_VARS=OPENROUTER_API_KEY"
COMMON="$COMMON LLM_MODELS=gemini=google/gemini-3.8-flash@openrouter:low"

enough() { [ $(( $(date +%s) + DUR + 90 )) -le "$STOP" ]; }

echo "$(date -Is) gemini cycle: up to $MAX repeats, stop before $STOP_AT ($(date -d "@$STOP" -Is))"
for r in $(seq 1 "$MAX"); do
  echo "$(date -Is) repeat $r"
  if ! enough; then echo "$(date -Is) stop before fixed pool of 8, repeat $r"; break; fi
  env $COMMON STOP_AT="$STOP_AT" REPEATS=$r POOL_START=8 SYSTEMS="saturate" \
    "$M/src/test/compare_systems.sh" "$OUT" social

  if ! enough; then echo "$(date -Is) stop before fixed pool of 60, repeat $r"; break; fi
  env $COMMON STOP_AT="$STOP_AT" REPEATS=$r POOL_START=60 SYSTEMS="saturate" \
    "$M/src/test/compare_systems.sh" "$OUT-p60" social

  if ! enough; then echo "$(date -Is) stop before l3 / gemini, repeat $r"; break; fi
  env $COMMON STOP_AT="$STOP_AT" REPEATS=$r POOL_START=8 SYSTEMS="l3 l3shift llm25-gemini" \
    "$M/src/test/compare_systems.sh" "$OUT" social
done
echo "$(date -Is) gemini cycle series stopped"
