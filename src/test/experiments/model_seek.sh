#!/bin/bash
# Throughput seek of the served models against the original Mercury settings.
# Hotel, then social, then media. No antagonist. Each application runs:
#   mlp, gru, tf   checkpoints on pd-gpu (llmctl --llm remote); MODELS is the
#                  server name, luna-20261102 or luna-seek-20261105
#   l3shift        saturate + shift + l3, the original comparison settings
#   fixed8         saturate, fixed pool of 8, each daemon's max is 8
#   fixed64        saturate, fixed pool asked for 64 (the allocatable set is
#                  CPUs 1-60 on dtn-01; pool_start stops there)
#   luna           llm40-luna: pool start 8, budget 40, OpenRouter gpt-6-luna,
#                  with add_replica and remove_replica (MERCURY_REUSEPORT=1)
# -duration 0 stops once the seek no longer raises the level.
set -u
M=$(cd "$(dirname "$0")/../../.." && pwd)
BASE=${BASE:-$M/data/dsb/compare}
# TAG is the directory stem: data/dsb/compare/<app>-<TAG>/<system>. The first
# sweep is model-seek-20261104. Later repeats pass their own tag.
TAG=${TAG:-model-seek-20261104}
LOG=${LOG:-$BASE/$TAG.log}
URL=${EVALSET_URL:-http://10.30.0.1:8790}
MODELS=${MODELS:-luna-20261102}
SYSTEMS=${SYSTEMS:-mlp gru tf l3shift fixed8 fixed64}
mkdir -p "$BASE"
HOTEL_SEED=${HOTEL_SEED:-$(cat "$HOME/.seed_hotel")}
SOCIAL_SEED=${SOCIAL_SEED:-$(cat "$HOME/.seed_social")}
MEDIA_SEED=${MEDIA_SEED:-$(cat "$HOME/.seed_media")}
case " $SYSTEMS " in
  *" luna "*) export OPENROUTER_API_KEY=$(cat "$HOME/.config/mercury/openrouter.key");;
esac
SEEK="-arrivals poisson -load seek -seek-start 50 -step 60s -seek-wait 120s -seek-error 0.10 -seek-max 65536 -duration 0 -window-ms 500 -hist -gen-min 8 -gen-max 16 -inflight 65536"
# SEEK_INC climbs by that many requests per second instead of doubling, on hotel, social, and media.
# A step at or over 10% holds for -seek-wait. Still over: stop. Back under: climb by SEEK_INC again.
# SEEK_START replaces -seek-start when SEEK_INC is set.
if [ -n "${SEEK_INC:-}" ]; then
  SEEK="$SEEK -seek-inc $SEEK_INC"
  if [ -n "${SEEK_START:-}" ]; then
    SEEK="$SEEK -seek-start $SEEK_START"
  fi
fi

echo "$(date -Is) model seek: $SYSTEMS" | tee -a "$LOG"
if ss -ltnH | grep -q ':8080 ' || pgrep -x mongod >/dev/null; then
  echo "$(date -Is) port 8080 or mongod is busy; not starting" | tee -a "$LOG"
  exit 1
fi
health=$(curl -sf -m 3 "$URL/health" || true)
if ! printf '%s' "$health" | grep -q '"ok": true'; then
  echo "$(date -Is) model server $URL is not answering; not starting" | tee -a "$LOG"
  exit 1
fi
for sys in $SYSTEMS; do
  case $sys in
    mlp|gru|tf)
      if ! printf '%s' "$health" | grep -q "\"${MODELS}/$sys\""; then
        echo "$(date -Is) model server has no ${MODELS}/$sys; not starting" | tee -a "$LOG"
        exit 1
      fi
      ;;
  esac
done

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

run() { # app seed routes system
  local app=$1 seed=$2 routes=$3 sys=$4 pfx out pool svc sidecar
  case $app in
    hotel) pfx=hotel-;;
    social) pfx=social-;;
    media) pfx=media-;;
  esac
  out=$BASE/$app-$TAG/$sys
  svc="-svc-min 1 -svc-max 16"
  pool="fixed,start=8,seed_cpu=2,exempt=${pfx}loadgen,max=64"
  sidecar=""
  case $sys in
    mlp|gru|tf)
      sidecar="python3 $M/src/test/system_sidecar.py"
      ;;
    l3shift)
      pool="l3,swap=1,start=8,seed_cpu=2,exempt=${pfx}loadgen"
      ;;
    fixed8)
      pool="fixed,start=8,seed_cpu=2,exempt=${pfx}loadgen,max=8"
      svc="-svc-min 1 -svc-max 8"
      ;;
    fixed64)
      pool="fixed,start=64,seed_cpu=2,exempt=${pfx}loadgen,max=64"
      svc="-svc-min 1 -svc-max 64"
      ;;
    luna)
      sidecar="python3 $M/src/test/system_sidecar.py"
      pool="fixed,start=8,seed_cpu=2,exempt=${pfx}loadgen,max=40"
      ;;
    *) echo "unknown system $sys" | tee -a "$LOG"; return 1;;
  esac
  echo "$(date -Is) $app $sys" | tee -a "$LOG"
  local -a envset
  envset=(POLICIES=saturate SEED="$seed" CPUSTAT=1 SVC_ARGS="$svc" POOL_POLICY="$pool")
  if [ "$sys" = luna ]; then
    local goal llm_args
    case $app in
      hotel) goal="$hotel_goal";;
      social) goal="$social_goal";;
      media) goal="$media_goal";;
    esac
    llm_args="--every 0.5 --model openai/gpt-6-luna --llm openrouter --thinking low --goal '$goal'"
    [ "$app" = media ] && llm_args="$llm_args --app media"
    envset+=(SC_SYSTEM=llm SC_LLM_OWNS=1 SC_REPLICAS=1 MERCURY_REUSEPORT=1
      SC_LLMCTL_PYTHON="$M/tools/llmpy-bin/llmpy"
      SC_LLM_ARGS="$llm_args"
      SIDECAR="$sidecar")
  elif [ -n "$sidecar" ]; then
    envset+=(SC_SYSTEM=llm SC_LLM_OWNS=1
      SC_LLM_ARGS="--llm remote --model ${MODELS}/$sys --every 0.05 --window-ms 50 --remote $URL"
      SIDECAR="$sidecar")
  fi
  if ! env "${envset[@]}" \
      "$M/src/test/dsb_ab.sh" "$app" "$out" 1 "$SEEK -routes $routes" >>"$LOG" 2>&1; then
    echo "$(date -Is) $app $sys failed" | tee -a "$LOG"
    settle
    return 1
  fi
  settle
}

hotel_routes=hotels,recommendations,user,review,restaurants,museums,cinema,reservation
social_routes=compose-post,read-home-timeline,read-user-timeline,follow
media_routes=read-page,compose-review,read-movie-reviews
hotel_goal="The hotels rate starts at 50 requests per second and doubles every 60 seconds while fewer than 10 percent of requests fail. Recommendations is 31/52 of that rate, review, restaurants, museums and cinema are 4/52, and user and reservation are 0.5/52. Above 10 percent the hotels rate drops by 25 percent and the search continues until it finds the highest rate still under 10 percent. Use scale_daemon so each service has the CPUs its cpu_use and pressure show it needs at this second: more when the rate has climbed or the service is waiting, fewer when the rate has fallen or the service is idle. A short service may also take add_replica so new connections spread, and remove_replica when that extra process is idle."
social_goal="The read-home-timeline rate starts at 50 requests per second and doubles every 60 seconds while fewer than 10 percent of requests fail. Read-user-timeline is 30/60 of that rate, compose-post is 10/60, and follow is idle. Above 10 percent the home-timeline rate drops by 25 percent and the search continues until it finds the highest rate still under 10 percent. Use scale_daemon so each service has the CPUs its cpu_use and pressure show it needs at this second: more when the rate has climbed or the service is waiting, fewer when the rate has fallen or the service is idle. A short service may also take add_replica so new connections spread, and remove_replica when that extra process is idle."
media_goal="The compose-review rate starts at 50 requests per second and doubles every 60 seconds while fewer than 10 percent of requests fail. Read-page and read-movie-reviews are 4/92 of that rate. Above 10 percent the compose-review rate drops by 25 percent and the search continues until it finds the highest rate still under 10 percent. Use scale_daemon so each service has the CPUs its cpu_use and pressure show it needs at this second: more when the rate has climbed or the service is waiting, fewer when the rate has fallen or the service is idle. A short service may also take add_replica so new connections spread, and remove_replica when that extra process is idle."

APPS=${APPS:-hotel social media}
for app in $APPS; do
  for sys in $SYSTEMS; do
    case $app in
      hotel) run hotel "$HOTEL_SEED" "$hotel_routes" "$sys" || true;;
      social) run social "$SOCIAL_SEED" "$social_routes" "$sys" || true;;
      media) run media "$MEDIA_SEED" "$media_routes" "$sys" || true;;
      *) echo "unknown app $app" | tee -a "$LOG"; exit 1;;
    esac
  done
done
echo "$(date -Is) model seek finished" | tee -a "$LOG"
