#!/bin/bash
# Luna teacher set for the RL dataset. Repeat 1 (SEED_N 1), then repeat 2 (SEED_N 2).
# Within a repeat: hotel, social, media. Within an application: stable low, medium and
# high with antagonists, the same three holds with no antagonist, one 300-per-minute
# wave up and back down, then a seek. Each schedule runs Luna, a fixed pool of 8, and
# a fixed pool of 40. A directory that already has DONE is skipped.
# Hotel's seek starts at 3100 and stops at 5500. Social and media start at 300 and
# climb until the 10% line. Output: data/dsb/compare/luna-rl-20261105.
set -u
M=$(cd "$(dirname "$0")/../../.." && pwd)
BASE=${BASE:-$M/data/dsb/compare/luna-rl-20261105}
LOG=$BASE/campaign.log
mkdir -p "$BASE"
export OPENROUTER_API_KEY=$(cat "$HOME/.config/mercury/openrouter.key")
HOTEL_SEED=${HOTEL_SEED:-$(cat "$HOME/.seed_hotel")}
SOCIAL_SEED=${SOCIAL_SEED:-$(cat "$HOME/.seed_social")}
MEDIA_SEED=${MEDIA_SEED:-$(cat "$HOME/.seed_media")}

say() { echo "$(date -Is) $*" | tee -a "$LOG"; }

settle() {
  sleep 2
  if ss -ltnH | grep -q ':8080 '; then
    say "stopping leftover mercury"
    sudo -n pkill -f "$M/build/mercury " || true
    sleep 2
  fi
  if pgrep -x mongod >/dev/null; then
    say "stopping leftover mongod"
    sudo -n pkill -x mongod || true
    sleep 2
  fi
}

triangle() { # start end step
  local s=$1 e=$2 d=$3
  local v=$s out=""
  while [ "$v" -lt "$e" ]; do
    out="${out:+$out,}$v"
    v=$((v + d))
  done
  out="$out,$e"
  v=$((e - d))
  while [ "$v" -ge "$s" ]; do
    out="$out,$v"
    v=$((v - d))
  done
  printf '%s' "$out"
}

hold_count() { awk -F, '{print NF}' <<< "$1"; }

# PHASES so one comma-list of 60 s holds fits in DUR = WARMUP + PHASES*(OFF+ON) + 30.
wave_phases() {
  local holds=$1
  local need=$((holds * 60))
  echo $(( (need - 80 + 49) / 50 ))
}

routes_of() {
  case $1 in
    hotel) echo "hotels,recommendations,user,review,restaurants,museums,cinema,reservation";;
    social) echo "compose-post,read-home-timeline,read-user-timeline,follow";;
    media) echo "read-page,compose-review,read-movie-reviews";;
  esac
}

seed_of() {
  case $1 in
    hotel) echo "$HOTEL_SEED";;
    social) echo "$SOCIAL_SEED";;
    media) echo "$MEDIA_SEED";;
  esac
}

mix_of() {
  case $1 in
    hotel) echo "Recommendations is 31/52 of that rate, review, restaurants, museums and cinema are 4/52, and user and reservation are 0.5/52.";;
    social) echo "Read-user-timeline is 30/60 of that rate, compose-post is 10/60, and follow is idle.";;
    media) echo "Read-page and read-movie-reviews are 4/92 of that rate.";;
  esac
}

primary_of() {
  case $1 in
    hotel) echo "hotels";;
    social) echo "read-home-timeline";;
    media) echo "compose-review";;
  esac
}

act="Use scale_daemon so each service has the CPUs its cpu_use and pressure show it needs at this second: more when the service is waiting, fewer when the service is idle. A short service may also take add_replica so new connections spread, and remove_replica when that extra process is idle."

goal_for() { # app kind detail
  local app=$1 kind=$2 detail=$3 name mix
  name=$(primary_of "$app")
  mix=$(mix_of "$app")
  case $kind in
    hold-ant)
      echo "The $name rate is held at $detail requests per second for this run. $mix Antagonists of the kinds same, smt, l3 and membw turn on for 30 seconds and off for 20 seconds, one service at a time. $act";;
    hold-clean)
      echo "The $name rate is held at $detail requests per second for this run. $mix No antagonist is running. $act";;
    wave)
      echo "The $name rate changes by 300 requests per second every 60 seconds. $detail $mix Antagonists of the kinds same, smt, l3 and membw turn on for 30 seconds and off for 20 seconds, one service at a time. $act";;
    seek)
      echo "The $name rate $detail. $mix A step whose error rate is still at or over 10 percent at the end of the minute holds for 120 seconds. Still over, the climb stops. Back under, it climbs by 300 again. No antagonist is running. $act";;
  esac
}

arm_env() { # arm app  -> echoes env assignments as a line the caller evals carefully; we set globals
  local arm=$1 app=$2 pfx
  case $app in
    hotel) pfx=hotel-;;
    social) pfx=social-;;
    media) pfx=media-;;
  esac
  SYSNAME=saturate
  POOL_START=8
  SVC_ARGS="-svc-min 1 -svc-max 16"
  SC_LLM_OWNS=""
  case $arm in
    luna)
      SYSNAME=llm40-luna
      POOL_START=8
      SC_LLM_OWNS=1
      ;;
    fixed8)
      POOL_START=8
      SVC_ARGS="-svc-min 1 -svc-max 8"
      ;;
    fixed40)
      POOL_START=40
      SVC_ARGS="-svc-min 1 -svc-max 40"
      ;;
    *) echo "unknown arm $arm" >&2; return 1;;
  esac
  POOL_POLICY="fixed,start=$POOL_START,seed_cpu=2,exempt=${pfx}loadgen"
  case $arm in
    luna) POOL_POLICY="$POOL_POLICY,max=40";;
    fixed8) POOL_POLICY="$POOL_POLICY,max=8";;
    fixed40) POOL_POLICY="$POOL_POLICY,max=40";;
  esac
}

run_list() { # app sched arm repeat seed levels phases step warmup interfere kind [goal detail]
  local app=$1 sched=$2 arm=$3 rep=$4 seedn=$5 levels=$6 phases=$7 step=$8 warmup=$9 interfere=${10} kind=${11}
  local detail=${12:-$levels}
  local out=$BASE/$app/$sched-$arm/r$rep mark goal
  arm_env "$arm" "$app" || return 1
  mark=$out/$SYSNAME/r1/DONE
  if [ -f "$mark" ]; then say "skip $app $sched $arm r$rep"; return 0; fi
  goal=$(goal_for "$app" "$kind" "$detail")
  mkdir -p "$out"
  printf 'app=%s schedule=%s arm=%s repeat=%s seed_n=%s levels=%s phases=%s step=%s warmup=%s interfere=%s\n' \
    "$app" "$sched" "$arm" "$rep" "$seedn" "$levels" "$phases" "${step:-all}" "$warmup" "$interfere" > "$out/plan.txt"
  say "start $app $sched $arm r$rep seed=$seedn levels=$levels"
  local -a envset=(
    SKIP_SNAPSHOT=1 INTERFERE="$interfere" DB=seed SEED_N="$seedn"
    PHASES="$phases" OFF=20 ON=30 WARMUP="$warmup"
    LEVELS="$levels" ROUTE="$(routes_of "$app")"
    GEN_CORES=16 OP_EXTRA="-inflight 65536"
    SYSTEMS="$SYSNAME" POOL_START="$POOL_START" SVC_ARGS="$SVC_ARGS"
    REPEATS=1
    LLM_KEY_VARS=OPENROUTER_API_KEY
    LLMCTL_PYTHON="$M/tools/llmpy-bin/llmpy"
    LLM_MODELS="luna=openai/gpt-6-luna@openrouter:low"
    LLM_ARGS="--every 0.5 --goal '$goal'"
    KINDS="same,smt,l3,membw"
  )
  [ -n "$step" ] && envset+=(STEP="$step")
  [ -n "$SC_LLM_OWNS" ] && envset+=(SC_LLM_OWNS=1)
  if ! env "${envset[@]}" "$M/src/test/compare_systems.sh" "$out" "$app" >>"$LOG" 2>&1; then
    say "failed $app $sched $arm r$rep (compare_systems exit $?)"
    settle
    return 0
  fi
  if [ -f "$mark" ]; then say "done $app $sched $arm r$rep"; else say "failed $app $sched $arm r$rep"; fi
  settle
}

run_seek() { # app arm repeat
  local app=$1 arm=$2 rep=$3
  local out=$BASE/$app/seek-$arm/r$rep start max tail goal pfx seed
  arm_env "$arm" "$app" || return 1
  if [ -f "$out/DONE" ]; then say "skip $app seek $arm r$rep"; return 0; fi
  case $app in
    hotel) start=3100; max=5500; tail=", and stops at 5500";;
    *) start=300; max=65536; tail=" until 10 percent of requests fail";;
  esac
  case $app in
    hotel) pfx=hotel-;;
    social) pfx=social-;;
    media) pfx=media-;;
  esac
  goal=$(goal_for "$app" seek "starts at $start requests per second and climbs by 300 every 60 seconds$tail")
  seed=$(seed_of "$app")
  mkdir -p "$out"
  printf 'app=%s schedule=seek arm=%s repeat=%s seek_start=%s seek_max=%s seek_inc=300\n' \
    "$app" "$arm" "$rep" "$start" "$max" > "$out/plan.txt"
  say "start $app seek $arm r$rep start=$start max=$max"
  local before seek
  before=$(wc -l < "$LOG")
  seek="-arrivals poisson -load seek -seek-start $start -seek-inc 300 -step 60s -seek-wait 120s -seek-error 0.10 -seek-max $max -duration 0 -window-ms 500 -hist -gen-min 8 -gen-max 16 -inflight 65536 -routes $(routes_of "$app")"
  local -a envset=(POLICIES=saturate SEED="$seed" CPUSTAT=1 SVC_ARGS="$SVC_ARGS" POOL_POLICY="$POOL_POLICY")
  if [ "$arm" = luna ]; then
    local llm_args="--every 0.5 --model openai/gpt-6-luna --llm openrouter --thinking low --goal '$goal'"
    [ "$app" = media ] && llm_args="$llm_args --app media"
    envset+=(SC_SYSTEM=llm SC_LLM_OWNS=1 SC_REPLICAS=1 MERCURY_REUSEPORT=1
      SC_LLMCTL_PYTHON="$M/tools/llmpy-bin/llmpy"
      SC_LLM_ARGS="$llm_args"
      SIDECAR="python3 $M/src/test/system_sidecar.py")
  fi
  if ! env "${envset[@]}" "$M/src/test/dsb_ab.sh" "$app" "$out" 1 "$seek" >>"$LOG" 2>&1; then
    say "failed $app seek $arm r$rep (dsb_ab exit $?)"
    settle
    return 0
  fi
  local rd op
  rd=$(ls -d "$out"/2026*/1-* 2>/dev/null | tail -1)
  op=$(tail -n +"$((before + 1))" "$LOG" | grep "operator exit" | tail -1)
  case $op in
    *"operator exit 0"*) echo "$rd" > "$out/DONE"; say "done $app seek $arm r$rep";;
    *) say "failed $app seek $arm r$rep (${op:-no operator exit})";;
  esac
  settle
}

say "luna rl dataset: snapshot next"
SNAP=$("$M/scripts/backup.sh") || { say "snapshot failed"; exit 1; }
say "snapshot $SNAP"
settle

HOTEL_WAVE=$(triangle 1500 5400 300)
SOCIAL_WAVE=$(triangle 800 2600 300)
MEDIA_WAVE=$(triangle 400 1300 300)
say "waves hotel=$(hold_count "$HOTEL_WAVE") social=$(hold_count "$SOCIAL_WAVE") media=$(hold_count "$MEDIA_WAVE")"

for rep in 1 2; do
  for app in hotel social media; do
    case $app in
      hotel) bands="1500 3100 5300"; names="low mid high"; wave=$HOTEL_WAVE;;
      social) bands="800 1600 2700"; names="low mid high"; wave=$SOCIAL_WAVE;;
      media) bands="400 800 1400"; names="low mid high"; wave=$MEDIA_WAVE;;
    esac
    i=1
    for level in $bands; do
      name=$(echo "$names" | cut -d' ' -f"$i")
      i=$((i + 1))
      for arm in luna fixed8 fixed40; do
        run_list "$app" "$name-ant" "$arm" "$rep" "$rep" "$level" 12 "" 30 1 hold-ant
        run_list "$app" "$name-clean" "$arm" "$rep" "$rep" "$level" 12 "" 30 0 hold-clean
      done
    done
    wave_phrase=""
    case $app in
      hotel) wave_phrase="It climbs from 1500 to 5400 and then steps back down to 1500, one minute at each rate.";;
      social) wave_phrase="It climbs from 800 to 2600 and then steps back down to 800, one minute at each rate.";;
      media) wave_phrase="It climbs from 400 to 1300 and then steps back down to 400, one minute at each rate.";;
    esac
    phases=$(wave_phases "$(hold_count "$wave")")
    for arm in luna fixed8 fixed40; do
      run_list "$app" wave "$arm" "$rep" "$rep" "$wave" "$phases" 60 50 1 wave "$wave_phrase"
      run_seek "$app" "$arm" "$rep"
    done
  done
done
say "luna rl dataset finished"
