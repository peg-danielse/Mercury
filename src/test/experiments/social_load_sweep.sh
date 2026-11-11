#!/bin/bash
# Social load sweep until STOP_AT (default 18:00 today). The offered rate steps
# 20, 40, 60, 80, 100, 120, 150 req/s per route, 120 s each. Each repeat is three arms
# on that schedule: saturate on a fixed pool of 8, Luna (llm40-luna) from 8 with a
# budget of 40, then saturate on a fixed pool of 40. A repeat starts only when that
# run would finish by STOP_AT. The two fixed pools cannot share an output directory
# (both are named saturate); plots rename the 40-CPU one with saturate=fixed40.
# Antagonists stay on the usual schedule. Resumable: a run with DONE is skipped.
set -u
M=$(cd "$(dirname "$0")/../../.." && pwd)
export OPENROUTER_API_KEY=$(cat "$HOME/.config/mercury/openrouter.key")
OUT=$M/data/dsb/compare/social-load-sweep-20261029
STOP_AT=${STOP_AT:-"18:00"}
STOP=$(date -d "$STOP_AT" +%s)
# One run is the 880 s load (warmup 50 s + 16 phases of 50 s + 30 s) plus about 90 s
# of start, seed and teardown (compare_systems.sh).
DUR=$((50 + 16 * 50 + 30))
MAX=${MAX:-8}
GOAL="The offered load is 20, then 40, 60, 80, 100, 120, then 150 requests per second per route, each held for 120 seconds from the start of the run, and then the list repeats. Use scale_daemon so each service has the CPUs its cpu_use and pressure show it needs at this second: more when the rate has climbed or the service is waiting, fewer when the rate has fallen or the service is idle."
export LLM_ARGS="--every 0.5 --goal '$GOAL'"
COMMON="DB=seed SEED_N=1 PHASES=16 OFF=20 ON=30 STEP=120"
COMMON="$COMMON LEVELS=20,40,60,80,100,120,150"
COMMON="$COMMON LLM_KEY_VARS=OPENROUTER_API_KEY"
COMMON="$COMMON LLMCTL_PYTHON=$M/tools/llmpy-bin/llmpy"
COMMON="$COMMON LLM_MODELS=luna=openai/gpt-6-luna@openrouter:low"

enough() { [ $(( $(date +%s) + DUR + 90 )) -le "$STOP" ]; }

echo "$(date -Is) social load sweep: up to $MAX repeats, stop before $STOP_AT ($(date -d "@$STOP" -Is))"
for r in $(seq 1 "$MAX"); do
  echo "$(date -Is) repeat $r"
  if ! enough; then echo "$(date -Is) stop before fixed pool of 8, repeat $r"; break; fi
  env $COMMON STOP_AT="$STOP_AT" REPEATS=$r POOL_START=8 SYSTEMS="saturate" \
    "$M/src/test/compare_systems.sh" "$OUT" social

  if ! enough; then echo "$(date -Is) stop before Luna, repeat $r"; break; fi
  env $COMMON STOP_AT="$STOP_AT" REPEATS=$r POOL_START=8 SYSTEMS="llm40-luna" \
    "$M/src/test/compare_systems.sh" "$OUT" social

  if ! enough; then echo "$(date -Is) stop before fixed pool of 40, repeat $r"; break; fi
  env $COMMON STOP_AT="$STOP_AT" REPEATS=$r POOL_START=40 SYSTEMS="saturate" \
    "$M/src/test/compare_systems.sh" "$OUT-p40" social
done
echo "$(date -Is) social load sweep finished"
