#!/bin/bash
# Compare placement systems on DeathStarBench under the same interference: per system one dsb_ab.sh run with
# the operator's autoscaling (AUTOSCALER: saturate, or the gru autoscaler; least_loaded placement, -svc-min 1
# -svc-max 16), Poisson arrivals at one fixed
# rate, from an empty database (DB=fresh, the default; DB=seed restores the seed snapshot instead), per-CPU telemetry (CPUSTAT=1), and system_sidecar.py as SIDECAR: the antagonist
# driver (src/test/interfere.py) plus the system under test. Every system sees the same scenario: the same
# antagonist kinds and levels at the same times (--seed), each phase on a fixed victim service (VICTIMS, phase i
# on the one at i mod n) at that service's busiest CPU. The antagonists switch on and off but take no action of
# their own (--actions none), so only the system moves daemons. Then compare_report.py OUT.
#
#   compare_systems.sh OUT [hotel|social|media]    (default: hotel)
#
# Env: SYSTEMS [saturate l3 shift l3shift degrade llm] (throttle: cut every service's CPU time instead;
#        every llm system, including horiz, offers add_replica / remove_replica (MERCURY_REUSEPORT=1);
#        horiz also sets MERCURY_HORIZ=1, so Mercury itself adds a replica on saturate's scale-up test)
#      DEGRADE_ROUTES [recommendations,restaurants,museums,cinema,review]: the optional routes degrade sheds, in order
#      DB [fresh]: fresh deletes MongoDB's data before every run (dsb_ab.sh FRESH_DB=1, under ~/Mercury/benchmarks);
#        seed restores ~/.seed_<system> (dsb_ab.sh SEED)
#      DSB_ROOT [~/Mercury/benchmarks]: the benchmarks tree; the Linux CPU layout is
#        benchmarks/perturb/perturb/hosts/$(hostname -s).env, same as dsb_ab.sh. degrade needs its frontend, which sheds the
#        routes Mercury disables in MERCURY_POLICY  ROUTE [hotels]  RATE [1600]  PHASES [12]  OFF [20]  ON [30]
#      WARMUP [30; 50 when LEVELS has more than one rate]  SEED_N [1]  GEN_CORES [12]  LLM_ARGS ["--every 0.5"]
#      LEVELS: comma-separated req/s per route, each held for STEP [2*(OFF+ON) seconds] and repeated until the run
#        ends (-duration). Social's default, when RATE is unset, is 100,120,150. One level (the default RATE, or a
#        single LEVELS entry) is held for the whole run. The first change falls at an antagonist off boundary when
#        WARMUP is one phase (OFF+ON) and STEP is two. Hotel's -mix dsb (the operator default) reads LEVELS as the
#        hotels rate and offers the other hotel routes a fixed share of it.

#      AUTOSCALER [saturate]: saturate, or gru (the services run policy gnn without pinning on GRU_MODEL
#        [models/gru_slo4_mft.json]; the load generator stays on saturate)
#      Scenario of 2026-10-21 (too little capacity plus a noisy neighbour): POOL_START=4 RATE=100 AUTOSCALER=gru
#        ROUTE=hotels,reservation,recommendations,review,restaurants,museums,cinema. Without a pool the services use
#        5.6 CPUs and every route is healthy (reservation, database-bound, fails from ~150 req/s per route whatever
#        the CPUs); on a 4-CPU pool the critical routes reach 1.2 s p95 and 19% errors.
#      POOL_START [8]: every system runs on a pool of whole L3 domains of this many CPUs from Mercury's start (MERCURY_POOL,
#        seeded at CPU 2 where the operator starts the services; the load generator is exempt): fixed for saturate,
#        degrade and llm (the llm grows and swaps it itself), l3 for l3 (grows on demand), fixed with swap for shift
#        (a pool domain loaded from outside Mercury is swapped for a clean one). NOPOOL=1: no pool at all.
#      LLM_POOL_MAX: the llm system's pool budget in CPUs (MERCURY_POOL max=; grow_pool refused beyond it). A system
#        llmN (llm15, llm25, ...) is the llm with a budget of N CPUs, so several budgets run interleaved in one OUT;
#        llmdegN is that llm also allowed to shed DEGRADE_ROUTES (llmctl --shed; never the CRITICAL routes)
#      STOP_AT: a time (date -d syntax, e.g. "13:00 tomorrow"): no run starts that would end after it
#      CRITICAL [hotels,reservation]: the routes compare_report.py reports as critical; the others are optional
#      INTERFERE [1]: 0 starts no antagonist driver; DONE then needs only a non-empty loadgen.hist.jsonl
#      SKIP_SNAPSHOT [unset]: 1 skips scripts/backup.sh  OP_EXTRA: appended to the operator arguments
#      KINDS [same,smt,l3,membw]: antagonist kinds; add load for application-load phases (the load generator's rate
#        x1.5 or x2 for the phase). Changing KINDS changes the shuffled schedule, so compare only runs with the same KINDS
#      BASELINE [0]: seconds with no antagonist after WARMUP, logged as event=baseline. The run's load hold is
#        lengthened by that many seconds. SEQUENTIAL=1 walks KINDS in order and ANT_LEVELS [1,2] low to high,
#        without the shuffle. PIN_BUSIEST=1 locks every phase to the daemon that was busiest at the end of the baseline.
#      MIN_USE [0.05]: cores a victim must be using, or that phase is skipped. ANT_POOL [1-60]: CPUs the antagonist may use
#      VICTIMS [hotel: frontend,search,geo,rate,profile,reservation,mongod; social: random by CPU use]
#      GEMINI_API_KEY (llm; read from stdin when unset and llm is in SYSTEMS; never written to disk); LLM_KEY_VAR
#        names another key variable for other providers (OPENROUTER_API_KEY, XAI_API_KEY)
#      LLMCTL_PYTHON: the interpreter for llmctl (one with torch for a distilled model: ALIAS=MODEL.pt@distilled)
# REPEATS [1]: runs per system, interleaved (round r runs every system once), in OUT/<system>/r<n>; every
#   repeat uses the same antagonist schedule (SEED_N), so the repeats measure run-to-run variance.
# Resumable: a run with OUT/<system>/r<n>/DONE is skipped.
set -u
OUT=$(realpath -m "$1"); SYS=${2:-hotel}
M=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)
. "$M/src/test/host_layout.sh"
SYSTEMS=${SYSTEMS:-$([ "$SYS" = media ] && echo "saturate l3 shift l3shift llm" || echo "saturate l3 shift l3shift degrade llm")}
# degrade sheds optional routes at the entry daemon; the media benchmark has no front end to shed at.
if [ "$SYS" = media ] && [[ " $SYSTEMS " =~ \ degrade\  ]]; then echo "degrade has no front end in the media benchmark: leave it out of SYSTEMS"; exit 2; fi
DSB_ROOT=${DSB_ROOT:-$M/benchmarks}
export DSB_ROOT
# hotel starts every run from an empty database; social restores its seed (the social graph and 10 posts per user,
# which the routes need: an empty social network has no one to follow or read).
DB=${DB:-$([ "$SYS" = social ] || [ "$SYS" = media ] && echo seed || echo fresh)}
RATE_SET=${RATE+x}
RATE=${RATE:-1600}
PHASES=${PHASES:-12}; OFF=${OFF:-20}; ON=${ON:-30}; WARMUP_SET=${WARMUP+x}; WARMUP=${WARMUP:-30}; SEED_N=${SEED_N:-1}; GL=${GEN_CORES:-12}
LLM_ARGS=${LLM_ARGS:---every 0.5}; AUTOSCALER=${AUTOSCALER:-saturate}; GRU_MODEL=${GRU_MODEL:-$M/models/gru_slo4_mft.json}
case $AUTOSCALER in
  saturate) SCALER=saturate;;
  gru) [ -f "$GRU_MODEL" ] || { echo "no GRU model $GRU_MODEL"; exit 2; }; SCALER="gnn-nopin@$(realpath $GRU_MODEL)";;
  *) echo "unknown AUTOSCALER $AUTOSCALER"; exit 2;;
esac
case $SYS in
  hotel)  ENTRY="-entry frontend"; PFX=hotel-;  VICTIMS=${VICTIMS-frontend,search,geo,rate,profile,reservation,mongod}
          CRITICAL=${CRITICAL:-hotels,reservation}
          DEGRADE_ROUTES=${DEGRADE_ROUTES:-recommendations,restaurants,museums,cinema,review}; DEGRADE_DAEMONS=frontend;;
  # social: no frontend (the load generator calls the services over Thrift); the optional routes are shed by the
  # services that serve them, and only by their route's methods (ReadUserTimeline; Follow/Unfollow), so compose-post's
  # own calls into those services go on.
  media)  ENTRY="";                PFX=media-
          VICTIMS=${VICTIMS-PageService,ComposeReviewService,MovieReviewService,ReviewStorageService,MovieInfoService,UserService,mongod-27020,mongod-27021,mongod-27022}
          CRITICAL=${CRITICAL:-read-page,compose-review}
          DEGRADE_ROUTES=${DEGRADE_ROUTES:-}; DEGRADE_DAEMONS=;;
  social) ENTRY="";                PFX=social-
          VICTIMS=${VICTIMS-ComposePostService,HomeTimelineService,UserTimelineService,PostStorageService,SocialGraphService,mongod-27040,mongod-27042,mongod-27043,redis-server}
          CRITICAL=${CRITICAL:-compose-post,read-home-timeline}
          DEGRADE_ROUTES=${DEGRADE_ROUTES:-follow,read-user-timeline}; DEGRADE_DAEMONS=SocialGraphService,UserTimelineService;;
  *) echo "unknown system $SYS"; exit 2;;
esac
[ "$SYS" = social ] && ROUTE=${ROUTE:-compose-post,read-home-timeline,read-user-timeline,follow}
[ "$SYS" = media ] && ROUTE=${ROUTE:-read-page,compose-review,read-movie-reviews}
[ "$SYS" = media ] && LLM_ARGS="$LLM_ARGS --app media"  # the controller describes the media application
ROUTE=${ROUTE:-hotels}
DBENV="FRESH_DB=1"
if [ "$DB" = seed ]; then
  case $SYS in
    hotel) SEEDDIR=${SEEDDIR:-$(cat ~/.seed_hotel)};;
    media) SEEDDIR=${MEDIA_SEED:-$(cat ~/.seed_media)};;
    *) SEEDDIR=${SOCIAL_SEED:-$(cat ~/.seed_social)};;
  esac
  DBENV="SEED=$SEEDDIR"
fi
POOL_START=${POOL_START:-8}
POOL_COMMON="start=$POOL_START,seed_cpu=2,exempt=${PFX}loadgen"
pool_for() {  # the MERCURY_POOL of one system
  [ -n "${NOPOOL:-}" ] && return
  case $1 in
    l3) echo "l3,$POOL_COMMON";;
    l3shift) echo "l3,swap=1,$POOL_COMMON";;
    shift) echo "fixed,swap=1,$POOL_COMMON";;
    llm|horiz) echo "fixed,$POOL_COMMON${LLM_POOL_MAX:+,max=$LLM_POOL_MAX}";;
    llm[0-9]*) echo "fixed,$POOL_COMMON,max=${1#llm}";;
    llmdeg[0-9]*) echo "fixed,$POOL_COMMON,max=${1#llmdeg}";;
    *) echo "fixed,$POOL_COMMON";;
  esac
}
STOP_EPOCH=""; [ -n "${STOP_AT:-}" ] && STOP_EPOCH=$(date -d "$STOP_AT" +%s)
# The llm's API keys, read from stdin when unset, one line each in this order (never written to disk): LLM_KEY_VARS
# names them (default GEMINI_API_KEY; e.g. "GEMINI_API_KEY OPENROUTER_API_KEY" when LLM_MODELS mixes providers;
# XAI_API_KEY for grok).
LLM_KEY_VARS=${LLM_KEY_VARS:-${LLM_KEY_VAR:-GEMINI_API_KEY}}
# A checkpoint on the eval-set server, or a tools/learn/rl checkpoint, needs no API key.
# Anything else named llm does.
need_key=0
need_torch=0
for sysname in $SYSTEMS; do
  if [[ $sysname == horiz ]] || [[ $sysname =~ ^llm(deg)?[0-9]*$ ]]; then
    need_key=1
  elif [[ $sysname =~ ^llm(deg)?[0-9]+-(.+)$ ]]; then
    spec=$(tr ',' '\n' <<< "${LLM_MODELS:-}" | sed -n "s/^${BASH_REMATCH[2]}=//p" | head -1)
    [[ $spec =~ :(off|low|high|default)$ ]] && spec=${spec%:*}
    provider=""; [[ $spec == *@* ]] && provider=${spec##*@}
    if [ "$provider" = rl ]; then
      need_torch=1
    elif [ "$provider" != remote ]; then
      need_key=1
    fi
  fi
done
if [ "$need_torch" = 1 ] && [ -z "${LLMCTL_PYTHON:-}" ] && [ -x /home/paul/opt/torchenv/bin/python ]; then
  LLMCTL_PYTHON=/home/paul/opt/torchenv/bin/python
fi
if [ "$need_key" = 1 ]; then
  for v in $LLM_KEY_VARS; do [ -z "${!v:-}" ] && read -r "$v"; export "$v"; done
fi
# Snapshot the repository before the experiment: scripts/restore.sh SNAPSHOT restores the state it ran from.
# SKIP_SNAPSHOT=1 leaves that to the caller (a campaign that already took one).
mkdir -p "$OUT"
if [ -z "${SKIP_SNAPSHOT:-}" ]; then
  SNAP=$("$M/scripts/backup.sh") || { echo "snapshot failed: the experiment is not started"; exit 1; }
  echo "snapshot $SNAP" >> "$OUT/meta.txt"
fi
# Social, with RATE left unset, cycles low/medium/high (100, 120, 150 req/s per route). Any app cycles when LEVELS
# has more than one entry. One entry is held for the whole run.
if [ -z "${LEVELS:-}" ]; then
  if [ "$SYS" = social ] && [ -z "$RATE_SET" ]; then LEVELS=100,120,150; else LEVELS=$RATE; fi
fi
case $LEVELS in
  *,*)
    STEP=${STEP:-$((2 * (OFF + ON)))}
    [ -z "$WARMUP_SET" ] && WARMUP=$((OFF + ON))
    ;;
esac
# usage() sleeps 2 s at each phase, and --pin-busiest sleeps another 2 s. The usual 30 s slack covers the
# shuffled schedule. A baseline run adds that sleep on top so the last phase is not cut off.
SLACK=30
if [ -n "${BASELINE:-}${SEQUENTIAL:-}${PIN_BUSIEST:-}" ]; then SLACK=$((30 + PHASES * 2 + 2)); fi
DUR=$((WARMUP + ${BASELINE:-0} + PHASES * (OFF + ON) + SLACK))
if [ -n "${STEP:-}" ]; then LOAD_STEP=${STEP}s; LOAD_DUR="-duration ${DUR}s"; else LOAD_STEP=${DUR}s; LOAD_DUR=""; fi
echo "started $(date -Is) $SYS $ROUTE levels=$LEVELS step=${STEP:-all}s systems=[$SYSTEMS] PHASES=$PHASES OFF=$OFF ON=$ON WARMUP=$WARMUP BASELINE=${BASELINE:-0} sequential=${SEQUENTIAL:-0} pin_busiest=${PIN_BUSIEST:-0} seed=$SEED_N kinds=${KINDS:-same,smt,l3,membw} levels_ant=${ANT_LEVELS:-1,2} victims=[$VICTIMS] autoscaler=$SCALER repeats=${REPEATS:-1} stop_at=${STOP_AT:-none} llm_pool_max=${LLM_POOL_MAX:-none} pool_start=${NOPOOL:+none}${NOPOOL:-$POOL_START} critical=${CRITICAL:-} dsb_root=$DSB_ROOT db=$DB run ${DUR}s mercury $(cat $M/SOURCE 2>/dev/null || git -C $M log --oneline -1)" >> "$OUT/meta.txt"
REPEATS=${REPEATS:-1}
for r in $(seq 1 $REPEATS); do for sysname in $SYSTEMS; do
  D=$OUT/$sysname/r$r
  [ -f "$D/DONE" ] && continue
  # A run takes the load phase plus about 90 s of start, seed and teardown.
  if [ -n "$STOP_EPOCH" ] && [ $(( $(date +%s) + DUR + 90 )) -gt "$STOP_EPOCH" ]; then
    echo "$(date +%T) STOP_AT $STOP_AT reached before $sysname r$r" | tee -a "$OUT/progress.log"
    break 2
  fi
  mkdir -p "$D"; echo "$(date +%T) RUN $sysname r$r" | tee -a "$OUT/progress.log"
  # dsb_ab.sh splits SIDECAR on spaces, so the sidecar's settings travel in the environment.
  # llmN: the llm with budget N; llmdegN: also shedding optional routes; llmN-ALIAS / llmdegN-ALIAS: with the model
  # LLM_MODELS maps ALIAS to, as ALIAS=MODEL[@PROVIDER][:THINKING] (llmctl --model, --llm, --thinking), e.g.
  # LLM_MODELS="gem=gemini-3.8-flash@gemini:low,luna=openai/gpt-6-luna@openrouter:low,q9=qwen/qwen3.5-9b@openrouter:off"
  # with SYSTEMS="llm25-gem llm25-luna llm25-q9". A trailing :off/:low/:high/:default is the thinking setting (model
  # ids may contain ":" themselves, e.g. ":free").
# PROVIDER remote is the eval-set server (serve.py on pd-gpu): MODEL is the checkpoint, for example
# luna-20261102/gru. SYSTEMS="llm40-luna llm40-tf llm40-gru llm40-mlp l3shift" is Luna, the transformer,
# the GRU, the MLP, and Shift+L3 on saturate. A remote run detaches the autoscaler and ticks every
# EVALSET_EVERY seconds (0.05, the training tick) at EVALSET_URL (http://10.30.0.1:8790).
# PROVIDER rl is tools/learn/rl: MODEL is a train.py checkpoint, for example
# LLM_MODELS="gnn-imitation=/path/gnn-imitation.pt@rl,tf-imitation=/path/tf-imitation.pt@rl" and
# SYSTEMS="llm40-gnn-imitation llm40-tf-imitation". The checkpoint's setting is the row. An rl run
# detaches the autoscaler and uses the same 0.05 s tick and 50 ms window. It needs no API key.
# When LLMCTL_PYTHON is unset and /home/paul/opt/torchenv/bin/python exists, that interpreter runs llmctl.
  sysbase=$sysname; run_llm_args=$LLM_ARGS; provider=""
  if [[ $sysname =~ ^llm(deg)?[0-9]+-(.+)$ ]]; then
    sysbase=${sysname%-*}; alias=${BASH_REMATCH[2]}
    spec=$(tr ',' '\n' <<< "${LLM_MODELS:-}" | sed -n "s/^$alias=//p" | head -1)
    [ -n "$spec" ] || { echo "no model for alias $alias in LLM_MODELS"; exit 2; }
    think=""; [[ $spec =~ :(off|low|high|default)$ ]] && { think=${BASH_REMATCH[1]}; spec=${spec%:*}; }
    [[ $spec == *@* ]] && { provider=${spec##*@}; spec=${spec%@*}; }
    run_llm_args="$LLM_ARGS --model $spec${provider:+ --llm $provider}${think:+ --thinking $think}"
  fi
  owns_set=0
  if [ "$provider" = remote ] || [ "$provider" = rl ]; then
    # The training tick is 50 ms. An --every left over from an LLM goal would call ten times slower.
    run_llm_args=$(printf '%s' "$run_llm_args" | sed -E 's/--every[[:space:]]+[0-9.]+//; s/--window-ms[[:space:]]+[0-9]+//')
    run_llm_args="$run_llm_args --every ${EVALSET_EVERY:-0.05} --window-ms ${EVALSET_WINDOW_MS:-50}"
    if [ "$provider" = remote ]; then
      run_llm_args="$run_llm_args --remote ${EVALSET_URL:-http://10.30.0.1:8790}"
    fi
    if [ -z "${SC_LLM_OWNS:-}" ]; then SC_LLM_OWNS=1; owns_set=1; fi
  fi
  # Replicas are part of every llm test. horiz is that same controller, and Mercury adds replicas on
  # saturate's scale-up test (MERCURY_HORIZ) as well as the model being offered the tools.
  SC_SYS=$sysbase; SC_LLM_SHED=""; SC_REPLICAS=""; MERCURY_HORIZ=""
  [[ $sysbase == horiz ]] && MERCURY_HORIZ=1
  [[ $sysbase == llm || $sysbase == horiz || $sysbase == llm[0-9]* ]] && SC_SYS=llm SC_REPLICAS=1
  [[ $sysbase == llmdeg[0-9]* ]] && SC_SYS=llm SC_LLM_SHED=1 SC_REPLICAS=1
  POOL_POLICY=$(pool_for $sysbase); POOL_SHIFT=""; [ -n "$POOL_POLICY" ] && POOL_SHIFT=1
  echo "$sysname pool=${POOL_POLICY:-none}" >> "$OUT/meta.txt"
  # MERCURY_REUSEPORT must be a real assignment. A ${var:+NAME=value} word is the command name, so the
  # next assignment (SC_CRITICAL=...) was executed and every run exited at once.
  if [ "${INTERFERE:-1}" = 0 ]; then
    SC_INTERFERE=""
  else
    SC_INTERFERE="python3 $M/src/test/interfere.py --phases $PHASES --off $OFF --on $ON --warmup $WARMUP --seed $SEED_N --actions none${MIN_USE:+ --min-use $MIN_USE}${ANT_POOL:+ --pool $ANT_POOL}${VICTIMS:+ --victims-cycle $VICTIMS}${KINDS:+ --kinds $KINDS}${BASELINE:+ --baseline $BASELINE}${SEQUENTIAL:+ --sequential}${ANT_LEVELS:+ --levels $ANT_LEVELS}${PIN_BUSIEST:+ --pin-busiest}"
  fi
  MERCURY_REUSEPORT=${SC_REPLICAS:+1} MERCURY_HORIZ=${MERCURY_HORIZ:-} \
  POOL_POLICY=$POOL_POLICY SC_POOL_SHIFT=$POOL_SHIFT SC_SYSTEM=$SC_SYS SC_LLM_SHED=$SC_LLM_SHED SC_REPLICAS=$SC_REPLICAS SC_CRITICAL=$CRITICAL SC_LLM_ARGS="$run_llm_args" SC_LLMCTL_PYTHON=${LLMCTL_PYTHON:-python3} SC_DEGRADE_ROUTES=$DEGRADE_ROUTES SC_DEGRADE_DAEMONS=$DEGRADE_DAEMONS \
    SC_INTERFERE="$SC_INTERFERE" \
    env $DBENV CPUSTAT=1 POLICIES=$SCALER SVC_ARGS="${SVC_ARGS:--svc-min 1 -svc-max 16}" \
    SIDECAR="python3 $M/src/test/system_sidecar.py" \
    "$M/src/test/dsb_ab.sh" $SYS "$D" 1 \
    "$ENTRY -routes $ROUTE -arrivals poisson -load list -levels $LEVELS -step $LOAD_STEP $LOAD_DUR -window-ms 500 -hist -gen-min $GL -gen-max $GL ${OP_EXTRA:-}" \
    > "$D/driver.log" 2>&1
  if [ "$owns_set" = 1 ]; then unset SC_LLM_OWNS; fi
  rd=$(ls -d "$D"/2026*/1-* 2>/dev/null | tail -1)
  n=$(grep -c '"event": "on"' "$rd/antagonists.jsonl" 2>/dev/null || true)
  if [ -s "$rd/loadgen.hist.jsonl" ] && { [ "${INTERFERE:-1}" = 0 ] || [ "${n:-0}" -gt 0 ]; }; then
    echo "$rd" > "$D/DONE"
  fi
  echo "$(date +%T) END $sysname r$r phases_on=${n:-0} $([ -f $D/DONE ] && echo done || echo FAILED)" | tee -a "$OUT/progress.log"
  [ -f "$D/DONE" ] && [ -f ~/gnn-example/mercury/slo/interfere_report.py ] &&
    python3 ~/gnn-example/mercury/slo/interfere_report.py "$(cat $D/DONE)" > "$D/interfere_report.txt" 2>&1
done; done
CRITICAL=${CRITICAL:-} python3 "$M/src/test/compare_report.py" "$OUT" | tee "$OUT/report.txt"
