#!/bin/bash
# One application at a time. APPS defaults to hotel.
# For that application: 3 Luna repeats, then 3 repeats of horiz, l3shift and
# degrade, then action-tf. Each repeat is the medium hold with antagonists, the
# seven-step wave, and the seek. Action-tf repeats until its mean latency on
# every section is at most 80% of the mean of the three Luna repeats, or until
# five repeats in a row fail to lower its best mean latency. A directory that
# already has DONE is skipped.
# Output: data/dsb/compare/action-tf-20261111.
# CHECKPOINT defaults to the hotel overnight winner. The other winners are
# hotel-lmh-ctx6-from2-20261108 and social-lmh-20261109.
set -u
M=$(cd "$(dirname "$0")/../../.." && pwd)
BASE=${BASE:-$M/data/dsb/compare/action-tf-20261111}
APPS=${APPS:-hotel}
STALL_MAX=${STALL_MAX:-5}
CHECKPOINT=${CHECKPOINT:-$M/data/learn/action-cycle/hotel-lmh-overnight-20261109/winner.pt}
export OPENROUTER_API_KEY=$(cat "$HOME/.config/mercury/openrouter.key")
LOG=$BASE/campaign.log
mkdir -p "$BASE"
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
    wave)
      echo "The $name rate changes by 300 requests per second every 60 seconds. $detail $mix Antagonists of the kinds same, smt, l3 and membw turn on for 30 seconds and off for 20 seconds, one service at a time. $act";;
    seek)
      echo "The $name rate $detail. $mix A step whose error rate is still at or over 10 percent at the end of the minute holds for 120 seconds. Still over, the climb stops. Back under, it climbs by 1000 again. No antagonist is running. $act";;
  esac
}

victims_of() {
  case $1 in
    hotel) echo "frontend,search,geo,rate,profile,reservation,mongod";;
    social) echo "ComposePostService,HomeTimelineService,UserTimelineService,PostStorageService,SocialGraphService,mongod-27040,mongod-27042,mongod-27043,redis-server";;
    media) echo "PageService,ComposeReviewService,MovieReviewService,ReviewStorageService,MovieInfoService,UserService,mongod-27020,mongod-27021,mongod-27022";;
  esac
}

arm_env() { # arm app. Sets SYSNAME, POOL_POLICY, SVC_ARGS, and the direct-launch flags.
  local arm=$1 app=$2 pfx
  case $app in
    hotel) pfx=hotel-;;
    social) pfx=social-;;
    media) pfx=media-;;
  esac
  POOL_START=8
  SVC_ARGS="-svc-min 1 -svc-max 16"
  SC_LLM_OWNS=""
  DIRECT=""
  HORIZ=""
  SC_SYSTEM=""
  DEGRADE_ROUTES=""
  DEGRADE_DAEMONS=""
  case $arm in
    luna)
      SYSNAME=llm40-luna
      SC_LLM_OWNS=1
      POOL_POLICY="fixed,start=8,seed_cpu=2,exempt=${pfx}loadgen,max=40"
      ;;
    action)
      SYSNAME=llm40-action
      SC_LLM_OWNS=1
      POOL_POLICY="fixed,start=8,seed_cpu=2,exempt=${pfx}loadgen,max=40"
      ;;
    horiz)
      SYSNAME=horiz
      DIRECT=1
      HORIZ=1
      SC_SYSTEM=saturate
      POOL_POLICY="fixed,start=8,seed_cpu=2,exempt=${pfx}loadgen"
      ;;
    l3shift)
      SYSNAME=l3shift
      SC_SYSTEM=l3shift
      POOL_POLICY="l3,swap=1,start=8,seed_cpu=2,exempt=${pfx}loadgen"
      ;;
    degrade)
      SYSNAME=degrade
      SC_SYSTEM=degrade
      POOL_POLICY="fixed,start=8,seed_cpu=2,exempt=${pfx}loadgen"
      case $app in
        hotel)
          DEGRADE_ROUTES="recommendations,restaurants,museums,cinema,review"
          DEGRADE_DAEMONS="frontend";;
        social)
          DEGRADE_ROUTES="follow,read-user-timeline"
          DEGRADE_DAEMONS="SocialGraphService,UserTimelineService";;
        *) echo "degrade has no front end on $app" >&2; return 1;;
      esac
      ;;
    *) echo "unknown arm $arm" >&2; return 1;;
  esac
}

drive_direct() { # out app extra interfere phases warmup seedn label
  local out=$1 app=$2 extra=$3 interfere=$4 phases=$5 warmup=$6 seedn=$7 label=$8
  local before interfere_cmd=""
  if [ "$interfere" = 1 ]; then
    interfere_cmd="python3 $M/src/test/interfere.py --phases $phases --off 20 --on 30 --warmup $warmup --seed $seedn --actions none --victims-cycle $(victims_of "$app") --kinds same,smt,l3,membw"
  fi
  before=$(wc -l < "$LOG")
  local -a envset=(
    POLICIES=saturate SEED="$(seed_of "$app")" CPUSTAT=1
    SVC_ARGS="$SVC_ARGS" POOL_POLICY="$POOL_POLICY"
    SIDECAR="python3 $M/src/test/system_sidecar.py"
    SC_SYSTEM="$SC_SYSTEM" SC_POOL_SHIFT=1
    SC_INTERFERE="$interfere_cmd"
    SC_DEGRADE_ROUTES="$DEGRADE_ROUTES" SC_DEGRADE_DAEMONS="$DEGRADE_DAEMONS"
  )
  [ -n "$HORIZ" ] && envset+=(MERCURY_HORIZ=1 MERCURY_REUSEPORT=1)
  if ! env "${envset[@]}" "$M/src/test/dsb_ab.sh" "$app" "$out" 1 "$extra" >>"$LOG" 2>&1; then
    say "failed $label (dsb_ab exit $?)"
    settle
    return 0
  fi
  local rd op
  rd=$(ls -d "$out"/2026*/1-* 2>/dev/null | tail -1)
  op=$(tail -n +"$((before + 1))" "$LOG" | grep "operator exit" | tail -1)
  case $op in
    *"operator exit 0"*) echo "$rd" > "$out/DONE"; say "done $label";;
    *) say "failed $label (${op:-no operator exit})";;
  esac
  settle
}

run_list() { # app sched arm repeat seed levels phases step warmup interfere kind [goal detail]
  local app=$1 sched=$2 arm=$3 rep=$4 seedn=$5 levels=$6 phases=$7 step=$8 warmup=$9 interfere=${10} kind=${11}
  local detail=${12:-$levels}
  local out=$BASE/$app/$sched-$arm/r$rep mark goal entry dur load_step load_dur extra
  arm_env "$arm" "$app" || return 1
  if [ -n "$DIRECT" ]; then mark=$out/DONE; else mark=$out/$SYSNAME/r1/DONE; fi
  if [ -f "$mark" ]; then say "skip $app $sched $arm r$rep"; return 0; fi
  goal=$(goal_for "$app" "$kind" "$detail")
  mkdir -p "$out"
  printf 'app=%s schedule=%s arm=%s repeat=%s seed_n=%s levels=%s phases=%s step=%s warmup=%s interfere=%s\n' \
    "$app" "$sched" "$arm" "$rep" "$seedn" "$levels" "$phases" "${step:-all}" "$warmup" "$interfere" > "$out/plan.txt"
  say "start $app $sched $arm r$rep seed=$seedn levels=$levels"
  if [ -n "$DIRECT" ]; then
    dur=$((warmup + phases * 50 + 30))
    if [ -n "$step" ]; then load_step=${step}s; load_dur="-duration ${dur}s"; else load_step=${dur}s; load_dur=""; fi
    entry=""; [ "$app" = hotel ] && entry="-entry frontend"
    extra="$entry -routes $(routes_of "$app") -arrivals poisson -load list -levels $levels -step $load_step $load_dur -window-ms 500 -hist -gen-min 16 -gen-max 16 -inflight 65536"
    drive_direct "$out" "$app" "$extra" "$interfere" "$phases" "$warmup" "$seedn" "$app $sched $arm r$rep"
    return 0
  fi
  local -a envset=(
    SKIP_SNAPSHOT=1 INTERFERE="$interfere" DB=seed SEED_N="$seedn"
    PHASES="$phases" OFF=20 ON=30 WARMUP="$warmup"
    LEVELS="$levels" ROUTE="$(routes_of "$app")"
    GEN_CORES=16 OP_EXTRA="-inflight 65536"
    SYSTEMS="$SYSNAME" POOL_START="$POOL_START" SVC_ARGS="$SVC_ARGS"
    REPEATS=1
    KINDS="same,smt,l3,membw"
  )
  if [ "$arm" = luna ]; then
    envset+=(
      LLM_KEY_VARS=OPENROUTER_API_KEY
      LLMCTL_PYTHON="$M/tools/llmpy-bin/llmpy"
      LLM_MODELS="luna=openai/gpt-6-luna@openrouter:low"
      LLM_ARGS="--every 0.5 --goal '$goal'"
    )
  elif [ "$arm" = action ]; then
    envset+=(
      LLMCTL_PYTHON="/home/paul/opt/torchenv/bin/python"
      LLM_MODELS="action=${CHECKPOINT}@rl"
      LLM_ARGS="--gate --goal '$goal'"
      EVALSET_EVERY=0.5
      EVALSET_WINDOW_MS=1000
    )
  fi
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
  local out=$BASE/$app/seek-$arm/r$rep start max tail goal seed seek
  arm_env "$arm" "$app" || return 1
  if [ -f "$out/DONE" ]; then say "skip $app seek $arm r$rep"; return 0; fi
  case $app in
    hotel) start=3100; max=10000; tail=", and stops at 10000";;
    social) start=1600; max=10000; tail=", and stops at 10000";;
    media) start=800; max=65536; tail=" until 10 percent of requests fail";;
  esac
  goal=$(goal_for "$app" seek "starts at $start requests per second and climbs by 1000 every 60 seconds$tail")
  seed=$(seed_of "$app")
  mkdir -p "$out"
  printf 'app=%s schedule=seek arm=%s repeat=%s seek_start=%s seek_max=%s seek_inc=1000\n' \
    "$app" "$arm" "$rep" "$start" "$max" > "$out/plan.txt"
  say "start $app seek $arm r$rep start=$start max=$max"
  seek="-arrivals poisson -load seek -seek-start $start -seek-inc 1000 -step 60s -seek-wait 120s -seek-error 0.10 -seek-max $max -duration 0 -window-ms 500 -hist -gen-min 8 -gen-max 16 -inflight 65536 -routes $(routes_of "$app")"
  if [ "$arm" != luna ] && [ "$arm" != action ]; then
    drive_direct "$out" "$app" "$seek" 0 0 0 "$rep" "$app seek $arm r$rep"
    return 0
  fi
  local before=$LOG
  before=$(wc -l < "$LOG")
  local -a envset=(POLICIES=saturate SEED="$seed" CPUSTAT=1 SVC_ARGS="$SVC_ARGS" POOL_POLICY="$POOL_POLICY")
  if [ "$arm" = action ]; then
    local llm_args="--every 0.5 --window-ms 1000 --gate --model $CHECKPOINT --llm rl --goal '$goal'"
    [ "$app" = media ] && llm_args="$llm_args --app media"
    envset+=(SC_SYSTEM=llm SC_LLM_OWNS=1 SC_REPLICAS=1 MERCURY_REUSEPORT=1
      SC_LLMCTL_PYTHON="/home/paul/opt/torchenv/bin/python"
      SC_LLM_ARGS="$llm_args"
      SIDECAR="python3 $M/src/test/system_sidecar.py")
  else
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

# Mean latency in ms of one finished run. Histogram bucket bounds stand for the requests in them.
section_ms() { # app arm sched rep
  python3 - "$BASE" "$1" "$2" "$3" "$4" << 'PY'
import json, os, sys
base, app, arm, sched, rep = sys.argv[1:6]
out = os.path.join(base, app, f"{sched}-{arm}", f"r{rep}")
names = {"luna": "llm40-luna", "action": "llm40-action", "l3shift": "l3shift", "degrade": "degrade"}
mark = os.path.join(out, "DONE") if sched == "seek" or arm == "horiz" else os.path.join(out, names[arm], "r1", "DONE")
if not os.path.isfile(mark):
    print("nan")
    raise SystemExit
run = open(mark).read().strip()
path = os.path.join(run, "loadgen.hist.jsonl")
if not os.path.isfile(path):
    print("nan")
    raise SystemExit
num = den = 0
for line in open(path):
    hist = json.loads(line).get("hist") or []
    pairs = hist.items() if isinstance(hist, dict) else hist
    for pair in pairs:
        num += int(pair[0]) * int(pair[1])
        den += int(pair[1])
print("nan" if den == 0 else f"{num / den / 1e6:.4f}")
PY
}

mean3() { # app arm sched. Mean of repeats 1, 2 and 3, or nan if one is missing.
  python3 - "$(section_ms "$1" "$2" "$3" 1)" "$(section_ms "$1" "$2" "$3" 2)" "$(section_ms "$1" "$2" "$3" 3)" << 'PY'
import statistics, sys
vals = []
for s in sys.argv[1:]:
    if s == "nan":
        print("nan")
        raise SystemExit
    vals.append(float(s))
print(f"{statistics.mean(vals):.4f}")
PY
}

# One action-tf repeat against the mean of the three Luna repeats.
judge() { # app rep
  local app=$1 rep=$2
  python3 - \
    "$(section_ms "$app" action mid-ant "$rep")" \
    "$(section_ms "$app" action wave "$rep")" \
    "$(section_ms "$app" action seek "$rep")" \
    "$(mean3 "$app" luna mid-ant)" \
    "$(mean3 "$app" luna wave)" \
    "$(mean3 "$app" luna seek)" << 'PY'
import sys
names = ("hold", "wave", "seek")
act, luna = sys.argv[1:4], sys.argv[4:7]
beat, scores = True, []
for name, a, l in zip(names, act, luna):
    if a == "nan" or l == "nan" or float(l) <= 0:
        print(f"{name} luna={l} action={a} beat=0")
        beat = False
        continue
    bar = 0.8 * float(l)
    ok = float(a) <= bar
    beat = beat and ok
    scores.append(float(a))
    print(f"{name} luna={float(l):.4f} action={float(a):.4f} bar={bar:.4f} beat={int(ok)}")
if len(scores) == 3:
    print(f"summary beat={int(beat)} score={sum(scores) / 3:.4f}")
else:
    print("summary beat=0 score=nan")
PY
}

pass() { # app arm rep
  local app=$1 arm=$2 rep=$3 level wave phases phrase
  arm_env "$arm" "$app" || { say "skip $app $arm r$rep"; return 0; }
  case $app in
    hotel) level=3100; wave=$HOTEL_WAVE; phrase="It climbs from 1500 to 2400 and then steps back down to 1500, one minute at each rate.";;
    social) level=1600; wave=$SOCIAL_WAVE; phrase="It climbs from 800 to 1700 and then steps back down to 800, one minute at each rate.";;
    media) level=800; wave=$MEDIA_WAVE; phrase="It climbs from 400 to 1300 and then steps back down to 400, one minute at each rate.";;
  esac
  phases=$(wave_phases "$(hold_count "$wave")")
  run_list "$app" mid-ant "$arm" "$rep" "$rep" "$level" 12 "" 30 1 hold-ant
  run_list "$app" wave "$arm" "$rep" "$rep" "$wave" "$phases" 60 50 1 wave "$phrase"
  run_seek "$app" "$arm" "$rep"
}

say "action-tf eval: apps=$APPS snapshot next"
SNAP=$("$M/scripts/backup.sh") || { say "snapshot failed"; exit 1; }
say "snapshot $SNAP"
settle

HOTEL_WAVE=$(triangle 1500 2400 300)
SOCIAL_WAVE=$(triangle 800 1700 300)
MEDIA_WAVE=$(triangle 400 1300 300)
say "waves hotel=$(hold_count "$HOTEL_WAVE") social=$(hold_count "$SOCIAL_WAVE") media=$(hold_count "$MEDIA_WAVE")"

for rep in 1 2 3; do
  for app in $APPS; do
    pass "$app" luna "$rep"
  done
done
for rep in 1 2 3; do
  for app in $APPS; do
    for arm in horiz l3shift degrade; do
      pass "$app" "$arm" "$rep"
    done
  done
done

for app in $APPS; do
  best=""
  stall=0
  rep=1
  while true; do
    pass "$app" action "$rep"
    report=$(judge "$app" "$rep")
    say "$app action r$rep"
    while IFS= read -r line; do say "  $line"; done <<< "$report"
    summary=$(echo "$report" | awk '/^summary /{print}')
    beat=$(echo "$summary" | sed -n 's/.*beat=\([0-9]\).*/\1/p')
    score=$(echo "$summary" | sed -n 's/.*score=\([^ ]*\).*/\1/p')
    if [ "$beat" = 1 ]; then
      say "$app action met the Luna bar at r$rep"
      break
    fi
    moved=0
    if [ -n "$score" ] && [ "$score" != nan ]; then
      if [ -z "$best" ] || python3 -c 'import sys; sys.exit(0 if float(sys.argv[1]) < float(sys.argv[2]) else 1)' "$score" "$best"; then
        best=$score
        moved=1
      fi
    fi
    if [ "$moved" = 1 ]; then
      stall=0
      say "$app action r$rep moved forward to ${score} ms"
    else
      stall=$((stall + 1))
      say "$app action r$rep did not move forward ($stall/$STALL_MAX)"
    fi
    if [ "$stall" -ge "$STALL_MAX" ]; then
      say "$app action stopped: $STALL_MAX repeats did not move forward"
      break
    fi
    rep=$((rep + 1))
  done
done
say "action-tf eval finished"

