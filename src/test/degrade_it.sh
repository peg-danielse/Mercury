#!/usr/bin/env bash
# Integration test of degrade (SPEC.md "degrade") with spinner daemons. Root (cgroup writes, /proc environ).
# Runs its own mercury on CPU 0 with every other online CPU as the pool, on a free loopback port in 18180-18199,
# on a fixed 4-CPU pool. Checks: every daemon gets MERCURY_POLICY; while the pool is saturated degrade disables the
# routes in order, one per window, in that file; when the load is gone it enables them again, the last first.
# Env: MERCURY_BIN, SPIN_BIN, PORT, WINDOW_MS (default 500), KEEP=1 keeps the log.
set -u
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)
BIN=${MERCURY_BIN:-$ROOT/build/mercury}
SPIN=${SPIN_BIN:-$ROOT/build/test/spinner}
if [ "$(id -u)" != 0 ]; then
  exec sudo -n env MERCURY_BIN="$BIN" SPIN_BIN="$SPIN" ${PORT:+PORT=$PORT} ${WINDOW_MS:+WINDOW_MS=$WINDOW_MS} ${KEEP:+KEEP=$KEEP} "$(readlink -f "$0")" "$@"
fi
WINDOW_MS=${WINDOW_MS:-500}
if [ -z "${PORT:-}" ]; then
  for p in $(seq 18180 18199); do ss -ltnH | awk '{print $4}' | grep -q ":$p\$" || { PORT=$p; break; }; done
fi
NCPU=$(nproc --all)
DIR=$(mktemp -d /tmp/mercury-degrade-XXXXXX); LOG=$DIR/mercury.log; URL=http://127.0.0.1:$PORT
CG=/sys/fs/cgroup/mercury; IDS=""; PASS=0; FAIL=0
cleanup() {
  [ -n "${MPID:-}" ] && kill $MPID 2>/dev/null && wait $MPID 2>/dev/null
  for id in $IDS; do [ -d $CG/$id ] && { echo 1 > $CG/$id/cgroup.kill; sleep 0.2; rmdir $CG/$id; } 2>/dev/null; done
  if [ "$FAIL" != 0 ] || [ -n "${KEEP:-}" ]; then echo "kept $DIR"; else rm -rf "$DIR"; fi
}
trap cleanup EXIT
verdict() { if [ $? = 0 ]; then PASS=$((PASS+1)); echo "  ok   $1"; else FAIL=$((FAIL+1)); echo "  FAIL $1"; fi; }
post() { curl -s -m 10 -X POST -d "$2" $URL/$1; }
code() { curl -s -m 10 -o /dev/null -w '%{http_code}' -X POST -d "$2" $URL/$1; }
spin() {  # spin ID CPU THREADS: saturate attached at start
  IDS="$IDS $1"
  post start "{\"daemon_id\":\"$1\",\"exec_path\":\"$SPIN\",\"cpu\":$2,\"env\":{\"SPIN_THREADS\":\"$3\",\"SPIN_DUTY\":\"100\"}}" | sed -n 's/.*"pid":\([0-9]*\).*/\1/p'
}
disabled() { python3 -c "import json; print(','.join(json.load(open('$DIR/front.policy'))['disabled']))" 2>/dev/null; }
windows() { sleep $(awk "BEGIN{print $1*$WINDOW_MS/1000}"); }

MERCURY_RINGS=$DIR MERCURY_POOL="fixed,start=4,window_ms=$WINDOW_MS" "$BIN" $PORT 0 $(seq 1 $((NCPU - 1))) >"$LOG" 2>&1 &
MPID=$!
for i in $(seq 50); do curl -s -m 1 $URL/map >/dev/null 2>&1 && break; sleep 0.1; done
echo "mercury port $PORT pid $MPID, $NCPU CPUs, fixed 4-CPU pool, window ${WINDOW_MS} ms"
sleep 0.5

FPID=$(spin front 2 1)
sleep 0.3
tr '\0' '\n' < /proc/$FPID/environ | grep -qx "MERCURY_POLICY=$DIR/front.policy"; verdict "a daemon gets MERCURY_POLICY=$DIR/front.policy"
[ "$(code degrade '{"daemon_id":"front","routes":"a,b","above":1.5}')" = 400 ]; verdict "degrade rejects above outside (0, 1)"
[ "$(code degrade '{"daemon_id":"front"}')" = 400 ]; verdict "degrade needs routes"
post degrade "{\"daemon_id\":\"front\",\"routes\":\"a,b,c\",\"window_ms\":$WINDOW_MS,\"hold\":2}" | grep -q '"enabled":true'
verdict "degrade attaches to front"
[ "$(disabled)" = "" ]; verdict "the policy file starts with nothing disabled"

# Saturate the 4-CPU pool: the routes go, a then b then c, one per window. The hog holds the whole pool directly
# (saturate would stop short of it: a CPU it shares with front lowers its occupancy).
spin hog 3 16 >/dev/null
post scale '{"daemon_id":"hog","policy":"none"}' >/dev/null
post cpus "{\"daemon_id\":\"hog\",\"cpus\":\"$(curl -s $URL/pool | python3 -c 'import json,sys; print(json.load(sys.stdin)["cpus"])')\"}" >/dev/null
seen=""
for i in $(seq 40); do
  windows 1
  now=$(disabled)
  [ "$now" != "${seen##*|}" ] && seen="$seen|$now"
  [ "$now" = "a,b,c" ] && break
done
[ "$(disabled)" = "a,b,c" ]; verdict "under saturation every route is disabled"
echo "$seen" | grep -q "|a|a,b|a,b,c"; verdict "one at a time, in order (${seen#|})"

# The load goes: the routes come back, c first.
post kill '{"daemon_id":"hog"}' >/dev/null
seen=""
for i in $(seq 80); do
  windows 1
  now=$(disabled)
  [ "$now" != "${seen##*|}" ] && seen="$seen|$now"
  [ -z "$now" ] && break
done
[ "$(disabled)" = "" ]; verdict "without the load every route comes back"
echo "$seen" | grep -q "a,b|a|"; verdict "the last disabled first (${seen#|})"
[ "$(grep -c '^degrade daemon_id=front \(disabled\|enabled\) ' "$LOG")" = 6 ]; verdict "six logged steps"
post degrade '{"daemon_id":"front","enabled":false}' | grep -q '"enabled":false'; verdict "enabled false detaches"
post kill '{"daemon_id":"front"}' >/dev/null

echo "--- degrade steps"; grep "^degrade daemon_id" "$LOG" | sed 's/^/  /'
echo "degrade_it: $PASS passed, $FAIL failed"
[ "$FAIL" = 0 ]
