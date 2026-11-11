#!/bin/bash
# Climb the offered load on hotel, then social, then media until 10% of requests
# fail, hold, and climb again only if that hold's error rate fell (-load seek).
# No antagonist. Each pass is llm40-luna: a fixed pool starting at 8 CPUs with a
# budget of 40, and the model (not saturate) sets each service's CPU count.
# Repeats until STOP_AT. A pass that would still be running at STOP_AT is not started.
# ONCE=1 stops after hotel, social and media have each run once.
set -u
M=$(cd "$(dirname "$0")/../../.." && pwd)
STOP_AT=${STOP_AT:-2026-11-03 08:00}
DUR=${DUR:-20m}
BASE=${BASE:-$M/data/dsb/compare/seek-night-20261102}
LOG=$BASE.log
mkdir -p "$BASE"
# 20 min of load plus the time to restore the seed and start the services.
BUDGET=${BUDGET:-2100}
SEEK="-arrivals poisson -load seek -seek-start 50 -step 60s -seek-wait 120s -seek-error 0.10 -seek-max 32768 -duration $DUR -window-ms 500 -hist -gen-min 8 -gen-max 16 -inflight 16384"
HOTEL_SEED=${HOTEL_SEED:-$(cat "$HOME/.seed_hotel")}
SOCIAL_SEED=${SOCIAL_SEED:-$(cat "$HOME/.seed_social")}
MEDIA_SEED=${MEDIA_SEED:-$(cat "$HOME/.seed_media")}
export OPENROUTER_API_KEY=$(cat "$HOME/.config/mercury/openrouter.key")
GOAL="The offered load starts at 50 requests per second per route and doubles every 60 seconds while fewer than 10 percent of requests fail. When the error rate reaches 10 percent, the load holds for 120 seconds. If the error rate then falls, it doubles again; otherwise it stays. Use scale_daemon so each service has the CPUs its cpu_use and pressure show it needs at this second: more when the rate has climbed or the service is waiting, fewer when the rate has fallen or the service is idle."

echo "$(date -Is) seek night: hotel, social, media, llm40-luna, duration $DUR, stop before $STOP_AT" | tee -a "$LOG"
round=0
while true; do
  now=$(date +%s)
  stop=$(date -d "$STOP_AT" +%s)
  if [ "$now" -ge "$stop" ]; then
    echo "$(date -Is) STOP_AT $STOP_AT reached" | tee -a "$LOG"
    break
  fi
  round=$((round + 1))
  echo "$(date -Is) round $round" | tee -a "$LOG"
  run() { # app seed routes
    now=$(date +%s)
    if [ $((now + BUDGET)) -gt "$stop" ]; then
      echo "$(date -Is) not starting $1: it would run past $STOP_AT" | tee -a "$LOG"
      return 1
    fi
    echo "$(date -Is) $1 llm40-luna" | tee -a "$LOG"
    case $1 in
      hotel) pfx=hotel-;;
      social) pfx=social-;;
      media) pfx=media-;;
    esac
    goal=$GOAL
    if [ "$1" = hotel ]; then
      goal="The hotels rate starts at 50 requests per second and doubles every 60 seconds while fewer than 10 percent of requests fail. Recommendations is 31/52 of that rate, review, restaurants, museums and cinema are 4/52, and user and reservation are 0.5/52. At 10 percent errors the rates hold for 120 seconds and double again only if the error rate fell. Use scale_daemon so each service has the CPUs its cpu_use and pressure show it needs at this second: more when the rate has climbed or the service is waiting, fewer when the rate has fallen or the service is idle."
    fi
    llm_args="--every 0.5 --model openai/gpt-6-luna --llm openrouter --thinking low --goal '$goal'"
    [ "$1" = media ] && llm_args="$llm_args --app media"
    if ! env POLICIES=saturate SEED="$2" CPUSTAT=1 SVC_ARGS="-svc-min 1 -svc-max 16" \
      POOL_POLICY="fixed,start=8,seed_cpu=2,exempt=${pfx}loadgen,max=40" \
      SC_SYSTEM=llm SC_LLM_OWNS=1 SC_REPLICAS=1 MERCURY_REUSEPORT=1 \
      SC_LLMCTL_PYTHON="$M/tools/llmpy-bin/llmpy" \
      SC_LLM_ARGS="$llm_args" \
      SIDECAR="python3 $M/src/test/system_sidecar.py" \
      "$M/src/test/dsb_ab.sh" "$1" "$BASE/$1" 1 "$SEEK -routes $3" >>"$LOG" 2>&1; then
      echo "$(date -Is) $1 failed" | tee -a "$LOG"
    fi
    return 0
  }
  run hotel "$HOTEL_SEED" hotels,recommendations,user,review,restaurants,museums,cinema,reservation || break
  run social "$SOCIAL_SEED" compose-post,read-home-timeline,read-user-timeline,follow || break
  run media "$MEDIA_SEED" read-page,compose-review,read-movie-reviews || break
  [ -n "${ONCE:-}" ] && break
done
echo "$(date -Is) seek night finished" | tee -a "$LOG"
