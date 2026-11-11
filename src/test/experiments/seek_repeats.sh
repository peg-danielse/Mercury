#!/bin/bash
# Two more passes of the six-system seek (the 2026-11-04 sweep is the first),
# then Luna's seek three times. Same mixes and seek steps. No antagonist.
# A failed run is logged and the next one still starts.
set -u
M=$(cd "$(dirname "$0")/../../.." && pwd)
BASE=${BASE:-$M/data/dsb/compare}
LOG=${LOG:-$BASE/seek-repeats-20261105.log}
mkdir -p "$BASE"
HOTEL_SEED=${HOTEL_SEED:-$(cat "$HOME/.seed_hotel")}
SOCIAL_SEED=${SOCIAL_SEED:-$(cat "$HOME/.seed_social")}
MEDIA_SEED=${MEDIA_SEED:-$(cat "$HOME/.seed_media")}
export OPENROUTER_API_KEY=$(cat "$HOME/.config/mercury/openrouter.key")
SEEK="-arrivals poisson -load seek -seek-start 50 -step 60s -seek-wait 120s -seek-error 0.10 -seek-max 65536 -duration 0 -window-ms 500 -hist -gen-min 8 -gen-max 16 -inflight 65536"

echo "$(date -Is) seek repeats: model r2 r3, then luna r1 r2 r3" | tee -a "$LOG"

settle() {
  sleep 2
  if ss -ltnH | grep -q ':8080 '; then
    echo "$(date -Is) stopping leftover mercury" | tee -a "$LOG"
    sudo -n pkill -f "$M/build/mercury " || true
    sleep 2
  fi
  if pgrep -x mongod >/dev/null; then
    echo "$(date -Is) stopping leftover mongod" | tee -a "$LOG"
    sudo -n pkill -x mongod || true
    sleep 2
  fi
}

for rep in r2 r3; do
  echo "$(date -Is) model seek $rep" | tee -a "$LOG"
  env TAG=model-seek-20261105-$rep LOG="$LOG" "$M/src/test/experiments/model_seek.sh" || \
    echo "$(date -Is) model seek $rep failed" | tee -a "$LOG"
  settle
done

hotel_routes=hotels,recommendations,user,review,restaurants,museums,cinema,reservation
social_routes=compose-post,read-home-timeline,read-user-timeline,follow
media_routes=read-page,compose-review,read-movie-reviews
hotel_goal="The hotels rate starts at 50 requests per second and doubles every 60 seconds while fewer than 10 percent of requests fail. Recommendations is 31/52 of that rate, review, restaurants, museums and cinema are 4/52, and user and reservation are 0.5/52. At 10 percent errors the rates hold for 120 seconds and double again only if the error rate fell. Use scale_daemon so each service has the CPUs its cpu_use and pressure show it needs at this second: more when the rate has climbed or the service is waiting, fewer when the rate has fallen or the service is idle."
social_goal="The read-home-timeline rate starts at 50 requests per second and doubles every 60 seconds while fewer than 10 percent of requests fail. Read-user-timeline is 30/60 of that rate, compose-post is 10/60, and follow is idle. At 10 percent errors the rates hold for 120 seconds and double again only if the error rate fell. Use scale_daemon so each service has the CPUs its cpu_use and pressure show it needs at this second: more when the rate has climbed or the service is waiting, fewer when the rate has fallen or the service is idle."
media_goal="The compose-review rate starts at 50 requests per second and doubles every 60 seconds while fewer than 10 percent of requests fail. Read-page and read-movie-reviews are 4/92 of that rate. At 10 percent errors the rates hold for 120 seconds and double again only if the error rate fell. Use scale_daemon so each service has the CPUs its cpu_use and pressure show it needs at this second: more when the rate has climbed or the service is waiting, fewer when the rate has fallen or the service is idle."

luna() { # rep app seed routes goal
  local rep=$1 app=$2 seed=$3 routes=$4 pfx out
  case $app in
    hotel) pfx=hotel-;;
    social) pfx=social-;;
    media) pfx=media-;;
  esac
  out=$BASE/$app-luna-seek-20261105/$rep
  local llm_args="--every 0.5 --model openai/gpt-6-luna --llm openrouter --thinking low --goal '$5'"
  [ "$app" = media ] && llm_args="$llm_args --app media"
  echo "$(date -Is) $app luna $rep" | tee -a "$LOG"
  if ! env POLICIES=saturate SEED="$seed" CPUSTAT=1 SVC_ARGS="-svc-min 1 -svc-max 16" \
      POOL_POLICY="fixed,start=8,seed_cpu=2,exempt=${pfx}loadgen,max=40" \
      SC_SYSTEM=llm SC_LLM_OWNS=1 SC_REPLICAS=1 MERCURY_REUSEPORT=1 \
      SC_LLMCTL_PYTHON="$M/tools/llmpy-bin/llmpy" \
      SC_LLM_ARGS="$llm_args" \
      SIDECAR="python3 $M/src/test/system_sidecar.py" \
      "$M/src/test/dsb_ab.sh" "$app" "$out" 1 "$SEEK -routes $routes" >>"$LOG" 2>&1; then
    echo "$(date -Is) $app luna $rep failed" | tee -a "$LOG"
    settle
    return 1
  fi
  settle
}

for rep in r1 r2 r3; do
  luna "$rep" hotel "$HOTEL_SEED" "$hotel_routes" "$hotel_goal" || true
  luna "$rep" social "$SOCIAL_SEED" "$social_routes" "$social_goal" || true
  luna "$rep" media "$MEDIA_SEED" "$media_routes" "$media_goal" || true
done
echo "$(date -Is) seek repeats finished" | tee -a "$LOG"
