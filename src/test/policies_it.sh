#!/usr/bin/env bash
# Integration test for policies.md on a partial share of this host.
#
# Starts its own mercury on a free loopback port with an explicit CPU share spanning NUMA
# nodes 0-2 (worker on CPU 0), starts spinner daemons through /start, attaches policies with
# POST /policies, and checks attach validation, shift, widen (max_groups and ceiling), degrade,
# and empty-document removal. Needs root for cgroup writes; re-executes itself with sudo -n.
#
# Usage: src/test/policies_it.sh            (from anywhere; uses build/mercury and build/test/spinner)
# Env:   MERCURY_BIN, SPIN_BIN, PORT, KEEP=1 keeps the temp dir with the mercury log.
set -u
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)
BIN=${MERCURY_BIN:-$ROOT/build/mercury}
SPIN=${SPIN_BIN:-$ROOT/build/test/spinner}
if [ "$(id -u)" != 0 ]; then
  exec sudo -n env MERCURY_BIN="$BIN" SPIN_BIN="$SPIN" ${PORT:+PORT=$PORT} ${KEEP:+KEEP=$KEEP} "$(readlink -f "$0")" "$@"
fi
[ -x "$BIN" ] || { echo "missing $BIN"; exit 2; }
[ -x "$SPIN" ] || { echo "missing $SPIN"; exit 2; }

node_cpus() { cat /sys/devices/system/node/node$1/cpulist; }
expand() { python3 -c 'import sys
out=[]
for p in sys.argv[1].strip().split(","):
    if not p: continue
    a,_,b=p.partition("-"); out+=range(int(a),int(b or a)+1)
print(" ".join(map(str,sorted(out))))' "$1"; }

N0=$(expand "$(node_cpus 0)"); N1=$(expand "$(node_cpus 1)"); N2=$(expand "$(node_cpus 2)")
SHARE="$N0 $N1 $N2"
WORKER=$(echo $SHARE | tr ' ' '\n' | sort -n | head -1)
N1_FIRST=$(echo $N1 | cut -d' ' -f1); N1_SECOND=$(echo $N1 | cut -d' ' -f2)
N2_A=$(echo $N2 | cut -d' ' -f1); N2_B=$(echo $N2 | cut -d' ' -f2); N2_C=$(echo $N2 | cut -d' ' -f3)

if [ -z "${PORT:-}" ]; then
  for p in $(seq 18180 18199); do
    if ! ss -ltnH | awk '{print $4}' | grep -q ":$p\$"; then PORT=$p; break; fi
  done
fi
[ -n "${PORT:-}" ] || { echo "no free port"; exit 2; }
TAG="pt$$"
DIR=$(mktemp -d /tmp/mercury-policies-XXXXXX)
LOG=$DIR/mercury.log
URL=http://127.0.0.1:$PORT
STARTED=()
HOGS=()
PASS=0
FAIL=0

cleanup() {
  for h in "${HOGS[@]}"; do kill "$h" 2>/dev/null; done
  for id in "${STARTED[@]}"; do curl -s -m 10 -X POST -d "{\"daemon_id\":\"$id\"}" $URL/kill >/dev/null 2>&1; done
  [ -n "${MPID:-}" ] && kill "$MPID" 2>/dev/null && wait "$MPID" 2>/dev/null
  for id in "${STARTED[@]}"; do rmdir /sys/fs/cgroup/mercury/$id 2>/dev/null; done
  if [ "$FAIL" != 0 ] || [ -n "${KEEP:-}" ]; then echo "kept $DIR (mercury log: $LOG)"; else rm -rf "$DIR"; fi
}
trap cleanup EXIT

ok() { PASS=$((PASS+1)); echo "  ok   $*"; }
bad() { FAIL=$((FAIL+1)); echo "  FAIL $*"; }
post() {  # post <path> <json>  -> sets CODE and BODY
  BODY=$(curl -s -m 20 -o - -w '\n%{http_code}' -X POST -d "$2" $URL$1)
  CODE=${BODY##*$'\n'}; BODY=${BODY%$'\n'*}
}
expect() {  # expect <code> <label> <path> <json>
  post "$3" "$4"
  if [ "$CODE" = "$1" ]; then ok "$2 ($CODE)"; else bad "$2: want $1 got $CODE $BODY"; fi
}
cpuset() { expand "$(cat /sys/fs/cgroup/mercury/$1/cpuset.cpus 2>/dev/null)"; }
in_set() { for c in $1; do case " $2 " in *" $c "*) ;; *) return 1;; esac; done; return 0; }
meets() { for c in $1; do case " $2 " in *" $c "*) return 0;; esac; done; return 1; }
count() { echo $1 | wc -w; }
start() {  # start <id> <cpu> <threads> <duty>
  STARTED+=("$1")
  post /start "{\"daemon_id\":\"$1\",\"exec_path\":\"$SPIN\",\"cpu\":$2,\"env\":{\"SPIN_THREADS\":\"$3\",\"SPIN_DUTY\":\"$4\"}}"
  [ "$CODE" = 200 ] || { bad "start $1 on $2: $CODE $BODY"; return 1; }
}
stop() { curl -s -m 10 -X POST -d "{\"daemon_id\":\"$1\"}" $URL/kill >/dev/null; rmdir /sys/fs/cgroup/mercury/$1 2>/dev/null; }
hog() {  # hog <cpulist> <threads>: external load outside Mercury
  taskset -c "$1" "$SPIN" "$2" 100 >/dev/null 2>&1 &
  HOGS+=($!); HOG=$!
}
group_of() {  # group_of <level> <cpu>
  curl -s $URL/map | python3 -c 'import json,sys
m=json.load(sys.stdin)
for g in m["groups"]:
    if g["level"]==sys.argv[1] and int(sys.argv[2]) in g["linux_cpus"]: print(g["name"]); break' "$1" "$2"
}

echo "mercury $BIN port $PORT share: $SHARE (worker cpu $WORKER) dir $DIR"
MERCURY_RINGS=$DIR "$BIN" $PORT $SHARE >"$LOG" 2>&1 &
MPID=$!
for i in $(seq 50); do curl -s -m 1 $URL/map >/dev/null 2>&1 && break; sleep 0.2; done
curl -s -m 2 $URL/map >/dev/null || { bad "mercury did not come up"; cat "$LOG"; exit 1; }
sleep 6  # node_hot needs five full 1 s intervals in the rings

echo "== attach validation and empty document"
V=$TAG-val
start $V $N2_B 1 10
expect 400 "unknown field" /policies "{\"daemon_id\":\"$V\",\"foo\":1}"
expect 400 "shift not boolean" /policies "{\"daemon_id\":\"$V\",\"shift\":\"yes\"}"
expect 400 "unknown restrict.level" /policies "{\"daemon_id\":\"$V\",\"restrict\":{\"level\":\"rack\"}}"
expect 400 "unknown restrict field" /policies "{\"daemon_id\":\"$V\",\"restrict\":{\"floor\":\"node.0\"}}"
expect 400 "bad component name" /policies "{\"daemon_id\":\"$V\",\"degrade\":{\"components\":[{\"name\":\"1bad\",\"cost\":0.2}]}}"
expect 400 "cost 0" /policies "{\"daemon_id\":\"$V\",\"degrade\":{\"components\":[{\"name\":\"trace\",\"cost\":0}]}}"
expect 400 "cost above 1" /policies "{\"daemon_id\":\"$V\",\"degrade\":{\"components\":[{\"name\":\"trace\",\"cost\":1.5}]}}"
expect 400 "unknown ceiling" /policies "{\"daemon_id\":\"$V\",\"restrict\":{\"level\":\"core\",\"ceiling\":\"node.99\"}}"
expect 400 "cpuset outside ceiling" /policies "{\"daemon_id\":\"$V\",\"restrict\":{\"level\":\"core\",\"ceiling\":\"node.0\"}}"
expect 400 "widen max_groups 0" /policies "{\"daemon_id\":\"$V\",\"widen\":{\"max_groups\":0}}"
expect 400 "exec_path differs" /policies "{\"daemon_id\":\"$V\",\"exec_path\":\"/bin/false\",\"shift\":true}"
expect 400 "daemon not running" /policies "{\"daemon_id\":\"$TAG-nope\",\"shift\":true}"
expect 400 "not JSON" /policies "{\"daemon_id\":\"$V\","
expect 200 "valid degrade attach" /policies "{\"daemon_id\":\"$V\",\"exec_path\":\"$SPIN\",\"degrade\":{\"components\":[{\"name\":\"trace\",\"cost\":0.3}]}}"
PFILE=$(echo "$BODY" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("path",""))')
[ "$PFILE" = "$DIR/$V.policy" ] && ok "response path $PFILE" || bad "response path '$PFILE' (body $BODY)"
[ "$(cat "$PFILE" 2>/dev/null)" = '{"components":[{"name":"trace","enabled":true}]}' ] && ok "policy file written at attach, all enabled" || bad "policy file: $(cat "$PFILE" 2>&1)"
cp "$PFILE" $DIR/before
expect 400 "bad re-post rejected" /policies "{\"daemon_id\":\"$V\",\"degrade\":{\"components\":[{\"name\":\"x\",\"cost\":2}]}}"
cmp -s "$PFILE" $DIR/before && ok "rejected post left the file as it was" || bad "file changed by a rejected post"
expect 200 "re-post replaces the document" /policies "{\"daemon_id\":\"$V\",\"degrade\":{\"components\":[{\"name\":\"metrics\",\"cost\":0.2},{\"name\":\"trace\",\"cost\":0.1}]}}"
[ "$(cat "$PFILE")" = '{"components":[{"name":"metrics","enabled":true},{"name":"trace","enabled":true}]}' ] && ok "re-post wrote a fresh file" || bad "re-post file: $(cat "$PFILE")"
expect 200 "empty document" /policies "{\"daemon_id\":\"$V\"}"
case "$BODY" in *path*) bad "empty document response has a path: $BODY";; *) ok "empty document response has no path";; esac
[ ! -e "$PFILE" ] && ok "empty document deleted the policy file" || bad "policy file still there"
post /scale "{\"daemon_id\":\"$V\",\"max\":2}"
[ "$CODE" = 200 ] && ok "/scale still answers after removal" || bad "/scale after removal $CODE"
stop $V

echo "== shift off a hot node"
S=$TAG-shift; K=$TAG-stay
start $S $N1_FIRST 1 100
start $K $N1_SECOND 1 100
expect 200 "attach shift" /policies "{\"daemon_id\":\"$S\",\"shift\":true}"
expect 200 "attach shift with ceiling node.1" /policies "{\"daemon_id\":\"$K\",\"shift\":true,\"restrict\":{\"level\":\"node\",\"ceiling\":\"node.1\"}}"
hog "$(node_cpus 1)" $(count "$N1")
moved=""
for i in $(seq 30); do
  sleep 1
  now=$(cpuset $S)
  if [ -n "$now" ] && ! meets "$now" "$N1"; then moved=$now; break; fi
done
if [ -n "$moved" ]; then ok "shift moved $S off node.1 to cpus [$moved] after ${i}s"; else bad "$S still on [$(cpuset $S)]"; fi
[ -n "$moved" ] && [ "$(count "$moved")" = 1 ] && ok "shift kept the cpu count" || bad "cpu count after shift [$moved]"
[ -n "$moved" ] && in_set "$moved" "$N0 $N2" && ! meets "$moved" "$WORKER" && ok "destination is another node, not the worker cpu" || bad "destination [$moved]"
sleep 6
in_set "$(cpuset $K)" "$N1" && ok "ceiling node.1 keeps $K on node.1 [$(cpuset $K)]" || bad "$K left its ceiling [$(cpuset $K)]"
grep -q "policy shift daemon_id=$S" "$LOG" && ok "shift logged" || bad "no shift log line"
kill $HOG 2>/dev/null; stop $S; stop $K
sleep 6  # let node.1 cool down

echo "== widen with max_groups (saturate fills the grant)"
W=$TAG-widen
start $W $N2_A 6 100
expect 200 "attach widen max_groups 2" /policies "{\"daemon_id\":\"$W\",\"restrict\":{\"level\":\"core\"},\"widen\":{\"max_groups\":2}}"
peak=0; reached=""
for i in $(seq 60); do
  sleep 1; n=$(count "$(cpuset $W)"); [ "$n" -gt "$peak" ] && peak=$n
  [ "$n" -ge 4 ] && [ -z "$reached" ] && reached=$i
  [ -n "$reached" ] && [ $((i - reached)) -ge 8 ] && break
done
final=$(cpuset $W)
[ -n "$reached" ] && ok "saturate and widen grew $W to [$final] after ${reached}s" || bad "did not reach 4 cpus [$final]"
[ "$peak" -le 4 ] && ok "max_groups 2 held the cpuset at $peak cpus with 6 busy threads" || bad "cpuset grew to $peak cpus"
cores=$(for c in $final; do group_of core $c; done | sort -u | wc -l)
[ "$cores" = 2 ] && ok "cpuset spans exactly 2 cores" || bad "cpuset spans $cores cores"
[ "$(grep -c "policy widen daemon_id=$W" "$LOG")" = 1 ] && ok "exactly one widen logged" || bad "widen log count $(grep -c "policy widen daemon_id=$W" "$LOG")"
stop $W

echo "== widen inside a ceiling"
C=$TAG-ceil
L3=$(group_of l3 $N2_C)
L3CPUS=$(expand "$(curl -s $URL/map | python3 -c 'import json,sys
m=json.load(sys.stdin)
print(",".join(str(c) for g in m["groups"] if g["name"]==sys.argv[1] for c in g["linux_cpus"]))' "$L3")")
start $C $N2_C 8 100
expect 200 "attach widen with ceiling $L3" /policies "{\"daemon_id\":\"$C\",\"restrict\":{\"level\":\"core\",\"ceiling\":\"$L3\"},\"widen\":{}}"
full=$(count "$L3CPUS"); reached=""; outside=""
for i in $(seq 60); do
  sleep 1; now=$(cpuset $C)
  in_set "$now" "$L3CPUS" || outside=$now
  [ "$(count "$now")" -ge "$full" ] && [ -z "$reached" ] && reached=$i
  [ -n "$reached" ] && [ $((i - reached)) -ge 8 ] && break
done
[ -z "$outside" ] && ok "cpuset stayed inside $L3 [$L3CPUS] with 8 busy threads" || bad "cpuset left the ceiling [$outside]"
[ -n "$reached" ] && ok "saturate and widen filled the ceiling [$(cpuset $C)] after ${reached}s" || bad "did not fill $L3 [$(cpuset $C)]"
stop $C

echo "== degrade brownout"
D=$TAG-deg
start $D $N2_B 1 20
expect 200 "attach degrade" /policies "{\"daemon_id\":\"$D\",\"degrade\":{\"components\":[{\"name\":\"trace\",\"cost\":0.3},{\"name\":\"metrics\",\"cost\":0.1}]}}"
DFILE=$DIR/$D.policy
ALL_ON='{"components":[{"name":"trace","enabled":true},{"name":"metrics","enabled":true}]}'
TRACE_OFF='{"components":[{"name":"trace","enabled":false},{"name":"metrics","enabled":true}]}'
[ "$(cat $DFILE)" = "$ALL_ON" ] && ok "attach wrote every component enabled" || bad "attach file $(cat $DFILE)"
hog "$(node_cpus 2)" $(count "$N2")
got=""
for i in $(seq 30); do sleep 1; [ "$(cat $DFILE)" = "$TRACE_OFF" ] && { got=$i; break; }; done
[ -n "$got" ] && ok "overload disabled trace (highest cost) after ${got}s" || bad "file under load: $(cat $DFILE)"
sleep 7
[ "$(cat $DFILE)" = "$TRACE_OFF" ] && ok "U - 0.3 <= 0.8: metrics stays enabled" || bad "file later: $(cat $DFILE)"
kill $HOG 2>/dev/null
got=""
for i in $(seq 30); do sleep 1; [ "$(cat $DFILE)" = "$ALL_ON" ] && { got=$i; break; }; done
[ -n "$got" ] && ok "trace re-enabled after five cool intervals (${got}s)" || bad "file after load: $(cat $DFILE)"
grep -q "policy degrade daemon_id=$D" "$LOG" && ok "degrade logged" || bad "no degrade log line"
stop $D

kill -0 $MPID 2>/dev/null && ok "mercury still running" || bad "mercury died"
echo "policies_it: $PASS passed, $FAIL failed"
[ "$FAIL" = 0 ]
