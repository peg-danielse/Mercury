#!/bin/bash
# DSB closed-loop A/B under Mercury: alternating saturate/gnn repeats with a DSB operator.
#   dsb_ab.sh hotel|social OUT REPEATS "operator extra args" [--trace]
# Starts its own mercury (port 8080, pool CPUs 1-60, worker CPU 0) per run, the operator's autoscale
# (which starts and kills every service), and dsb_sample.py on CPU 63. Stops mercury with SIGTERM.
# The defaults are dtn-01's layout. Another host overrides them, e.g. a 32-CPU laptop:
#   DSB_ROOT=~/Documents/Benchmarks POOL="$(seq 1 29)" GEN_CPU=29 OP_CPU=30 TRACE_CPUS=31 SAMPLE_CPU=31
# DSB_ROOT: Benchmarks checkout (default /home/paul/Benchmarks); Mercury is this script's repository.
# POOL: Mercury's daemon CPUs (default 1-60; Mercury itself takes CPU 0). GEN_CPU: the loadgen home
# CPU, in POOL (default 60). OP_CPU: the operator (61). TRACE_CPUS: threadtrace (62,63). SAMPLE_CPU:
# dsb_sample.py (63). The last three stay outside POOL. PORT: Mercury's port (default 8080).
# Each run dir gets rings/ (Mercury's 10 ms archives) and alloc_vs_use.{png,txt} from tools/plot_rings.py.
set -u
SYS=$1; OUT=$2; REPS=$3; EXTRA=$4; TRACE=${5:-}
B=${DSB_ROOT:-/home/paul/Benchmarks}; M=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)
POOL=${POOL:-$(seq 1 60)}; GEN_CPU=${GEN_CPU:-60}; OP_CPU=${OP_CPU:-61}; TRACE_CPUS=${TRACE_CPUS:-62,63}; SAMPLE_CPU=${SAMPLE_CPU:-63}; PORT=${PORT:-8080}
export DSB_ROOT=$B
case $SYS in
  hotel) WD=$B/deathstar-hotelReservations; OP=$WD/bin/loadgen; PFX=hotel-; TR="hotel-frontend hotel-search hotel-geo hotel-profile hotel-rate";;
  social) WD=$B/deathstar-socialNetwork; OP=$WD/goperate/bin/goperate; PFX=social-; TR="social-ComposePostService social-HomeTimelineService social-UserTimelineService social-PostStorageService social-SocialGraphService";;
esac
OUT=$(realpath -m $OUT); RUN=$OUT/$(date +%Y%m%d-%H%M%S); mkdir -p $RUN
killcg() { for c in /sys/fs/cgroup/mercury/$PFX*; do [ -d $c ] && echo 1 | sudo -n tee $c/cgroup.kill >/dev/null; done; sleep 1; for c in /sys/fs/cgroup/mercury/$PFX*; do [ -d $c ] && sudo -n rmdir $c 2>/dev/null; done; }
cleanup() { killcg; [ -n "${SPID:-}" ] && kill $SPID 2>/dev/null; [ -n "${MPID:-}" ] && sudo -n kill -TERM $MPID 2>/dev/null; sleep 2; }
trap cleanup EXIT
# POLICIES: space-separated list; gnn-nopin = gnn with -pin=false. Default "saturate gnn".
#   A token may carry a model: gnn@/abs/model.json or gnn-nopin@/abs/model.json (MERCURY_GNN_MODEL for
#   that run's mercury; the run dir is named <policy>-<model file stem>).
# TRACEALL=MS: threadtrace every $PFX daemon at MS ms in every run, from the load generator start
#   (TRACE_SLOW daemons at 50 ms).
# SVC_ARGS: extra operator args per run appended verbatim (e.g. "-svc-min 4 -svc-max 4").
for r in $(seq 1 $REPS); do for pol in ${POLICIES:-saturate gnn}; do
  MODEL=""; NAME=$pol
  case $pol in *@*) MODEL=${pol#*@}; pol=${pol%%@*}; NAME=$pol-$(basename $MODEL .json);; esac
  case $pol in gnn-nopin) PARGS="-policy gnn -pin=false";; *) PARGS="-policy $pol";; esac
  D=$RUN/$r-$NAME; mkdir -p $D
  ss -ltnH | grep -q ":$PORT " && { echo "port $PORT busy"; exit 1; }
  (cd $WD && exec sudo -n env MERCURY_RINGS=$D/rings ${MODEL:+MERCURY_GNN_MODEL=$MODEL} $M/build/mercury $PORT 0 $POOL) > $D/mercury.log 2>&1 &
  sleep 1; MPID=$(pgrep -f "^$M/build/mercury $PORT 0" | head -1)
  for i in $(seq 50); do curl -s -m 1 127.0.0.1:$PORT/ping >/dev/null && break; sleep 0.2; done
  taskset -c $SAMPLE_CPU python3 $M/src/test/dsb_sample.py --prefix $PFX --out $D/sample.jsonl & SPID=$!
  if [ "$TRACE" = --trace ] && [ $r = 1 ] && [ $pol = saturate ] && [ -z "${NOTRACE:-}" ]; then
    ( sleep 20; for d in $TR; do taskset -c ${TRACE_CPUS%%,*} sudo -n $M/build/threadtrace $d --interval-ms 50 --duration-s 240 --out $D/trace-$d.jsonl 2>/dev/null & done; wait ) &
  fi
  if [ -n "${TRACEALL:-}" ]; then
    ( for i in $(seq 300); do [ -d /sys/fs/cgroup/mercury/${PFX}loadgen ] && break; sleep 1; done  # services are all up once the load generator starts
      for c in /sys/fs/cgroup/mercury/$PFX*; do d=$(basename $c); iv=$TRACEALL
        case " ${TRACE_SLOW:-} " in *" $d "*) iv=50;; esac
        taskset -c $TRACE_CPUS sudo -n $M/build/threadtrace $d --interval-ms $iv --duration-s 900 --out $D/trace-$d.jsonl 2>/dev/null & done; wait ) &
  fi
  echo "$(date +%T) run $r $NAME"
  (cd $WD && taskset -c $OP_CPU $OP -mercury 127.0.0.1:$PORT -cpu 2 -gen-cpu $GEN_CPU -client mercury $PARGS -placement least_loaded -gen-out $D/loadgen.out $EXTRA ${SVC_ARGS:-} autoscale) > $D/operate.out 2> $D/operate.err
  echo "  operator exit $? routes=$(grep -c ^route $D/operate.out)"
  kill $SPID; SPID=""
  sudo -n kill -TERM $MPID; for i in $(seq 50); do kill -0 $MPID 2>/dev/null || break; sleep 0.2; done; MPID=""
  sudo -n pkill -x threadtrace 2>/dev/null
  killcg
  # 10 ms cores-vs-use per daemon from this run's ring archives (tools/plot_rings.py).
  python3 $M/tools/plot_rings.py $D/rings --prefix $PFX --out $D/alloc_vs_use.png > $D/alloc_vs_use.txt 2>&1 \
    && echo "  $(tail -1 $D/alloc_vs_use.txt)"
  echo "  numa_balancing=$(cat /proc/sys/kernel/numa_balancing)"
  sleep 3
done; done
echo "done: $RUN"
