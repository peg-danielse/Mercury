#!/usr/bin/env bash
# Smoke test of the worker routes (SPEC.md Messages): start with env and a listen socket, cpus,
# migrate, scale (none, full, unknown), map, rings, kill, and SIGTERM removing the daemon cgroups.
# Runs its own mercury on CPU 0 with daemons on CPUs 1-3, on a free loopback port in 18180-18199.
# Uses env_probe as the daemon. Re-executes itself with sudo -n (cgroup writes). A few seconds.
# Env: MERCURY_BIN, PROBE_BIN, PORT, KEEP=1 keeps the log.
set -u
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)
BIN=${MERCURY_BIN:-$ROOT/build/mercury}
PROBE=${PROBE_BIN:-$ROOT/build/test/env_probe}
if [ "$(id -u)" != 0 ]; then
  exec sudo -n env MERCURY_BIN="$BIN" PROBE_BIN="$PROBE" ${PORT:+PORT=$PORT} ${KEEP:+KEEP=$KEEP} "$(readlink -f "$0")" "$@"
fi
if [ -z "${PORT:-}" ]; then
  for p in $(seq 18180 18199); do ss -ltnH | awk '{print $4}' | grep -q ":$p\$" || { PORT=$p; break; }; done
fi
DIR=$(mktemp -d /tmp/mercury-smoke-XXXXXX); LOG=$DIR/mercury.log; URL=http://127.0.0.1:$PORT
CG=/sys/fs/cgroup/mercury; A="sm$$-a"; B="sm$$-b"; PASS=0; FAIL=0
cleanup() {
  [ -n "${MPID:-}" ] && kill $MPID 2>/dev/null && wait $MPID 2>/dev/null
  for id in $A $B; do [ -d $CG/$id ] && { echo 1 > $CG/$id/cgroup.kill; sleep 0.2; rmdir $CG/$id; } 2>/dev/null; done
  if [ "$FAIL" != 0 ] || [ -n "${KEEP:-}" ]; then echo "kept $DIR"; else rm -rf "$DIR"; fi
}
trap cleanup EXIT
# Run a condition, then `verdict WHAT`: it passes when the condition's status was 0.
verdict() { if [ $? = 0 ]; then PASS=$((PASS+1)); echo "  ok   $1"; else FAIL=$((FAIL+1)); echo "  FAIL $1"; fi; }
post() { curl -s -m 10 -X POST -d "$2" $URL/$1; }
code() { curl -s -m 10 -o /dev/null -w '%{http_code}' -X POST -d "$2" $URL/$1; }
get_code() { curl -s -m 10 -o /dev/null -w '%{http_code}' $URL/$1; }
cpus_of() { cat $CG/$1/cpuset.cpus 2>/dev/null; }
pid_of() { sed -n 's/.*"pid":\([0-9]*\).*/\1/p'; }
alive() { kill -0 "$1" 2>/dev/null; }
start_body() { echo "{\"daemon_id\":\"$1\",\"exec_path\":\"$PROBE\",\"cpu\":1,\"listen_port\":$2$3}"; }

MERCURY_RINGS=$DIR "$BIN" $PORT 0 1 2 3 >"$LOG" 2>&1 &
MPID=$!
for i in $(seq 50); do curl -s -m 1 $URL/map >/dev/null 2>&1 && break; sleep 0.1; done
echo "mercury port $PORT pid $MPID, daemons on 1-3"

[ "$(code start "$(start_body $A 18101 ',"env":{"bad-name":"x"}')")" = 400 ]; verdict "start rejects a bad env name"
PA=$(post start "$(start_body $A 18101 ',"env":{"MERCURY_TEST_ENV":"from-request","PATH":"/only-this"}')" | pid_of)
[ -n "$PA" ]; verdict "start returns a pid"
[ "$(cpus_of $A)" = 1 ] && grep -qx "$PA" $CG/$A/cgroup.procs; verdict "daemon sits in its cgroup on cpu 1"

post scale "{\"daemon_id\":\"$A\",\"policy\":\"none\"}" >/dev/null
[ "$(code scale "{\"daemon_id\":\"$A\",\"policy\":\"bogus\"}")" = 400 ]; verdict "scale rejects an unknown policy"
[ "$(code cpus "{\"daemon_id\":\"$A\",\"cpus\":\"2-3\"}")" = 200 ] && [ "$(cpus_of $A)" = 2-3 ]; verdict "cpus writes 2-3"
[ "$(code cpus "{\"daemon_id\":\"$A\",\"cpus\":\"0-2\"}")" = 400 ]; verdict "cpus rejects mercury's cpu"
[ "$(code cpus "{\"daemon_id\":\"$A\",\"cpus\":\"nope\"}")" = 400 ]; verdict "cpus rejects a bad set"
[ "$(code cpus '{"daemon_id":"../x","cpus":"2"}')" = 400 ] && [ "$(cpus_of $A)" = 2-3 ]; verdict "cpus rejects a path in daemon_id"
[ "$(post migrate "{\"daemon_id\":\"$A\",\"cpu\":2}" | pid_of)" = "$PA" ] && [ "$(cpus_of $A)" = 2 ]
verdict "migrate keeps the pid and moves to cpu 2"
post scale "{\"daemon_id\":\"$A\",\"policy\":\"full\"}" | grep -q '"cores":3' && [ "$(cpus_of $A)" = 1-3 ]
verdict "scale full gives the whole pool"

curl -s -m 10 $URL/map | grep -q "\"mercury_cpu\":0.*\"level\":\"core\".*\"$A\""; verdict "map shows the tree and the daemon"
[ "$(get_code 'rings?count=1')" = 200 ]; verdict "rings answers count=1"
[ "$(get_code 'rings?count=nope')" = 400 ]; verdict "rings rejects a bad count"

PB=$(post start "$(start_body $B 18102 '')" | pid_of)
post kill "{\"daemon_id\":\"$A\"}" | grep -q '"ok":true' && ! alive $PA; verdict "kill stops the first daemon"
grep -q 'log=MERCURY_TEST_ENV=from-request' $LOG && grep -q '^PATH=/only-this' $LOG; verdict "the daemon got its env"

kill -TERM $MPID; wait $MPID; RC=$?; MPID=""
[ $RC = 143 ]; verdict "SIGTERM exits mercury with 143"
[ -n "$PB" ] && ! alive $PB && [ ! -d $CG/$B ]; verdict "SIGTERM killed the second daemon and removed its cgroup"

echo "smoke_it: $PASS passed, $FAIL failed"
[ "$FAIL" = 0 ]
