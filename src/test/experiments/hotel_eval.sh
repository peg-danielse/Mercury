#!/bin/bash
# Hotel comparison on the same cycle as social_eval.sh (100, 120, 150 req/s per route).
# Repeats run until STOP_AT: no run starts that would end after that.
# Each repeat:
#   saturate, fixed pool of 8, each daemon's max is 8
#   saturate, fixed pool of 40, each daemon's max is 40
#   shift, degrade, l3, l3+shift on saturate (max 16)
#   the same four on the GNN autoscaler (gru, no pinning)
#   Luna (llm40-luna, pool start 8, budget 40) with the operator's autoscaler detached,
#   so the model is what changes a service's CPU count
# Hotel has no rabbitmq, so the GNN arm does not pass -saturate-only.
# LEVELS and ROUTE are set here: only social defaults to 100,120,150, and hotel's
# default route is hotels alone. LEVELS is the hotels rate. The operator's -mix dsb
# gives the other routes a fixed share of it. The route list is the one the hotel
# comparisons drive, so degrade has the optional routes to shed.
# KINDS, when set in the environment, is passed through to compare_systems.sh.
# The two fixed pools cannot share an output directory (both are named saturate);
# the GNN arms cannot share one with the saturate arms (same system names).
# Resumable: a run with DONE is skipped. OUT can be overridden.
set -u
M=$(cd "$(dirname "$0")/../../.." && pwd)
export OPENROUTER_API_KEY=$(cat "$HOME/.config/mercury/openrouter.key")
OUT=${OUT:-$M/data/dsb/compare/hotel-eval-20261101}
GOAL="The hotels route is offered 100, then 120, then 150 requests per second, each held for 100 seconds, and then the list repeats. Recommendations is 31/52 of that rate, review, restaurants, museums and cinema are 4/52, and user and reservation are 0.5/52. Use scale_daemon so each service has the CPUs its cpu_use and pressure show it needs at this second: more when the rate has climbed or the service is waiting, fewer when the rate has fallen or the service is idle."
export LLM_ARGS="--every 0.5 --goal '$GOAL'"
COMMON="DB=seed SEED_N=1 PHASES=12 OFF=20 ON=30"
COMMON="$COMMON LEVELS=100,120,150"
COMMON="$COMMON ROUTE=hotels,recommendations,user,review,restaurants,museums,cinema,reservation"
COMMON="$COMMON LLM_KEY_VARS=OPENROUTER_API_KEY"
COMMON="$COMMON LLMCTL_PYTHON=$M/tools/llmpy-bin/llmpy"
COMMON="$COMMON LLM_MODELS=luna=openai/gpt-6-luna@openrouter:low"
OTHER="shift degrade l3 l3shift"
MAX=${MAX:-6}
STOP_AT=${STOP_AT:-2026-11-01 12:00}

echo "$(date -Is) hotel eval: up to $MAX repeats, stop before $STOP_AT, fixed budgets, saturate and gnn, luna owns scaling"
for r in $(seq "${FROM:-1}" "$MAX"); do
  echo "$(date -Is) repeat $r"
  env $COMMON REPEATS=$r POOL_START=8 SYSTEMS="saturate" SVC_ARGS="-svc-min 1 -svc-max 8" STOP_AT="$STOP_AT" \
    "$M/src/test/compare_systems.sh" "$OUT" hotel
  env $COMMON REPEATS=$r POOL_START=40 SYSTEMS="saturate" SVC_ARGS="-svc-min 1 -svc-max 40" STOP_AT="$STOP_AT" \
    "$M/src/test/compare_systems.sh" "$OUT-p40" hotel
  env $COMMON REPEATS=$r POOL_START=8 SYSTEMS="$OTHER" STOP_AT="$STOP_AT" \
    "$M/src/test/compare_systems.sh" "$OUT" hotel
  env $COMMON REPEATS=$r POOL_START=8 SYSTEMS="$OTHER" AUTOSCALER=gru \
    SVC_ARGS="-svc-min 1 -svc-max 16" STOP_AT="$STOP_AT" \
    "$M/src/test/compare_systems.sh" "$OUT-gnn" hotel
  env $COMMON REPEATS=$r POOL_START=8 SYSTEMS="llm40-luna" SC_LLM_OWNS=1 STOP_AT="$STOP_AT" \
    "$M/src/test/compare_systems.sh" "$OUT" hotel
done
echo "$(date -Is) hotel eval finished"
