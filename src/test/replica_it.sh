#!/usr/bin/env bash
# Horizontal scaling (SPEC.md "LLM interface"): MERCURY_REUSEPORT, add_replica and remove_replica.
# Two processes share a port, the kernel sends new connections to both, and remove_replica stops
# only the added one. Root (cgroup writes).
# Env: MERCURY_BIN, REUSE_BIN, PORT, KEEP=1 keeps the log.
set -u
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)
BIN=${MERCURY_BIN:-$ROOT/build/mercury}
PROBE=${REUSE_BIN:-$ROOT/build/test/reuse_probe}
if [ "$(id -u)" != 0 ]; then
  exec sudo -n env MERCURY_BIN="$BIN" REUSE_BIN="$PROBE" ${PORT:+PORT=$PORT} ${KEEP:+KEEP=$KEEP} "$(readlink -f "$0")" "$@"
fi
if [ -z "${PORT:-}" ]; then
  for p in $(seq 18180 18199); do ss -ltnH | awk '{print $4}' | grep -q ":$p\$" || { PORT=$p; break; }; done
fi
NCPU=$(nproc --all)
DIR=$(mktemp -d /tmp/mercury-replica-XXXXXX); LOG=$DIR/mercury.log; URL=http://127.0.0.1:$PORT
CG=/sys/fs/cgroup/mercury; IDS=""; PASS=0; FAIL=0
P_FD=$((PORT + 1)); P_OWN=$((PORT + 2))
cleanup() {
  [ -n "${MPID:-}" ] && kill $MPID 2>/dev/null && wait $MPID 2>/dev/null
  for id in $IDS; do [ -d $CG/$id ] && { echo 1 > $CG/$id/cgroup.kill; sleep 0.2; rmdir $CG/$id; } 2>/dev/null; done
  if [ "$FAIL" != 0 ] || [ -n "${KEEP:-}" ]; then echo "kept $DIR"; else rm -rf "$DIR"; fi
}
trap cleanup EXIT
verdict() { if [ "$1" = 0 ]; then PASS=$((PASS+1)); echo "  ok   $2"; else FAIL=$((FAIL+1)); echo "  FAIL $2"; fi; }
post() { curl -s -m 10 -X POST -d "$2" $URL/$1; }
code() { curl -s -m 10 -o /dev/null -w '%{http_code}' -X POST -d "$2" $URL/$1; }

MERCURY_RINGS=$DIR MERCURY_REUSEPORT=1 "$BIN" $PORT 0 $(seq 1 $((NCPU - 1))) >"$LOG" 2>&1 &
MPID=$!
for i in $(seq 50); do curl -s -m 1 $URL/ping >/dev/null 2>&1 && break; sleep 0.1; done
echo "mercury port $PORT pid $MPID"

# Passed socket: Mercury binds the port. Self bind: the process binds BIND_PORT, and LD_PRELOAD sets SO_REUSEPORT.
IDS="fd own x-loadgen"
post start "{\"daemon_id\":\"fd\",\"exec_path\":\"$PROBE\",\"cpu\":1,\"listen_port\":$P_FD}" >/dev/null
post start "{\"daemon_id\":\"own\",\"exec_path\":\"$PROBE\",\"cpu\":1,\"env\":{\"BIND_PORT\":\"$P_OWN\"}}" >/dev/null
post start "{\"daemon_id\":\"x-loadgen\",\"exec_path\":\"$PROBE\",\"cpu\":1,\"listen_port\":$((PORT + 3))}" >/dev/null
post scale '{"daemon_id":"fd","policy":"none"}' >/dev/null
post scale '{"daemon_id":"own","policy":"none"}' >/dev/null
post scale '{"daemon_id":"x-loadgen","policy":"none"}' >/dev/null

[ "$(code llm/act '{"action":"add_replica","daemon_id":"x-loadgen"}')" = 409 ]; verdict $? "loadgen is not replicated"
[ "$(code llm/act '{"action":"remove_replica","daemon_id":"fd"}')" = 409 ]; verdict $? "remove with no replica is 409"

ADD=$(post llm/act '{"action":"add_replica","daemon_id":"fd"}')
echo "$ADD" | grep -q '"daemon_id":"fd-r2"'; verdict $? "add_replica of a passed socket is fd-r2 ($ADD)"
ADD2=$(post llm/act '{"action":"add_replica","daemon_id":"own"}')
echo "$ADD2" | grep -q '"daemon_id":"own-r2"'; verdict $? "add_replica of a self-bound port is own-r2 ($ADD2)"
sleep 0.3

hits() {  # hits PORT: how many of 40 connections each daemon accepted
  python3 - "$1" <<'PY'
import socket, sys
from collections import Counter
c = Counter()
port = int(sys.argv[1])
for _ in range(40):
    s = socket.create_connection(("127.0.0.1", port), 2)
    c[s.recv(64).decode().strip()] += 1
    s.close()
print(" ".join(f"{k}={v}" for k, v in sorted(c.items())))
PY
}
FDH=$(hits $P_FD)
OWNH=$(hits $P_OWN)
echo "$FDH" | grep -q 'fd=' && echo "$FDH" | grep -q 'fd-r2='; verdict $? "passed socket balances ($FDH)"
echo "$OWNH" | grep -q 'own=' && echo "$OWNH" | grep -q 'own-r2='; verdict $? "self bind balances ($OWNH)"

S=$(curl -s -m 10 "$URL/llm/state?window_ms=500")
echo "$S" | python3 -c 'import json,sys; s=json.load(sys.stdin); d={x["daemon_id"]: x for x in s["daemons"]}; assert s["port_lb"] and d["fd-r2"]["replica"] and d["fd-r2"]["set"]=="fd" and not d["fd"]["replica"]'
verdict $? "state marks the replica and port_lb"

post llm/act '{"action":"remove_replica","daemon_id":"fd"}' | grep -q '"daemon_id":"fd-r2"'; verdict $? "remove_replica stops fd-r2"
sleep 0.2
LEFT=$(hits $P_FD)
echo "$LEFT" | grep -q 'fd=' && ! echo "$LEFT" | grep -q 'fd-r2='; verdict $? "after remove only the original accepts ($LEFT)"
[ "$(code llm/act '{"action":"remove_replica","daemon_id":"fd-r2"}')" = 409 ]; verdict $? "the original is not removed"

echo "$PASS passed, $FAIL failed"
[ "$FAIL" = 0 ]
