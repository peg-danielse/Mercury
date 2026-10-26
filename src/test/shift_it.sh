#!/usr/bin/env bash
# Integration test of shift (SPEC.md "shift") with spinner daemons. Root (cgroup writes).
# Runs its own mercury on CPU 0 with every other online CPU as the pool, on a free loopback port in
# 18180-18199. Three one-thread daemons share CPU 1, so its L3 domain is at 100%: x and y have shift,
# z does not. x and y must leave, onto CPUs no one else holds, and z must stay. A daemon alone on its
# domain at 100% must not move: any domain it moves to would be just as hot. Detach and bad
# parameters are checked too. CPUs are dealt one at a time on purpose: on pd-gpu every CPU is its own
# L3 domain; on dtn-01 four CPUs share one, so set ONE to a whole L3 domain without Mercury's CPU (e.g. 2,3,34,35).
# Env: MERCURY_BIN, SPIN_BIN, PORT, ONE (default 1), WINDOW_MS (default 500), KEEP=1 keeps the log.
set -u
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)
BIN=${MERCURY_BIN:-$ROOT/build/mercury}
SPIN=${SPIN_BIN:-$ROOT/build/test/spinner}
if [ "$(id -u)" != 0 ]; then
  exec sudo -n env MERCURY_BIN="$BIN" SPIN_BIN="$SPIN" ${PORT:+PORT=$PORT} ${ONE:+ONE=$ONE} ${WINDOW_MS:+WINDOW_MS=$WINDOW_MS} ${KEEP:+KEEP=$KEEP} "$(readlink -f "$0")" "$@"
fi
ONE=${ONE:-1}; WINDOW_MS=${WINDOW_MS:-500}
if [ -z "${PORT:-}" ]; then
  for p in $(seq 18180 18199); do ss -ltnH | awk '{print $4}' | grep -q ":$p\$" || { PORT=$p; break; }; done
fi
NCPU=$(nproc --all)
DIR=$(mktemp -d /tmp/mercury-shift-XXXXXX); LOG=$DIR/mercury.log; URL=http://127.0.0.1:$PORT
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
cpus_of() { cat $CG/$1/cpuset.cpus 2>/dev/null; }
spin() {  # spin ID CPUS THREADS: fixed cpuset, no scaling policy
  IDS="$IDS $1"
  post start "{\"daemon_id\":\"$1\",\"exec_path\":\"$SPIN\",\"cpu\":${2%%[-,]*},\"env\":{\"SPIN_THREADS\":\"$3\",\"SPIN_DUTY\":\"100\"}}" >/dev/null
  post scale "{\"daemon_id\":\"$1\",\"policy\":\"none\"}" >/dev/null
  post cpus "{\"daemon_id\":\"$1\",\"cpus\":\"$2\"}" >/dev/null
}
windows() { sleep $(awk "BEGIN{print $1*$WINDOW_MS/1000}"); }

MERCURY_RINGS=$DIR "$BIN" $PORT 0 $(seq 1 $((NCPU - 1))) >"$LOG" 2>&1 &
MPID=$!
for i in $(seq 50); do curl -s -m 1 $URL/map >/dev/null 2>&1 && break; sleep 0.1; done
echo "mercury port $PORT pid $MPID, $NCPU CPUs, hot domain on $ONE, window ${WINDOW_MS} ms"
sleep 0.5

THREADS=$(echo "$ONE" | tr ',' '\n' | wc -l)
for id in x y z; do spin $id "$ONE" "$THREADS"; done
[ "$(code shift '{"daemon_id":"x","above":1}')" = 400 ]; verdict "shift rejects above outside (0, 1)"
[ "$(code shift '{"daemon_id":"x","enabled":"maybe"}')" = 400 ]; verdict "shift rejects a bad enabled"
[ "$(code shift '{"daemon_id":"nope"}')" = 400 ]; verdict "shift rejects an unknown daemon"
for id in x y; do post shift "{\"daemon_id\":\"$id\",\"window_ms\":$WINDOW_MS}" >/dev/null; done

windows 8
X=$(cpus_of x); Y=$(cpus_of y); Z=$(cpus_of z); ONESPEC=$(echo "$ONE" | tr ',' '\n' | sort -n | python3 -c "
import sys; c=[int(x) for x in sys.stdin]; out=[]; i=0
while i < len(c):
    j=i
    while j+1 < len(c) and c[j+1]==c[j]+1: j+=1
    out.append(str(c[i]) if i==j else f'{c[i]}-{c[j]}'); i=j+1
print(','.join(out))")
[ "$X" != "$ONESPEC" ] && [ "$Y" != "$ONESPEC" ]; verdict "x and y left the hot domain (x=$X y=$Y)"
[ "$Z" = "$ONESPEC" ]; verdict "z without shift stayed ($Z)"
[ "$X" != "$Y" ]; verdict "x and y went to different CPUs"
[ "$(grep -c '^shift daemon_id=[xy] from=.* to=' "$LOG")" = 2 ]; verdict "exactly two moves logged"

# x is now alone on its domain at 100%: moving would only make another domain as hot.
windows 6
[ "$(cpus_of x)" = "$X" ]; verdict "a daemon alone on its hot domain stays ($X)"
grep -q 'shift daemon_id=[xy] .* no domain can take it, holding' "$LOG"; verdict "the hold is logged"

post shift '{"daemon_id":"x","enabled":false}' | grep -q '"enabled":false'; verdict "enabled false detaches"
for id in x y z; do post kill "{\"daemon_id\":\"$id\"}" >/dev/null; done

echo "--- shift log"; grep "^shift daemon_id" "$LOG" | sed 's/^/  /' | head -20
echo "shift_it: $PASS passed, $FAIL failed"
[ "$FAIL" = 0 ]
