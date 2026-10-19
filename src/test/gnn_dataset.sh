#!/usr/bin/env bash
# gnn policy phase 1 (SPEC.md): run a matrix of gnnload workloads under Mercury with saturate
# (the baseline policy, attached at start) and record per-thread traces plus ground-truth labels.
# Runs its own mercury on the CPUs of NUMA nodes 0-2 (worker on the lowest) on a free port in
# 18180-18199, like saturate_it.sh. Re-executes itself with sudo -n (cgroups, perf on root daemons).
#
# Flags (defaults keep the whole run to about 3 minutes):
#   --out DIR          dataset root (default $ROOT/data/gnn); the run goes to DIR/<YYYYmmdd-HHMMSS>
#   --patterns "..."   default "uniform hot geometric bursty ramp"
#   --threads "..."    default "4 8"
#   --totals "..."     total demand in cores, default "2.5"
#   --duration S       seconds per case (default 15)
#   --interval-ms N    threadtrace tick (default 1000)
#   --phase-ms N       gnnload phase length (default 1000)
#   --ramp-s S         gnnload ramp period (default = duration)
#   --placement P      core hand-out for both policies (least_loaded | nearest_first; default least_loaded)
#   --max N            /scale max cores (default 16)
#   --workload W       gnnload (default) | stressng. stressng: --threads are worker counts, --patterns are
#                      steps (20/50/80/100/50/20% at --step-s each) | ramp (10->100% over --ramp-s, 1 s steps) |
#                      vm (--vm N --vm-bytes 1G) | stream (--stream N); --totals is ignored. Labels: per-step
#                      tids, demand = cpu-load/100 (wrappers/stressng). result.json has bogo-ops per step.
#   --step-s S         stressng steps length (default 3)
#   --fixed-cpus SPEC  hold the cpuset fixed at SPEC (policy none, POST /cpus) instead of scaling
#   --policy P         saturate (default) or gnn (posted to /scale after start; gnn uses nearest_first)
#   --tick-ms N        gnn decision tick (default 10)
#   --seed-base N      gnnload GNN_SEED = N + case index (default 0; v4 DAgger rounds use distinct bases)
#   --no-validate      skip gnn_validate.py
#   --trace-cpu N      CPU threadtrace is pinned to (default: highest online CPU, outside the share)
# Layout: DIR/<run>/{meta.txt,mercury.log,validation.txt,validation.json}
#         DIR/<run>/<pattern>-t<threads>-c<total>/{trace.jsonl,labels.csv,case.txt}
set -u
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)
if [ "$(id -u)" != 0 ]; then
  exec sudo -n env SUDO_USER="${USER:-}" MERCURY_GNN_MODEL="${MERCURY_GNN_MODEL:-}" MERCURY_GNN_HOLD_TICKS="${MERCURY_GNN_HOLD_TICKS:-}" MERCURY_GNN_STACK_K="${MERCURY_GNN_STACK_K:-}" "$(readlink -f "$0")" "$@"
fi
BIN=${MERCURY_BIN:-$ROOT/build/mercury}
TRACE=${THREADTRACE_BIN:-$ROOT/build/threadtrace}
LOAD=${GNNLOAD_BIN:-$ROOT/build/test/gnnload}
SNG=$ROOT/src/test/wrappers/stressng
OUT=$ROOT/data/gnn; PATTERNS="uniform hot geometric bursty ramp"; THREADS="4 8"; TOTALS="2.5"
DUR=15; INTERVAL=1000; PHASE=1000; RAMP=""; PLACEMENT=""; VALIDATE=1; TRACE_CPU=""; POLICY=saturate; TICK=10; MAXC=16; WORKLOAD=gnnload; STEP_S=3; FIXED=""; SEEDBASE=0
while [ $# -gt 0 ]; do
  case "$1" in
    --out) OUT=$2; shift 2;; --patterns) PATTERNS=$2; shift 2;; --threads) THREADS=$2; shift 2;;
    --totals) TOTALS=$2; shift 2;; --duration) DUR=$2; shift 2;; --interval-ms) INTERVAL=$2; shift 2;;
    --phase-ms) PHASE=$2; shift 2;; --ramp-s) RAMP=$2; shift 2;; --placement) PLACEMENT=$2; shift 2;;
    --no-validate) VALIDATE=0; shift;; --trace-cpu) TRACE_CPU=$2; shift 2;; --policy) POLICY=$2; shift 2;; --tick-ms) TICK=$2; shift 2;;
    --max) MAXC=$2; shift 2;; --workload) WORKLOAD=$2; shift 2;; --step-s) STEP_S=$2; shift 2;; --fixed-cpus) FIXED=$2; shift 2;; --seed-base) SEEDBASE=$2; shift 2;;
    *) echo "unknown flag $1"; exit 2;;
  esac
done
RAMP=${RAMP:-$DUR}
PLACEMENT=${PLACEMENT:-least_loaded}
[ "$WORKLOAD" = stressng ] && TOTALS=x
for b in "$BIN" "$TRACE" "$LOAD"; do [ -x "$b" ] || { echo "missing $b (cmake --build build)"; exit 1; }; done
expand() { python3 -c 'import sys
out=[]
for p in sys.argv[1].strip().split(","):
    if not p: continue
    a,_,b=p.partition("-"); out+=range(int(a),int(b or a)+1)
print(" ".join(map(str,sorted(out))))' "$1"; }
SHARE="$(expand "$(cat /sys/devices/system/node/node0/cpulist)") $(expand "$(cat /sys/devices/system/node/node1/cpulist)") $(expand "$(cat /sys/devices/system/node/node2/cpulist)")"
TRACE_CPU=${TRACE_CPU:-$(expand "$(cat /sys/devices/system/cpu/online)" | tr " " "\n" | tail -1)}
FIRST=$(echo $SHARE | tr ' ' '\n' | sort -n | sed -n 2p)
for p in $(seq 18180 18199); do ss -ltnH | awk '{print $4}' | grep -q ":$p\$" || { PORT=$p; break; }; done
[ -n "${PORT:-}" ] || { echo "no free port in 18180-18199"; exit 1; }
RUN=$OUT/$(date +%Y%m%d-%H%M%S); mkdir -p "$RUN"; RINGS=$(mktemp -d /tmp/mercury-gnn-XXXXXX)
URL=http://127.0.0.1:$PORT; TAG="gnn$$"; CUR=""; TPID=""
cleanup() {
  [ -n "$TPID" ] && kill $TPID 2>/dev/null
  if [ -n "$CUR" ]; then curl -s -m 10 -X POST -d "{\"daemon_id\":\"$CUR\"}" $URL/kill >/dev/null 2>&1; fi
  [ -n "${MPID:-}" ] && kill $MPID 2>/dev/null && wait $MPID 2>/dev/null
  [ -n "$CUR" ] && rmdir /sys/fs/cgroup/mercury/$CUR 2>/dev/null
  rm -rf "$RINGS"
  [ -n "${SUDO_USER:-}" ] && [ "$SUDO_USER" != root ] && chown -R "$SUDO_USER": "$OUT" 2>/dev/null
}
trap cleanup EXIT
{
  echo "date=$(date -Iseconds) host=$(hostname) kernel=$(uname -r)"
  echo "share=$SHARE first=$FIRST port=$PORT policy=$POLICY tick_ms=$TICK placement=${PLACEMENT:-default} trace_cpu=$TRACE_CPU"
  echo "patterns=$PATTERNS threads=$THREADS totals=$TOTALS duration_s=$DUR interval_ms=$INTERVAL phase_ms=$PHASE ramp_s=$RAMP seed_base=$SEEDBASE"
  echo "gnn_model=${MERCURY_GNN_MODEL:-default} hold_ticks=${MERCURY_GNN_HOLD_TICKS:-default} stack_k=${MERCURY_GNN_STACK_K:-default}"
  echo "numa_balancing=$(cat /proc/sys/kernel/numa_balancing 2>/dev/null) (mercury sets 0 while it runs)"
} > "$RUN/meta.txt"
echo "gnn dataset -> $RUN (mercury port $PORT)"
MERCURY_RINGS=$RINGS "$BIN" $PORT $SHARE >"$RUN/mercury.log" 2>&1 &
MPID=$!
for i in $(seq 50); do curl -s -m 1 $URL/map >/dev/null 2>&1 && break; sleep 0.2; done
n=0
for T in $THREADS; do for C in $TOTALS; do for P in $PATTERNS; do
  n=$((n+1)); CASE="$P-t$T-c$C"; DIR=$RUN/$CASE; mkdir -p "$DIR"; CUR="$TAG-$n"
  if [ "$WORKLOAD" = stressng ]; then
    CASE="$P-n$T"; DIR=$RUN/$CASE; mkdir -p "$DIR"; EXE=$SNG
    case "$P" in
      steps) X="\"STRESS_STEPS\":\"20:$STEP_S,50:$STEP_S,80:$STEP_S,100:$STEP_S,50:$STEP_S,20:$STEP_S\""; CDUR=$(python3 -c "print(int(6*$STEP_S+3))");;
      ramp) X="\"STRESS_RAMP\":\"10,100,$RAMP\""; CDUR=$(python3 -c "print(int($RAMP*1.1+3))");;
      vm|stream) X="\"STRESS_MODE\":\"$P\",\"STRESS_SECONDS\":\"$DUR\""; CDUR=$((DUR+3));;
      *) echo "unknown stressng pattern $P"; exit 2;;
    esac
    ENV="{\"STRESS_WORKERS\":\"$T\",$X,\"STRESS_LABELS\":\"$DIR/labels.csv\",\"STRESS_RESULT\":\"$DIR/result.json\",\"SUDO_USER\":\"${SUDO_USER:-}\"}"
  else
  EXE=$LOAD; CDUR=$DUR
  ENV="{\"GNN_THREADS\":\"$T\",\"GNN_PATTERN\":\"$P\",\"GNN_TOTAL\":\"$C\",\"GNN_PHASE_MS\":\"$PHASE\",\"GNN_RAMP_S\":\"$RAMP\",\"GNN_SEED\":\"$((SEEDBASE+n))\",\"GNN_LABELS\":\"$DIR/labels.csv\"}"
  fi
  code=$(curl -s -o "$DIR/start.json" -w '%{http_code}' -X POST -d "{\"daemon_id\":\"$CUR\",\"exec_path\":\"$EXE\",\"cpu\":$FIRST,\"env\":$ENV}" $URL/start)
  if [ "$code" != 200 ]; then echo "  $CASE: start failed ($code)"; CUR=""; continue; fi
  if [ -n "$FIXED" ]; then
    curl -s -m 10 -o "$DIR/scale.json" -X POST -d "{\"daemon_id\":\"$CUR\",\"policy\":\"none\"}" $URL/scale
    curl -s -m 10 -o "$DIR/cpus.json" -X POST -d "{\"daemon_id\":\"$CUR\",\"cpus\":\"$FIXED\"}" $URL/cpus
  else
    curl -s -m 10 -o "$DIR/scale.json" -X POST -d "{\"daemon_id\":\"$CUR\",\"policy\":\"$POLICY\",\"placement\":\"$PLACEMENT\",\"min\":1,\"max\":$MAXC,\"tick_ms\":$TICK}" $URL/scale
  fi
  for i in $(seq 50); do [ -n "$(cat /sys/fs/cgroup/mercury/$CUR/cgroup.threads 2>/dev/null)" ] && break; sleep 0.1; done  # sysfs sizes read 0, so test content
  for f in /sys/devices/system/node/node*/numastat; do echo "$f $(tr "\n" " " < $f)"; done > "$DIR/numastat.before"
  taskset -c "$TRACE_CPU" "$TRACE" "$CUR" --interval-ms "$INTERVAL" --duration-s "$CDUR" --out "$DIR/trace.jsonl" 2>"$DIR/threadtrace.log" &
  TPID=$!
  wait $TPID; TPID=""
  for f in /sys/devices/system/node/node*/numastat; do echo "$f $(tr "\n" " " < $f)"; done > "$DIR/numastat.after"
  echo "case=$CASE daemon_id=$CUR threads=$T pattern=$P total=$C final_cpuset=$(cat /sys/fs/cgroup/mercury/$CUR/cpuset.cpus 2>/dev/null)" > "$DIR/case.txt"
  curl -s -m 10 -X POST -d "{\"daemon_id\":\"$CUR\"}" $URL/kill >/dev/null; sleep 0.3
  grep -a "daemon_id=$CUR " "$RUN/mercury.log" | grep -a "^gnn \|^scale " > "$DIR/controller.log" 2>/dev/null
  rmdir /sys/fs/cgroup/mercury/$CUR 2>/dev/null; CUR=""
  echo "  $CASE: $(wc -l < "$DIR/trace.jsonl") ticks, $(($(wc -l < "$DIR/labels.csv") - 2)) label rows, $(cut -d' ' -f6 "$DIR/case.txt")"
done; done; done
if [ "$VALIDATE" = 1 ]; then python3 "$ROOT/src/test/gnn_validate.py" "$RUN" | tee "$RUN/validation.txt"; fi
echo "done: $RUN ($(du -sh "$RUN" | cut -f1))"
