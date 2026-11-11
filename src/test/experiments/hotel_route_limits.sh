#!/bin/bash
# Seek each hotel frontend route alone until 10% of its requests fail, then offer
# every route, at the same time, the highest level that stayed under that line.
# llm40-luna sets the CPU counts. No antagonist.
set -u
M=$(cd "$(dirname "$0")/../../.." && pwd)
BASE=${BASE:-$M/data/dsb/compare/hotel-routes-20261103}
LOG=$BASE.log
mkdir -p "$BASE"
HOTEL_SEED=${HOTEL_SEED:-$(cat "$HOME/.seed_hotel")}
export OPENROUTER_API_KEY=$(cat "$HOME/.config/mercury/openrouter.key")
ROUTES="hotels recommendations user review restaurants museums cinema reservation"
SEEK="-arrivals poisson -load seek -seek-start 50 -step 60s -seek-wait 120s -seek-error 0.10 -seek-max 65536 -duration 0 -window-ms 500 -hist -gen-min 8 -gen-max 16 -inflight 65536"
HOLD=${HOLD:-5m}

llm_run() { # goal extra-args
  local goal="$1" extra="$2"
  env POLICIES=saturate SEED="$HOTEL_SEED" CPUSTAT=1 SVC_ARGS="-svc-min 1 -svc-max 16" \
    POOL_POLICY="fixed,start=8,seed_cpu=2,exempt=hotel-loadgen,max=40" \
    SC_SYSTEM=llm SC_LLM_OWNS=1 SC_REPLICAS=1 MERCURY_REUSEPORT=1 \
    SC_LLMCTL_PYTHON="$M/tools/llmpy-bin/llmpy" \
    SC_LLM_ARGS="--every 0.5 --model openai/gpt-6-luna --llm openrouter --thinking low --goal '$goal'" \
    SIDECAR="python3 $M/src/test/system_sidecar.py" \
    "$M/src/test/dsb_ab.sh" hotel "$3" 1 "$extra"
}

echo "$(date -Is) hotel route limits: one route at a time, then all at those rates, llm40-luna" | tee -a "$LOG"
: > "$BASE/limits.txt"
fail=0
rates=""
for route in $ROUTES; do
  echo "$(date -Is) seek $route" | tee -a "$LOG"
  goal="Only the $route route is loaded. Its offered rate starts at 50 requests per second and doubles every 60 seconds while fewer than 10 percent of requests fail. At 10 percent it holds for 120 seconds and doubles again only if the error rate fell. Use scale_daemon so each service has the CPUs its cpu_use and pressure show it needs at this second: more when the rate has climbed or the service is waiting, fewer when the rate has fallen or the service is idle."
  mark=$(wc -l < "$LOG")
  if ! llm_run "$goal" "$SEEK -routes $route" "$BASE/$route" >>"$LOG" 2>&1; then
    echo "$(date -Is) $route failed" | tee -a "$LOG"
    fail=1
    continue
  fi
  if ! tail -n +"$mark" "$LOG" | grep -q "operator exit 0"; then
    echo "$(date -Is) $route operator failed" | tee -a "$LOG"
    fail=1
    continue
  fi
  run=$(grep '^done: ' "$LOG" | tail -1 | sed 's/^done: //')
  limit=$(python3 - "$run/1-saturate/operate.out" << 'PY'
import re, sys
best = None
for line in open(sys.argv[1]):
    m = re.search(r"offered=(\d+) err=([0-9.]+)", line)
    if m and float(m.group(2)) < 0.10:
        best = int(m.group(1))
print("" if best is None else best)
PY
)
  if [ -z "$limit" ]; then
    echo "$(date -Is) $route: no level stayed under 10%" | tee -a "$LOG"
    fail=1
    continue
  fi
  echo "$(date -Is) $route limit $limit req/s" | tee -a "$LOG"
  echo "$route=$limit" >> "$BASE/limits.txt"
  rates="${rates:+$rates,}$route=$limit"
done

if [ "$fail" != 0 ] || [ -z "$rates" ]; then
  echo "$(date -Is) not running the combined load: a route has no limit" | tee -a "$LOG"
  exit 1
fi
echo "$(date -Is) together $rates" | tee -a "$LOG"
goal="Every route is offered a constant rate, the highest that route served alone with fewer than 10 percent of requests failing: $rates. These rates are offered at the same time for five minutes. Use scale_daemon so each service has the CPUs its cpu_use and pressure show it needs at this second: more when a service is waiting, fewer when it is idle."
joined=${ROUTES// /,}
if ! llm_run "$goal" "-arrivals poisson -load list -levels $rates -step $HOLD -duration $HOLD -window-ms 500 -hist -gen-min 8 -gen-max 16 -inflight 65536 -routes $joined" "$BASE/together" >>"$LOG" 2>&1; then
  echo "$(date -Is) together failed" | tee -a "$LOG"
  exit 1
fi
echo "$(date -Is) hotel route limits finished" | tee -a "$LOG"
