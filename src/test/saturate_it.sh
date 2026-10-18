#!/usr/bin/env bash
# Acceptance for saturate (SPEC.md Scaling): a spinner with N busy threads started on one CPU
# settles at N CPUs and holds there for ten 640 ms windows. Runs its own mercury on the CPUs of
# NUMA nodes 0-2 (worker on the lowest), on a free loopback port. Re-executes itself with sudo -n.
# Env: MERCURY_BIN, SPIN_BIN, PORT, COUNTS (default "1 3 6"), KEEP=1 keeps the log.
set -u
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)
BIN=${MERCURY_BIN:-$ROOT/build/mercury}
SPIN=${SPIN_BIN:-$ROOT/build/test/spinner}
if [ "$(id -u)" != 0 ]; then
  exec sudo -n env MERCURY_BIN="$BIN" SPIN_BIN="$SPIN" ${PORT:+PORT=$PORT} ${COUNTS:+COUNTS="$COUNTS"} ${KEEP:+KEEP=$KEEP} "$(readlink -f "$0")" "$@"
fi
expand() { python3 -c 'import sys
out=[]
for p in sys.argv[1].strip().split(","):
    if not p: continue
    a,_,b=p.partition("-"); out+=range(int(a),int(b or a)+1)
print(" ".join(map(str,sorted(out))))' "$1"; }
SHARE="$(expand "$(cat /sys/devices/system/node/node0/cpulist)") $(expand "$(cat /sys/devices/system/node/node1/cpulist)") $(expand "$(cat /sys/devices/system/node/node2/cpulist)")"
FIRST=$(echo $SHARE | tr ' ' '\n' | sort -n | sed -n 2p)
if [ -z "${PORT:-}" ]; then
  for p in $(seq 18180 18199); do ss -ltnH | awk '{print $4}' | grep -q ":$p\$" || { PORT=$p; break; }; done
fi
DIR=$(mktemp -d /tmp/mercury-saturate-XXXXXX); LOG=$DIR/mercury.log; URL=http://127.0.0.1:$PORT
TAG="st$$"; STARTED=(); PASS=0; FAIL=0
cleanup() {
  for id in "${STARTED[@]}"; do curl -s -m 10 -X POST -d "{\"daemon_id\":\"$id\"}" $URL/kill >/dev/null 2>&1; rmdir /sys/fs/cgroup/mercury/$id 2>/dev/null; done
  [ -n "${MPID:-}" ] && kill $MPID 2>/dev/null && wait $MPID 2>/dev/null
  for id in "${STARTED[@]}"; do rmdir /sys/fs/cgroup/mercury/$id 2>/dev/null; done
  if [ "$FAIL" != 0 ] || [ -n "${KEEP:-}" ]; then echo "kept $DIR"; else rm -rf "$DIR"; fi
}
trap cleanup EXIT
ok() { PASS=$((PASS+1)); echo "  ok   $*"; }
bad() { FAIL=$((FAIL+1)); echo "  FAIL $*"; }
count() { expand "$(cat /sys/fs/cgroup/mercury/$1/cpuset.cpus 2>/dev/null)" | wc -w; }
echo "mercury port $PORT share: $SHARE"
MERCURY_RINGS=$DIR "$BIN" $PORT $SHARE >"$LOG" 2>&1 &
MPID=$!
for i in $(seq 50); do curl -s -m 1 $URL/map >/dev/null 2>&1 && break; sleep 0.2; done
for N in ${COUNTS:-1 3 6}; do
  id=$TAG-n$N; STARTED+=("$id")
  t0=$(date +%s.%N)
  code=$(curl -s -o /dev/null -w '%{http_code}' -X POST -d "{\"daemon_id\":\"$id\",\"exec_path\":\"$SPIN\",\"cpu\":$FIRST,\"env\":{\"SPIN_THREADS\":\"$N\",\"SPIN_DUTY\":\"100\"}}" $URL/start)
  # SAT_PLACEMENT (least_loaded | nearest_first) picks saturate's CPU placement; default leaves mercury's.
  if [ -n "${SAT_PLACEMENT:-}" ] && [ "$code" = 200 ]; then curl -s -m 10 -X POST -d "{\"daemon_id\":\"$id\",\"placement\":\"$SAT_PLACEMENT\"}" $URL/scale >/dev/null; fi
  [ "$code" = 200 ] || { bad "start $id: $code"; continue; }
  settled=""
  for i in $(seq 240); do sleep 0.25; [ "$(count $id)" -ge "$N" ] && { settled=$(python3 -c "import time;print(round(time.time()-$t0,1))"); break; }; done
  if [ -z "$settled" ]; then bad "N=$N did not reach $N cpus (at $(count $id))"; continue; fi
  # Hold for ten windows of 640 ms plus one window of slack for a decision already in flight.
  lo=$N; hi=$N
  for i in $(seq 28); do sleep 0.25; c=$(count $id); [ $c -lt $lo ] && lo=$c; [ $c -gt $hi ] && hi=$c; done
  if [ $lo = $N ] && [ $hi = $N ]; then ok "N=$N settled at $N cpus after ${settled}s and held 10 windows ($(cat /sys/fs/cgroup/mercury/$id/cpuset.cpus))"
  else bad "N=$N reached $N after ${settled}s but moved to $lo..$hi cpus during the hold"; fi
  curl -s -m 10 -X POST -d "{\"daemon_id\":\"$id\"}" $URL/kill >/dev/null; rmdir /sys/fs/cgroup/mercury/$id 2>/dev/null
done
echo "saturate_it: $PASS passed, $FAIL failed"
[ "$FAIL" = 0 ]
