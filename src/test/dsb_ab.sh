#!/bin/bash
# DSB closed-loop A/B under Mercury: alternating saturate/gnn repeats with a DSB operator.
#   dsb_ab.sh hotel|social|media OUT REPEATS "operator extra args" [--trace]
# Starts its own mercury (port 8080, worker CPU 0) per run, the operator's autoscale
# (which starts and kills every service), and dsb_sample.py. Stops mercury with SIGTERM.
# The Linux CPU layout comes from benchmarks/perturb/perturb/hosts/$(hostname -s).env
# (Apollo and dtn-01). HOST_ENV selects another file. Names that file leaves unset keep
# dtn-01: POOL 1-60, GEN_CPU 60, OP_CPU 61, TRACE_CPUS 62,63, SAMPLE_CPU 63.
# DSB_ROOT: benchmarks directory (default: this repository's benchmarks/).
# POOL: Mercury's daemon CPUs (Mercury itself takes CPU 0). GEN_CPU: the loadgen home
# CPU, inside POOL. OP_CPU: the operator. TRACE_CPUS: threadtrace. SAMPLE_CPU: dsb_sample.py.
# The last three stay outside POOL. PORT: Mercury's port (default 8080).
# Each run dir gets rings/ (Mercury's 10 ms archives) and alloc_vs_use.{png,txt} from tools/plots/rings/plot_rings.py.
# EXTERNAL=1: use the mercury already listening on PORT instead of starting and stopping one per run.
#   GEN_CPU and CPU 2 must be in its pool; its log stays its own; gnn@model tokens are refused (the
#   model is that process's environment). Its ring archives are copied into each run dir.
set -u
SYS=$1; OUT=$2; REPS=$3; EXTRA=$4; TRACE=${5:-}
M=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)
. "$M/src/test/host_layout.sh"
B=${DSB_ROOT:-$M/benchmarks}
PORT=${PORT:-8080}
export DSB_ROOT=$B
case $SYS in
  hotel) WD=$B/deathstar-hotelReservations; OP=$WD/bin/loadgen; PFX=hotel-; TR="hotel-frontend hotel-search hotel-geo hotel-profile hotel-rate";;
  social) WD=$B/deathstar-socialNetwork; OP=$WD/goperate/bin/goperate; PFX=social-; TR="social-ComposePostService social-HomeTimelineService social-UserTimelineService social-PostStorageService social-SocialGraphService";;
  media) WD=$B/deathstar-mediaMicroservices; OP=$WD/goperate/bin/mediaoperate; PFX=media-; TR="media-PageService media-ComposeReviewService media-MovieReviewService media-MovieInfoService media-ReviewStorageService";;
esac
case $SYS in
  media) MONGO_DIR=$B/runtime/mongodb/27018/db; REDIS_DUMP=$WD/run/data/redis/dump.rdb;;
  *) MONGO_DIR=$B/runtime/mongodb/27017/db; REDIS_DUMP=$B/runtime/redis/6379/dump.rdb;;
esac
OUT=$(realpath -m $OUT); RUN=$OUT/$(date +%Y%m%d-%H%M%S); mkdir -p $RUN
killcg() { for c in /sys/fs/cgroup/mercury/$PFX*; do [ -d $c ] && echo 1 | sudo -n tee $c/cgroup.kill >/dev/null; done; sleep 1; for c in /sys/fs/cgroup/mercury/$PFX*; do [ -d $c ] && sudo -n rmdir $c 2>/dev/null; done; }
cleanup() { killcg; [ -n "${SPID:-}" ] && kill $SPID 2>/dev/null; [ -n "${SCPID:-}" ] && kill -TERM $SCPID 2>/dev/null; [ -n "${MPID:-}" ] && sudo -n kill -TERM $MPID 2>/dev/null; sleep 2; }
trap cleanup EXIT
if [ -n "${EXTERNAL:-}" ]; then
  curl -s -m 2 127.0.0.1:$PORT/ping | grep -q pong || { echo "no mercury answering on port $PORT"; exit 1; }
  XPID=$(sudo -n ss -ltnpH "sport = :$PORT" | grep -o 'pid=[0-9]*' | head -1 | cut -d= -f2)
  XCWD=$(sudo -n readlink /proc/$XPID/cwd)
  XRINGS=$(sudo -n cat /proc/$XPID/environ | tr '\0' '\n' | sed -n 's/^MERCURY_RINGS=//p')
  XRINGS=${XRINGS:-rings}; case $XRINGS in /*) ;; *) XRINGS=$XCWD/$XRINGS;; esac
  ALLOC=$(curl -s -m 2 127.0.0.1:$PORT/map | grep -o '"allocatable":\[[0-9,]*\]' | head -1)
  for c in 2 $GEN_CPU; do echo "$ALLOC" | grep -qE "[\[,]$c[],]" || { echo "cpu $c is not in the pool of mercury on $PORT ($ALLOC)"; exit 1; }; done
  case " ${POLICIES:-} " in *@*) echo "EXTERNAL=1 cannot switch gnn models per run"; exit 1;; esac
  echo "using mercury pid $XPID on port $PORT, rings in $XRINGS"
fi
# POLICIES: space-separated list; gnn-nopin = gnn with -pin=false. Default "saturate gnn".
#   A token may carry a model: gnn@/abs/model.json or gnn-nopin@/abs/model.json (MERCURY_GNN_MODEL for
#   that run's mercury; the run dir is named <policy>-<model file stem>).
# TRACEALL=MS: threadtrace every $PFX daemon at MS ms in every run, from the load generator start
#   (TRACE_SLOW daemons at 50 ms). TRACE_ONLY="hotel-geo hotel-rate": trace only these daemons.
# SVC_ARGS: extra operator args per run appended verbatim (e.g. "-svc-min 4 -svc-max 4").
# CPUSTAT=1: per-CPU rings (rings/cpu-<n>.mring) and rings/placement.jsonl (src/cpustat.hpp), for interference;
#   CPUSTAT_MS [50] sampling period, CPUSTAT_CPU [SAMPLE_CPU] the sampler's CPU (off the operator's and Mercury's).
# SIDECAR="cmd args": started once the services are up (the load generator's cgroup exists) with --run-dir D
#   --prefix PFX --port PORT appended, on SAMPLE_CPU, output in D/sidecar.log; SIGTERM when the operator exits.
# MERCURY_REUSEPORT=1: passed into Mercury so several processes of one service can share its port (SPEC.md "LLM interface").
# POOL_POLICY="l3,start=8,..." : Mercury's l3 pool from its start (MERCURY_POOL, SPEC.md "l3 pool"); autoscaling
#   hands out CPUs only from the pool, which gains whole L3 domains when it is busy.
# FRESH_DB=1: before every run, delete MongoDB's data directory ($B/runtime/mongodb/27017/db) and redis's dump.rdb, so
#   every run starts from zero: mongod starts empty and the services fill the hotel databases with their initial data
#   (the operator also drops the hotel databases once mongod is up). Refuses while a mongod runs; overrides SEED.
#   Only for a Benchmarks tree of these experiments (DSB_ROOT), never one whose data matters.
# SEED=DIR: before every run, restore a dsb_seed.sh snapshot (MongoDB's data directory and redis's dump.rdb), so every
#   run starts from the same filled state. Refuses while a mongod runs.
for r in $(seq 1 $REPS); do for pol in ${POLICIES:-saturate gnn}; do
  MODEL=""; NAME=$pol
  case $pol in *@*) MODEL=${pol#*@}; pol=${pol%%@*}; NAME=$pol-$(basename $MODEL .json);; esac
  case $pol in gnn-nopin) PARGS="-policy gnn -pin=false";; *) PARGS="-policy $pol";; esac
  D=$RUN/$r-$NAME; mkdir -p $D
  if [ -n "${FRESH_DB:-}" ]; then
    pgrep -x mongod >/dev/null && { echo "mongod is running: cannot start from an empty database"; exit 1; }
    sudo -n rm -rf $MONGO_DIR
    sudo -n rm -f $REDIS_DUMP
    mkdir -p $MONGO_DIR
    echo "fresh: empty database $MONGO_DIR $(date -Is)" > $D/seed.txt
  elif [ -n "${SEED:-}" ]; then
    pgrep -x mongod >/dev/null && { echo "mongod is running: cannot restore SEED"; exit 1; }
    [ -d "$SEED/mongodb" ] || { echo "SEED $SEED has no mongodb/"; exit 1; }
    sudo -n mkdir -p $MONGO_DIR $(dirname $REDIS_DUMP)
    sudo -n rsync -a --delete "$SEED/mongodb/" $MONGO_DIR/
    if [ -f "$SEED/redis/dump.rdb" ]; then sudo -n cp -p "$SEED/redis/dump.rdb" $REDIS_DUMP
    else sudo -n rm -f $REDIS_DUMP; fi
    echo "seed $SEED restored $(date -Is)" > $D/seed.txt
  fi
  if [ -n "${EXTERNAL:-}" ]; then
    echo "external mercury pid $XPID port $PORT from $(date +%T); its log is its own output" > $D/mercury.log
  else
    ss -ltnH | grep -q ":$PORT " && { echo "port $PORT busy"; exit 1; }
    (cd $WD && exec sudo -n env MERCURY_RINGS=$D/rings ${MODEL:+MERCURY_GNN_MODEL=$MODEL} ${POOL_POLICY:+MERCURY_POOL=$POOL_POLICY} ${MERCURY_REUSEPORT:+MERCURY_REUSEPORT=$MERCURY_REUSEPORT} ${CPUSTAT:+MERCURY_CPUSTAT=$CPUSTAT MERCURY_CPUSTAT_MS=${CPUSTAT_MS:-50} MERCURY_CPUSTAT_CPU=${CPUSTAT_CPU:-$SAMPLE_CPU}} $M/build/mercury $PORT 0 $POOL) > $D/mercury.log 2>&1 &
    sleep 1; MPID=$(pgrep -f "^$M/build/mercury $PORT 0" | head -1)
    for i in $(seq 50); do curl -s -m 1 127.0.0.1:$PORT/ping >/dev/null && break; sleep 0.2; done
  fi
  taskset -c $SAMPLE_CPU python3 $M/src/test/dsb_sample.py --prefix $PFX --out $D/sample.jsonl & SPID=$!
  if [ "$TRACE" = --trace ] && [ $r = 1 ] && [ $pol = saturate ] && [ -z "${NOTRACE:-}" ]; then
    ( sleep 20; for d in $TR; do taskset -c ${TRACE_CPUS%%,*} sudo -n $M/build/threadtrace $d --interval-ms 50 --duration-s 240 --out $D/trace-$d.jsonl 2>/dev/null & done; wait ) &
  fi
  if [ -n "${TRACEALL:-}" ]; then
    ( for i in $(seq 300); do [ -d /sys/fs/cgroup/mercury/${PFX}loadgen ] && break; sleep 1; done  # services are all up once the load generator starts
      for c in /sys/fs/cgroup/mercury/$PFX*; do d=$(basename $c); iv=$TRACEALL
        [ -n "${TRACE_ONLY:-}" ] && case " $TRACE_ONLY " in *" $d "*) ;; *) continue;; esac
        case " ${TRACE_SLOW:-} " in *" $d "*) iv=50;; esac
        taskset -c $TRACE_CPUS sudo -n $M/build/threadtrace $d --interval-ms $iv --duration-s 900 --out $D/trace-$d.jsonl 2>/dev/null & done; wait ) &
  fi
  if [ -n "${SIDECAR:-}" ]; then
    ( for i in $(seq 300); do [ -d /sys/fs/cgroup/mercury/${PFX}loadgen ] && break; sleep 1; done
      exec taskset -c $SAMPLE_CPU $SIDECAR --run-dir $D --prefix $PFX --port $PORT ) > $D/sidecar.log 2>&1 & SCPID=$!
  fi
  echo "$(date +%T) run $r $NAME"
  T0=$(python3 -c 'import time; print(time.monotonic_ns())')
  (cd $WD && taskset -c $OP_CPU $OP -mercury 127.0.0.1:$PORT -cpu 2 -gen-cpu $GEN_CPU -client mercury $PARGS -placement least_loaded -gen-out $D/loadgen.out $EXTRA ${SVC_ARGS:-} autoscale) > $D/operate.out 2> $D/operate.err
  echo "  operator exit $? routes=$(grep -c ^route $D/operate.out)"
  T1=$(python3 -c 'import time; print(time.monotonic_ns())')
  kill $SPID; SPID=""
  [ -n "${SCPID:-}" ] && { kill -TERM $SCPID 2>/dev/null; wait $SCPID 2>/dev/null; SCPID=""; }
  if [ -n "${MPID:-}" ]; then
    sudo -n kill -TERM $MPID; for i in $(seq 50); do kill -0 $MPID 2>/dev/null || break; sleep 0.2; done; MPID=""
  fi
  sudo -n pkill -x threadtrace 2>/dev/null
  killcg
  # The external mercury appends every run to the same archives; plot_rings.py keeps the newest run.
  [ -n "${EXTERNAL:-}" ] && { mkdir -p $D/rings; sudo -n cp $XRINGS/$PFX*.mring $D/rings/ 2>/dev/null; sudo -n chown -R $(id -u):$(id -g) $D/rings; }
  # 10 ms cores-vs-use per daemon from this run's ring archives (tools/plots/rings/plot_rings.py).
  python3 $M/tools/plots/rings/plot_rings.py $D/rings --prefix $PFX --since-ns $T0 --until-ns $T1 --out $D/alloc_vs_use.png > $D/alloc_vs_use.txt 2>&1 \
    && echo "  $(tail -1 $D/alloc_vs_use.txt)"
  echo "  numa_balancing=$(cat /proc/sys/kernel/numa_balancing)"
  sleep 3
done; done
echo "done: $RUN"
