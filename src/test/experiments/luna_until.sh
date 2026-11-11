#!/bin/bash
# Luna only, on hotel, then social, then media, repeated until STOP_AT.
# Same load cycle and antagonists as night_eval.sh: 100, 120, 150 req/s per route,
# 12 phases, kinds same,smt,l3,membw,load. Each app runs llm40-luna then llm24-luna,
# the operator's autoscaler detached and the pool starting at 8, so Luna's
# scale_daemon sets the CPU count inside a budget of 40 and then of 24.
# A repeat whose directory already contains DONE is skipped.
# compare_systems.sh also starts no run that would still be going at STOP_AT.
set -u
M=$(cd "$(dirname "$0")/../../.." && pwd)
export OPENROUTER_API_KEY=$(cat "$HOME/.config/mercury/openrouter.key")
export KINDS=${KINDS:-same,smt,l3,membw,load}
STOP_AT=${STOP_AT:-2026-11-02 20:00}
MAX=${MAX:-40}
BASE=${BASE:-$M/data/dsb/compare}
GOAL="The offered load is 100, then 120, then 150 requests per second per route, each held for 100 seconds, and then the list repeats. Use scale_daemon so each service has the CPUs its cpu_use and pressure show it needs at this second: more when the rate has climbed or the service is waiting, fewer when the rate has fallen or the service is idle."
export LLM_ARGS="--every 0.5 --goal '$GOAL'"
COMMON="DB=seed SEED_N=1 PHASES=12 OFF=20 ON=30"
COMMON="$COMMON LEVELS=100,120,150"
COMMON="$COMMON LLM_KEY_VARS=OPENROUTER_API_KEY"
COMMON="$COMMON LLMCTL_PYTHON=$M/tools/llmpy-bin/llmpy"
COMMON="$COMMON LLM_MODELS=luna=openai/gpt-6-luna@openrouter:low"
HOTEL_ROUTE="ROUTE=hotels,recommendations,user,review,restaurants,museums,cinema,reservation"

SYSTEMS=${SYSTEMS:-llm40-luna llm24-luna}
echo "$(date -Is) luna cycle: hotel, social, media, systems [$SYSTEMS], up to $MAX rounds, stop before $STOP_AT, kinds $KINDS"
for r in $(seq 1 "$MAX"); do
  now=$(date +%s)
  stop=$(date -d "$STOP_AT" +%s)
  if [ "$now" -ge "$stop" ]; then
    echo "$(date -Is) STOP_AT $STOP_AT reached before round $r"
    break
  fi
  echo "$(date -Is) round $r"
  env $COMMON $HOTEL_ROUTE REPEATS=$r POOL_START=8 SYSTEMS="$SYSTEMS" SC_LLM_OWNS=1 STOP_AT="$STOP_AT" \
    "$M/src/test/compare_systems.sh" "$BASE/hotel-luna-20261102" hotel
  env $COMMON REPEATS=$r POOL_START=8 SYSTEMS="$SYSTEMS" SC_LLM_OWNS=1 STOP_AT="$STOP_AT" \
    "$M/src/test/compare_systems.sh" "$BASE/social-luna-20261102" social
  env $COMMON REPEATS=$r POOL_START=8 SYSTEMS="$SYSTEMS" SC_LLM_OWNS=1 STOP_AT="$STOP_AT" \
    "$M/src/test/compare_systems.sh" "$BASE/media-luna-20261102" media
done
echo "$(date -Is) luna cycle finished"
