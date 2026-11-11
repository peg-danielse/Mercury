#!/bin/bash
# Social comparison on the default cycle (100, 120, 150 req/s per route).
# Repeats run until STOP_AT (08:00 on 2026-10-30): no run starts that would end after that,
# so the set finishes tomorrow morning instead of around 03:00.
# Each repeat:
#   saturate, fixed pool of 8, each daemon's max is 8
#   saturate, fixed pool of 40, each daemon's max is 40
#   shift, degrade, l3, l3+shift on saturate (max 16)
#   the same four on the GNN autoscaler (gru, no pinning; rabbitmq stays on saturate)
#   Luna (llm40-luna, pool start 8, budget 40) with the operator's autoscaler detached,
#   so the model is what changes a service's CPU count
# The two fixed pools cannot share an output directory (both are named saturate);
# the GNN arms cannot share one with the saturate arms (same system names).
# Plots rename the 40-CPU pool with saturate=fixed40.
# Resumable: a run with DONE is skipped.
set -u
M=$(cd "$(dirname "$0")/../../.." && pwd)
export OPENROUTER_API_KEY=$(cat "$HOME/.config/mercury/openrouter.key")
OUT=${OUT:-$M/data/dsb/compare/social-eval-20261029}
GOAL="The offered load is 100, then 120, then 150 requests per second per route, each held for 100 seconds, and then the list repeats. Use scale_daemon so each service has the CPUs its cpu_use and pressure show it needs at this second: more when the rate has climbed or the service is waiting, fewer when the rate has fallen or the service is idle."
export LLM_ARGS="--every 0.5 --goal '$GOAL'"
COMMON="DB=seed SEED_N=1 PHASES=12 OFF=20 ON=30"
COMMON="$COMMON LLM_KEY_VARS=OPENROUTER_API_KEY"
COMMON="$COMMON LLMCTL_PYTHON=$M/tools/llmpy-bin/llmpy"
COMMON="$COMMON LLM_MODELS=luna=openai/gpt-6-luna@openrouter:low"
OTHER="shift degrade l3 l3shift"
MAX=${MAX:-6}
STOP_AT=${STOP_AT:-2026-10-30 08:00}

echo "$(date -Is) social eval: up to $MAX repeats, stop before $STOP_AT, fixed budgets, saturate and gnn, luna owns scaling"
for r in $(seq "${FROM:-1}" "$MAX"); do
  echo "$(date -Is) repeat $r"
  env $COMMON REPEATS=$r POOL_START=8 SYSTEMS="saturate" SVC_ARGS="-svc-min 1 -svc-max 8" STOP_AT="$STOP_AT" \
    "$M/src/test/compare_systems.sh" "$OUT" social
  env $COMMON REPEATS=$r POOL_START=40 SYSTEMS="saturate" SVC_ARGS="-svc-min 1 -svc-max 40" STOP_AT="$STOP_AT" \
    "$M/src/test/compare_systems.sh" "$OUT-p40" social
  env $COMMON REPEATS=$r POOL_START=8 SYSTEMS="$OTHER" STOP_AT="$STOP_AT" \
    "$M/src/test/compare_systems.sh" "$OUT" social
  env $COMMON REPEATS=$r POOL_START=8 SYSTEMS="$OTHER" AUTOSCALER=gru \
    SVC_ARGS="-svc-min 1 -svc-max 16 -saturate-only rabbitmq-server" STOP_AT="$STOP_AT" \
    "$M/src/test/compare_systems.sh" "$OUT-gnn" social
  env $COMMON REPEATS=$r POOL_START=8 SYSTEMS="llm40-luna" SC_LLM_OWNS=1 STOP_AT="$STOP_AT" \
    "$M/src/test/compare_systems.sh" "$OUT" social
done
echo "$(date -Is) social eval finished"
