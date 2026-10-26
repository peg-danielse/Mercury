#!/usr/bin/env bash
# Integration test of the l3 pool (SPEC.md "l3 pool") with spinner daemons. Root (cgroup writes).
# Runs its own mercury on CPU 0 with every other online CPU as the daemon pool, on a free loopback
# port in 18180-18199. Checks: bad parameters are 400; the pool starts with START CPUs of whole L3
# domains; a busy daemon under saturate (attached at start, least_loaded) only gets pool CPUs; when the
# pool is busy it gains whole L3 domains and the daemon grows into them; an operator's start and /cpus
# outside the pool are remapped into it; an exempt daemon is placed outside the pool; when the pool is
# nearly idle it gives the added domains back one at a time, last first, down to the start pool, and a
# daemon on a removed domain is moved into the CPUs left; off turns it off.
# Env: MERCURY_BIN, SPIN_BIN, PORT, START (default 8), WINDOW_MS (default 500), KEEP=1 keeps the log.
set -u
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)
BIN=${MERCURY_BIN:-$ROOT/build/mercury}
SPIN=${SPIN_BIN:-$ROOT/build/test/spinner}
if [ "$(id -u)" != 0 ]; then
  exec sudo -n env MERCURY_BIN="$BIN" SPIN_BIN="$SPIN" ${PORT:+PORT=$PORT} ${START:+START=$START} ${WINDOW_MS:+WINDOW_MS=$WINDOW_MS} ${KEEP:+KEEP=$KEEP} "$(readlink -f "$0")" "$@"
fi
START=${START:-8}; WINDOW_MS=${WINDOW_MS:-500}
if [ -z "${PORT:-}" ]; then
  for p in $(seq 18180 18199); do ss -ltnH | awk '{print $4}' | grep -q ":$p\$" || { PORT=$p; break; }; done
fi
NCPU=$(nproc --all)
DIR=$(mktemp -d /tmp/mercury-l3-XXXXXX); LOG=$DIR/mercury.log; URL=http://127.0.0.1:$PORT
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
expand() { local r; for r in ${1//,/ }; do case $r in *-*) seq ${r%-*} ${r#*-};; *) echo $r;; esac; done; }
pool_cpus() { curl -s $URL/pool | python3 -c "import json,sys; print(json.load(sys.stdin)['cpus'])"; }
# within A B: every CPU of cpuset A is in cpuset B.
within() { [ -z "$(comm -23 <(expand "$1" | sort) <(expand "$2" | sort))" ]; }
spin() {  # spin ID CPU THREADS: saturate (min 1, max 16, least_loaded) is attached at start
  IDS="$IDS $1"
  post start "{\"daemon_id\":\"$1\",\"exec_path\":\"$SPIN\",\"cpu\":$2,\"env\":{\"SPIN_THREADS\":\"$3\",\"SPIN_DUTY\":\"100\"}}" >/dev/null
}

MERCURY_RINGS=$DIR "$BIN" $PORT 0 $(seq 1 $((NCPU - 1))) >"$LOG" 2>&1 &
MPID=$!
for i in $(seq 50); do curl -s -m 1 $URL/map >/dev/null 2>&1 && break; sleep 0.1; done
echo "mercury port $PORT pid $MPID, $NCPU CPUs, start $START, window ${WINDOW_MS} ms"
sleep 0.5

curl -s $URL/pool | grep -q '"policy":"off"'; verdict "the pool is off by default"
[ "$(code pool '{"policy":"l3","up":1.5}')" = 400 ]; verdict "pool rejects up outside (0, 1)"
[ "$(code pool '{"policy":"l3","start":0}')" = 400 ]; verdict "pool rejects start 0"
[ "$(code pool '{"policy":"bogus"}')" = 400 ]; verdict "pool rejects an unknown policy"
[ "$(code pool '{"policy":"l3","down":0.95}')" = 400 ]; verdict "pool rejects down at or above up"
post pool "{\"policy\":\"l3\",\"start\":$START,\"window_ms\":$WINDOW_MS,\"down_window_ms\":$((WINDOW_MS * 4)),\"exempt\":\"ex\"}" >/dev/null
P0=$(pool_cpus); n0=$(expand "$P0" | wc -l)
[ "$n0" -ge "$START" ]; verdict "pool starts with at least $START CPUs ($P0)"
curl -s "$URL/llm/state" | python3 -c "
import json,sys; s=json.load(sys.stdin); pool=set()
for r in '$P0'.split(','):
    a,_,b=r.partition('-'); pool.update(range(int(a), int(b or a)+1))
sys.exit(0 if all(set(b['cpus']) <= pool or not (set(b['cpus']) & pool) for b in s['l3']) else 1)"
verdict "the pool is whole L3 domains"

# A busy daemon started on a pool CPU: saturate grows it, only inside the pool; the busy pool then grows.
FIRST=$(expand "$P0" | head -1)
spin w $FIRST 32
inside=0
for i in $(seq 40); do
  sleep $(awk "BEGIN{print $WINDOW_MS/1000}")
  within "$(cpus_of w)" "$(pool_cpus)" && inside=$((inside + 1))
done
W=$(cpus_of w)
[ $inside = 40 ]; verdict "w only ever holds pool CPUs ($inside/40 samples; w=$W)"
P1=$(pool_cpus); n1=$(expand "$P1" | wc -l)
[ "$n1" -gt "$n0" ]; verdict "the busy pool grew by whole domains: $n0 -> $n1 CPUs ($P1)"
nw=$(expand "$(cpus_of w)" | wc -l)
[ "$nw" -gt "$n0" ]; verdict "w grew past the start pool into the new domains ($nw CPUs)"
grep -q "^pool l3 load=.* cores=" "$LOG"; verdict "each pool step is logged with its load"

# An operator's explicit placement is mapped into the pool: a start CPU and a /cpus request outside it.
OUT1=$(comm -23 <(seq 1 $((NCPU - 1)) | sort) <(expand "$(pool_cpus)" | sort) | sort -n | head -1)
spin home $OUT1 1
sleep 0.5
H=$(cpus_of home); within "$H" "$(pool_cpus)"; verdict "a start CPU outside the pool lands inside it (asked $OUT1, got $H)"
post cpus "{\"daemon_id\":\"home\",\"cpus\":\"$OUT1\"}" | grep -q '"ok":true'
H=$(cpus_of home); within "$H" "$(pool_cpus)"; verdict "a /cpus request outside the pool is remapped into it ($H)"
grep -q "daemon_id=home pool remapped" "$LOG"; verdict "remaps are logged"
post kill '{"daemon_id":"home"}' >/dev/null

# An exempt daemon, started outside the pool: saturate may place it anywhere.
OUTSIDE=$(comm -23 <(seq 1 $((NCPU - 1)) | sort) <(expand "$(pool_cpus)" | sort) | sort -n | tail -1)
spin ex $OUTSIDE 4
sleep 3
EX=$(cpus_of ex)
! within "$EX" "$(pool_cpus)"; verdict "the exempt daemon is outside the pool ($EX)"
post kill '{"daemon_id":"ex"}' >/dev/null

# Scale-in: w stops; a light daemon sits on the domain added last. The idle pool gives domains back one at a
# time, last first, down to the start pool, and the light daemon is moved into the CPUs that stay.
GROWN=$(pool_cpus)
LASTDOM=$(curl -s "$URL/llm/state" | python3 -c "
import json,sys; s=json.load(sys.stdin); pool=set()
for r in '$GROWN'.split(','):
    a,_,b=r.partition('-'); pool.update(range(int(a), int(b or a)+1))
start=set()
for r in '$P0'.split(','):
    a,_,b=r.partition('-'); start.update(range(int(a), int(b or a)+1))
grown=[b for b in s['l3'] if set(b['cpus']) <= pool and not set(b['cpus']) & start]
print(max(min(b['cpus']) for b in grown))")
post kill '{"daemon_id":"w"}' >/dev/null
spin light $LASTDOM 1
post cpus "{\"daemon_id\":\"light\",\"cpus\":\"$LASTDOM\"}" >/dev/null
L0=$(cpus_of light)
shrunk=0
for i in $(seq 120); do
  sleep $(awk "BEGIN{print $WINDOW_MS/1000}")
  [ "$(pool_cpus)" = "$P0" ] && { shrunk=1; break; }
done
PF=$(pool_cpus)
[ $shrunk = 1 ]; verdict "the idle pool shrank back to the start pool ($GROWN -> $PF)"
steps=$(grep -c "^pool l3 shrink" "$LOG")
grown_steps=$(grep -c "^pool l3 load=.* cores=" "$LOG")
[ "$steps" = "$grown_steps" ]; verdict "one domain per step, as many removals as additions ($steps)"
LF=$(cpus_of light)
within "$LF" "$P0"; verdict "the daemon on a removed domain was moved into the pool ($L0 -> $LF)"
grep -q "^pool daemon_id=light moved" "$LOG"; verdict "the move is logged"
post kill '{"daemon_id":"light"}' >/dev/null

# A fixed pool with swap: it never grows, but a pool domain loaded from outside Mercury is swapped for a clean one
# and the service on it moves along.
post pool "{\"policy\":\"fixed\",\"swap\":true,\"start\":$START,\"window_ms\":$WINDOW_MS}" | grep -q '"policy":"fixed","swap":true'
verdict "a fixed pool with swap is set"
PF0=$(pool_cpus)
DOM=$(curl -s "$URL/llm/state" | python3 -c "
import json,sys; s=json.load(sys.stdin)
b=[b for b in s['l3'] if b.get('in_pool') and len(b['cpus']) == max(len(x['cpus']) for x in s['l3'])][-1]
print(b['name'], ','.join(map(str, b['cpus'])))")
DNAME=${DOM%% *}; DCPUS=${DOM#* }
spin calm ${DCPUS%%,*} 1
post cpus "{\"daemon_id\":\"calm\",\"cpus\":\"${DCPUS%%,*}\"}" >/dev/null
sleep 1
taskset -c $DCPUS "$SPIN" $(echo "$DCPUS" | tr ',' '\n' | wc -l) >/dev/null 2>&1 &
HOG=$!
swapped=0
for i in $(seq 40); do
  sleep $(awk "BEGIN{print $WINDOW_MS/1000}")
  grep -q "^pool swap " "$LOG" && { swapped=1; break; }
done
kill $HOG 2>/dev/null; wait $HOG 2>/dev/null
PF1=$(pool_cpus); C1=$(cpus_of calm)
[ $swapped = 1 ]; verdict "a neighbour outside Mercury on $DNAME makes the pool swap it ($PF0 -> $PF1)"
[ "$(expand "$PF1" | wc -l)" = "$(expand "$PF0" | wc -l)" ]; verdict "the swap keeps the pool's size"
! expand "$C1" | grep -qxF -f <(echo "$DCPUS" | tr ',' '\n'); verdict "the service left the noisy domain ($C1)"
within "$C1" "$PF1"; verdict "and stayed in the pool"
sleep 2
grow=$(grep -c "^pool l3 load=.* cores=" "$LOG")
post kill '{"daemon_id":"calm"}' >/dev/null
spin busy2 $(expand "$PF1" | head -1) $((NCPU * 2))
sleep $(awk "BEGIN{print 6*$WINDOW_MS/1000}")
[ "$(grep -c "^pool l3 load=.* cores=" "$LOG")" = "$grow" ] && [ "$(pool_cpus)" = "$PF1" ]; verdict "a fixed pool never grows, however busy"
post kill '{"daemon_id":"busy2"}' >/dev/null

post pool '{"policy":"off"}' | grep -q '"policy":"off"'; verdict "off turns the pool off"

echo "--- pool steps"; grep "^pool " "$LOG" | sed 's/^/  /' | head -20
echo "l3_it: $PASS passed, $FAIL failed"
[ "$FAIL" = 0 ]
