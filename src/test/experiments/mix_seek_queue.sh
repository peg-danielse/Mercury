#!/bin/bash
# After the hotel mix seek (HOTEL_PID) exits and releases port 8080, seek the
# highest load social and then media can hold under their default mixes.
# llm40-luna sets the CPU counts. No antagonist. -duration 0 stops once the
# level is no longer raised.
set -u
M=$(cd "$(dirname "$0")/../../.." && pwd)
HOTEL_PID=${HOTEL_PID:-317547}
BASE=${BASE:-$M/data/dsb/compare}
LOG=${LOG:-$BASE/mix-seek-queue-20261103.log}
mkdir -p "$BASE"
SOCIAL_SEED=${SOCIAL_SEED:-$(cat "$HOME/.seed_social")}
MEDIA_SEED=${MEDIA_SEED:-$(cat "$HOME/.seed_media")}
export OPENROUTER_API_KEY=$(cat "$HOME/.config/mercury/openrouter.key")
SEEK="-arrivals poisson -load seek -seek-start 50 -step 60s -seek-wait 120s -seek-error 0.10 -seek-max 65536 -duration 0 -window-ms 500 -hist -gen-min 8 -gen-max 16 -inflight 65536"

echo "$(date -Is) queued: social then media mix seek after hotel pid $HOTEL_PID" | tee -a "$LOG"
while kill -0 "$HOTEL_PID" 2>/dev/null; do
  sleep 30
done
echo "$(date -Is) hotel pid $HOTEL_PID has exited" | tee -a "$LOG"
for i in $(seq 60); do
  if ! ss -ltnH | grep -q ':8080 ' && ! pgrep -x mongod >/dev/null; then
    break
  fi
  sleep 2
done
if ss -ltnH | grep -q ':8080 ' || pgrep -x mongod >/dev/null; then
  echo "$(date -Is) port 8080 or mongod still busy; not starting" | tee -a "$LOG"
  exit 1
fi

run() { # app seed routes goal
  echo "$(date -Is) $1 llm40-luna" | tee -a "$LOG"
  case $1 in
    social) pfx=social-;;
    media) pfx=media-;;
  esac
  llm_args="--every 0.5 --model openai/gpt-6-luna --llm openrouter --thinking low --goal '$4'"
  [ "$1" = media ] && llm_args="$llm_args --app media"
  if ! env POLICIES=saturate SEED="$2" CPUSTAT=1 SVC_ARGS="-svc-min 1 -svc-max 16" \
    POOL_POLICY="fixed,start=8,seed_cpu=2,exempt=${pfx}loadgen,max=40" \
    SC_SYSTEM=llm SC_LLM_OWNS=1 SC_REPLICAS=1 MERCURY_REUSEPORT=1 \
    SC_LLMCTL_PYTHON="$M/tools/llmpy-bin/llmpy" \
    SC_LLM_ARGS="$llm_args" \
    SIDECAR="python3 $M/src/test/system_sidecar.py" \
    "$M/src/test/dsb_ab.sh" "$1" "$BASE/$1-mix-seek-20261103" 1 "$SEEK -routes $3" >>"$LOG" 2>&1; then
    echo "$(date -Is) $1 failed" | tee -a "$LOG"
    return 1
  fi
}

social_goal="The read-home-timeline rate starts at 50 requests per second and doubles every 60 seconds while fewer than 10 percent of requests fail. Read-user-timeline is 30/60 of that rate, compose-post is 10/60, and follow is idle. At 10 percent errors the rates hold for 120 seconds and double again only if the error rate fell. Use scale_daemon so each service has the CPUs its cpu_use and pressure show it needs at this second: more when the rate has climbed or the service is waiting, fewer when the rate has fallen or the service is idle."
media_goal="The compose-review rate starts at 50 requests per second and doubles every 60 seconds while fewer than 10 percent of requests fail. Read-page and read-movie-reviews are 4/92 of that rate. At 10 percent errors the rates hold for 120 seconds and double again only if the error rate fell. Use scale_daemon so each service has the CPUs its cpu_use and pressure show it needs at this second: more when the rate has climbed or the service is waiting, fewer when the rate has fallen or the service is idle."

run social "$SOCIAL_SEED" compose-post,read-home-timeline,read-user-timeline,follow "$social_goal" || exit 1
run media "$MEDIA_SEED" read-page,compose-review,read-movie-reviews "$media_goal" || exit 1
echo "$(date -Is) social and media mix seeks finished" | tee -a "$LOG"
