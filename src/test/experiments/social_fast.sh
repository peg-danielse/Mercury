#!/bin/bash
# Social load sweep on the fast cpuset path (cpuset.mems grows and is not shrunk).
# One saturate pool of 8, one saturate pool of 40, three Luna, three Gemini 3.8.
# The offered rate steps 20, 40, 60, 80, 100, 120, 150 req/s per route, 120 s each.
# The two fixed pools cannot share an output directory (both are named saturate);
# plots rename the 40-CPU one with saturate=fixed40. Resumable: a run with DONE is skipped.
set -u
M=$(cd "$(dirname "$0")/../../.." && pwd)
export OPENROUTER_API_KEY=$(cat "$HOME/.config/mercury/openrouter.key")
OUT=$M/data/dsb/compare/social-fast-20261029
GOAL="The offered load is 20, then 40, 60, 80, 100, 120, then 150 requests per second per route, each held for 120 seconds from the start of the run, and then the list repeats. Use scale_daemon so each service has the CPUs its cpu_use and pressure show it needs at this second: more when the rate has climbed or the service is waiting, fewer when the rate has fallen or the service is idle."
export LLM_ARGS="--every 0.5 --goal '$GOAL'"
COMMON="DB=seed SEED_N=1 PHASES=16 OFF=20 ON=30 STEP=120"
COMMON="$COMMON LEVELS=20,40,60,80,100,120,150"
COMMON="$COMMON LLM_KEY_VARS=OPENROUTER_API_KEY"
COMMON="$COMMON LLMCTL_PYTHON=$M/tools/llmpy-bin/llmpy"
COMMON="$COMMON LLM_MODELS=luna=openai/gpt-6-luna@openrouter:low,gemini38=google/gemini-3.8-flash@openrouter:low"

echo "$(date -Is) social fast: fixed 8, fixed 40, luna x3, gemini 3.8 x3"
env $COMMON REPEATS=1 POOL_START=8 SYSTEMS="saturate" \
  "$M/src/test/compare_systems.sh" "$OUT" social
env $COMMON REPEATS=1 POOL_START=40 SYSTEMS="saturate" \
  "$M/src/test/compare_systems.sh" "$OUT-p40" social
env $COMMON REPEATS=3 POOL_START=8 SYSTEMS="llm40-luna" \
  "$M/src/test/compare_systems.sh" "$OUT" social
env $COMMON REPEATS=3 POOL_START=8 SYSTEMS="llm40-gemini38" \
  "$M/src/test/compare_systems.sh" "$OUT" social
echo "$(date -Is) social fast finished"
