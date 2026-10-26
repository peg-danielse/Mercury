#!/usr/bin/env bash
# Integration test of throttle (SPEC.md "throttle") with spinner daemons. Root (cgroup writes).
# Runs its own mercury on CPU 0 with every other online CPU as the pool (needs at least 14 CPUs), on
# a free loopback port in 18180-18199. Throttle judges the CPUs of all Mercury's daemons. A daemon with
# 4 busy threads on CPUs 1-4 is throttled, beside a near-idle daemon on CPUs 5-12:
# - the daemons' CPUs below the line (4 of 12 busy): no cut;
# - a hog daemon on every CPU from 5 takes them above it: th's cpu.max is cut by 75%, and its
#   measured use falls to about one CPU;
# - the hog still running: the release test keeps the cut (no flapping);
# - the hog killed: the cut is lifted. Detach and bad parameters are checked too.
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
NCPU=$(nproc --all); LAST=$((NCPU - 1))
[ "$NCPU" -ge 14 ] || { echo "throttle_it needs at least 14 CPUs"; exit 1; }
DIR=$(mktemp -d /tmp/mercury-throttle-XXXXXX); LOG=$DIR/mercury.log; URL=http://127.0.0.1:$PORT
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
cpu_max() { cat $CG/$1/cpu.max 2>/dev/null; }
# Mean CPUs used by the daemon cgroup over one second, from cpu.stat usage_usec.
used() {
  local a b; a=$(awk '/usage_usec/{print $2}' $CG/$1/cpu.stat); sleep 1; b=$(awk '/usage_usec/{print $2}' $CG/$1/cpu.stat)
  awk "BEGIN{printf \"%.2f\", ($b - $a) / 1e6}"
}
spin() {  # spin ID CPUS THREADS [DUTY]: fixed cpuset, no scaling policy
  IDS="$IDS $1"
  post start "{\"daemon_id\":\"$1\",\"exec_path\":\"$SPIN\",\"cpu\":${2%%-*},\"env\":{\"SPIN_THREADS\":\"$3\",\"SPIN_DUTY\":\"${4:-100}\"}}" >/dev/null
  post scale "{\"daemon_id\":\"$1\",\"policy\":\"none\"}" >/dev/null
  post cpus "{\"daemon_id\":\"$1\",\"cpus\":\"$2\"}" >/dev/null
}
windows() { sleep $(awk "BEGIN{print $1*$WINDOW_MS/1000}"); }

MERCURY_RINGS=$DIR "$BIN" $PORT 0 $(seq 1 $LAST) >"$LOG" 2>&1 &
MPID=$!
for i in $(seq 50); do curl -s -m 1 $URL/map >/dev/null 2>&1 && break; sleep 0.1; done
echo "mercury port $PORT pid $MPID, $NCPU CPUs, window ${WINDOW_MS} ms"
sleep 0.5

spin th 1-4 4
spin ballast 5-12 1 1
[ "$(cat $CG/th/cpuset.cpus)" = 1-4 ] && [ "$(cat $CG/ballast/cpuset.cpus)" = 5-12 ]; verdict "th on 1-4, a near-idle daemon on 5-12"
[ "$(code throttle '{"daemon_id":"th","above":1.2}')" = 400 ]; verdict "throttle rejects above outside (0, 1)"
[ "$(code throttle '{"daemon_id":"th","cut":1}')" = 400 ]; verdict "throttle rejects cut 1"
[ "$(code throttle '{"daemon_id":"nope"}')" = 400 ]; verdict "throttle rejects an unknown daemon"
post throttle "{\"daemon_id\":\"th\",\"window_ms\":$WINDOW_MS}" | grep -q '"cut":0.75'; verdict "throttle attaches with cut 0.75 above 0.8"

windows 4
m=$(cpu_max th); case "$m" in max*|"") true;; *) false;; esac; verdict "daemons' CPUs at a third: no cut (${m:-no cpu.max yet})"
u=$(used th); awk "BEGIN{exit !($u > 3.5)}"; verdict "uncut, th uses ~4 CPUs ($u)"

spin hog 5-$LAST $((NCPU * 2))
windows 4
m=$(cpu_max th); [ "$m" = "100000 100000" ]; verdict "busy daemons' CPUs: cpu.max cut to one CPU of four ($m)"
u=$(used th); awk "BEGIN{exit !($u < 1.2)}"; verdict "cut, th uses ~1 CPU ($u)"
windows 6
[ "$(cpu_max th)" = "100000 100000" ]; verdict "hog still running: cut held, no flapping"
[ "$(grep -c 'throttle daemon_id=th system_load=.* cut' "$LOG")" = 1 ]; verdict "exactly one cut so far"

post kill '{"daemon_id":"hog"}' >/dev/null
windows 4
m=$(cpu_max th); case "$m" in max*) true;; *) false;; esac; verdict "hog gone: cut lifted ($m)"
grep -q 'throttle daemon_id=th .* released' "$LOG"; verdict "release is logged"

post throttle '{"daemon_id":"th","cut":0}' | grep -q '"active":false'; verdict "cut 0 detaches"
post kill '{"daemon_id":"th"}' >/dev/null; post kill '{"daemon_id":"ballast"}' >/dev/null

echo "--- throttle log"; grep "throttle daemon_id" "$LOG" | sed 's/^/  /'
echo "throttle_it: $PASS passed, $FAIL failed"
[ "$FAIL" = 0 ]
