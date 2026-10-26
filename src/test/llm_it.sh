#!/usr/bin/env bash
# Integration test of the LLM interface (SPEC.md "LLM interface"): GET /llm/state with its cursor,
# POST /llm/act, and tools/llmctl.py's checking cycle in mock mode (no model, no network). Root
# (cgroup writes). Runs its own mercury on CPU 0 with every other online CPU as the pool, on a free
# loopback port in 18180-18199. Needs python3.
# Env: MERCURY_BIN, SPIN_BIN, PORT, KEEP=1 keeps the log.
set -u
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)
BIN=${MERCURY_BIN:-$ROOT/build/mercury}
SPIN=${SPIN_BIN:-$ROOT/build/test/spinner}
CTL=$ROOT/tools/llmctl.py
if [ "$(id -u)" != 0 ]; then
  exec sudo -n env MERCURY_BIN="$BIN" SPIN_BIN="$SPIN" ${PORT:+PORT=$PORT} ${KEEP:+KEEP=$KEEP} "$(readlink -f "$0")" "$@"
fi
if [ -z "${PORT:-}" ]; then
  for p in $(seq 18180 18199); do ss -ltnH | awk '{print $4}' | grep -q ":$p\$" || { PORT=$p; break; }; done
fi
NCPU=$(nproc --all)
DIR=$(mktemp -d /tmp/mercury-llm-XXXXXX); LOG=$DIR/mercury.log; URL=http://127.0.0.1:$PORT
CG=/sys/fs/cgroup/mercury; IDS=""; PASS=0; FAIL=0
cleanup() {
  [ -n "${MPID:-}" ] && kill $MPID 2>/dev/null && wait $MPID 2>/dev/null
  for id in $IDS; do [ -d $CG/$id ] && { echo 1 > $CG/$id/cgroup.kill; sleep 0.2; rmdir $CG/$id; } 2>/dev/null; done
  if [ "$FAIL" != 0 ] || [ -n "${KEEP:-}" ]; then echo "kept $DIR"; else rm -rf "$DIR"; fi
}
trap cleanup EXIT
verdict() { if [ $? = 0 ]; then PASS=$((PASS+1)); echo "  ok   $1"; else FAIL=$((FAIL+1)); echo "  FAIL $1"; fi; }
# verdict_s STATUS WHAT: for messages with a command substitution, which would reset $? before verdict ran.
verdict_s() { if [ "$1" = 0 ]; then PASS=$((PASS+1)); echo "  ok   $2"; else FAIL=$((FAIL+1)); echo "  FAIL $2"; fi; }
post() { curl -s -m 10 -X POST -d "$2" $URL/$1; }
code() { curl -s -m 10 -o /dev/null -w '%{http_code}' -X POST -d "$2" $URL/$1; }
state() { curl -s -m 10 "$URL/llm/state?window_ms=500$1"; }
cpus_of() { cat $CG/$1/cpuset.cpus 2>/dev/null; }
# py EXPR: evaluate EXPR with s = the JSON on stdin.
py() { python3 -c "import json,sys; s=json.load(sys.stdin); print($1)"; }
spin() {  # spin ID CPUS THREADS: fixed cpuset, no scaling policy
  IDS="$IDS $1"
  post start "{\"daemon_id\":\"$1\",\"exec_path\":\"$SPIN\",\"cpu\":${2%%[-,]*},\"env\":{\"SPIN_THREADS\":\"$3\",\"SPIN_DUTY\":\"100\"}}" >/dev/null
  post scale "{\"daemon_id\":\"$1\",\"policy\":\"none\"}" >/dev/null
  post cpus "{\"daemon_id\":\"$1\",\"cpus\":\"$2\"}" >/dev/null
}

MERCURY_RINGS=$DIR MERCURY_CPUSTAT=1 "$BIN" $PORT 0 $(seq 1 $((NCPU - 1))) >"$LOG" 2>&1 &
MPID=$!
for i in $(seq 50); do curl -s -m 1 $URL/map >/dev/null 2>&1 && break; sleep 0.1; done
echo "mercury port $PORT pid $MPID, $NCPU CPUs"
sleep 0.6

S=$(state "")
echo "$S" | py 's["ok"] and len(s["l3"]) > 0 and "cursor" in s' | grep -q True; verdict "state lists L3 domains and a cursor"
C0=$(echo "$S" | py 's["cursor"]')
[ "$(curl -s -o /dev/null -w '%{http_code}' "$URL/llm/state?cursor=nonsense")" = 400 ]; verdict "a bad cursor is 400"

spin d 1 1
sleep 0.6
S=$(state "")
echo "$S" | py 'len(s.get("cpus", [])) > 0 and any(c["cpu"] == 1 and "d" in c["daemons"] and c["busy"] > 0.5 for c in s["cpus"])' | grep -q True
verdict "per-CPU counters: CPU 1 is busy and holds d"
echo "$S" | py '"cpu_wait" in next(x for x in s["daemons"] if x["daemon_id"] == "d")' | grep -q True; verdict "daemons carry aggregated counters"
S=$(state "&cursor=$C0")
echo "$S" | py 'any(e["kind"] == "start" and e["daemon_id"] == "d" for e in s["since"]["events"])' | grep -q True
verdict "since the first cursor: the start of d is an event"
echo "$S" | py 'len(next(iter(s["since"]["l3_history"].values()))) >= 1' | grep -q True; verdict "since carries per-domain load history"
C1=$(echo "$S" | py 's["cursor"]')
[ "$(state "&cursor=$C1" | py 'len(s["since"]["events"])')" = 0 ]; verdict "nothing new since the latest cursor"

[ "$(code llm/act '{"action":"jump","daemon_id":"d"}')" = 400 ]; verdict "unknown action is 400"
[ "$(code llm/act '{"action":"add_cores","daemon_id":"nope"}')" = 400 ]; verdict "unknown daemon is 400"
[ "$(code llm/act '{"action":"add_cores","daemon_id":"d","count":0}')" = 400 ]; verdict "count 0 is 400"
post llm/act '{"action":"add_cores","daemon_id":"d","count":2}' | grep -q '"cores":3'; verdict_s $? "add_cores 2 gives 3 CPUs ($(cpus_of d))"
post llm/act '{"action":"remove_cores","daemon_id":"d","count":1}' | grep -q '"cores":2'; verdict_s $? "remove_cores 1 gives 2 CPUs ($(cpus_of d))"
n=$(cpus_of d | tr ',' '\n' | wc -l)
post llm/act '{"action":"add_l3","daemon_id":"d"}' | grep -q '"ok":true'; verdict_s $? "add_l3 adds a free domain ($(cpus_of d))"
S=$(state "")
FROM=$(echo "$S" | py 'next(b["name"] for b in s["l3"] if "d" in b["daemons"])')
TO=$(echo "$S" | py 'next(b["name"] for b in s["l3"] if not b["daemons"] and len(b["free"]) == len(b["cpus"]))')
OTHER=$(echo "$S" | py 'next(b["name"] for b in s["l3"] if not b["daemons"] and b["name"] != "'$TO'")')
[ "$(code llm/act "{\"action\":\"move\",\"daemon_id\":\"d\",\"from\":\"$OTHER\",\"to\":\"$TO\"}")" = 409 ]
verdict "moving from a domain d does not use is 409"
post llm/act "{\"action\":\"move\",\"daemon_id\":\"d\",\"from\":\"$FROM\",\"to\":\"$TO\"}" | grep -q '"ok":true'
verdict_s $? "move d from $FROM to $TO ($(cpus_of d))"
S=$(state "&cursor=$C1")
echo "$S" | py 'next(x["policy"] for x in s["daemons"] if x["daemon_id"] == "d")' | grep -qx none; verdict "d keeps its own scaling policy (none here)"
[ "$(echo "$S" | py 'sum(e["kind"] == "llm" for e in s["since"]["events"])')" = 4 ]; verdict "the four actions are events since the cursor"
post kill '{"daemon_id":"d"}' >/dev/null

# Two daemons share every CPU of one whole L3 domain (one CPU on pd-gpu, four on dtn-01), each with a thread
# per CPU: the domain is hot and shared. The domain holding Mercury's CPU is avoided.
read -r HOT N <<< "$(state "" | py '" ".join(next((",".join(map(str, b["cpus"])), str(len(b["cpus"]))) for b in s["l3"] if 0 not in b["cpus"] and len(b["free"]) == len(b["cpus"])))')"
spin a $HOT $N; spin b $HOT $N
timeout 90 python3 "$CTL" --llm mock --url $URL --window-ms 500 --every 0.5 --confirm 2 --cooldown 0 --max-episodes 1 \
  --duration 60 --log $DIR/ctl.jsonl >$DIR/ctl.out 2>/dev/null
A=$(cpus_of a); B=$(cpus_of b)
[ "$A" != "$B" ]; verdict "the mock cycle separated a and b (a=$A b=$B)"
grep -q '"event": "check".*"streak": 2' $DIR/ctl.jsonl; verdict "the episode waited for two troubled checks"
grep -q '"event": "episode".*at 100%' $DIR/ctl.jsonl || grep -q '"event": "episode".*"trigger": ".*%' $DIR/ctl.jsonl
verdict "the trigger names the hot domain"
grep -q 'move_daemon' $DIR/ctl.out; verdict_s $? "the summary reports the move ($(cat $DIR/ctl.out))"

# Dry run: the same trouble, nothing applied.
post cpus "{\"daemon_id\":\"a\",\"cpus\":\"$HOT\"}" >/dev/null; post cpus "{\"daemon_id\":\"b\",\"cpus\":\"$HOT\"}" >/dev/null
HOTSPEC=$(cpus_of a)
sleep 1.2
python3 "$CTL" --llm mock --url $URL --window-ms 500 --dry-run >$DIR/dry.out 2>/dev/null
[ "$(cpus_of a)" = "$HOTSPEC" ] && [ "$(cpus_of b)" = "$HOTSPEC" ]; verdict_s $? "dry run changes nothing ($(cat $DIR/dry.out))"
for id in a b; do post kill "{\"daemon_id\":\"$id\"}" >/dev/null; done

# Pool actions: grow_pool and swap_domain; a move outside the pool is refused.
post pool '{"policy":"fixed","start":4}' >/dev/null
P0=$(curl -s $URL/pool | py 's["cpus"]')
S=$(state "")
FREE=$(echo "$S" | py 's["pool"]["free_domains"][0]')
INPOOL=$(echo "$S" | py 'next(b["name"] for b in s["l3"] if b.get("in_pool"))')
post llm/act "{\"action\":\"grow_pool\",\"l3\":\"$FREE\"}" | grep -q '"ok":true'; verdict "grow_pool adds $FREE to the pool"
[ "$(curl -s $URL/pool | py 's["cores"]')" -gt "$(echo "$P0" | tr ',' '\n' | wc -l)" ] || [ "$(curl -s $URL/pool | py 's["cpus"]')" != "$P0" ]
verdict "the pool is larger"
S=$(state "")
OUT1=$(echo "$S" | py 's["pool"]["free_domains"][0]')
post llm/act "{\"action\":\"swap_domain\",\"from\":\"$INPOOL\",\"to\":\"$OUT1\"}" | grep -q '"ok":true'; verdict "swap_domain $INPOOL -> $OUT1"
[ "$(curl -s -o /dev/null -w '%{http_code}' -X POST -d "{\"action\":\"swap_domain\",\"from\":\"$INPOOL\",\"to\":\"$OUT1\"}" $URL/llm/act)" = 409 ]
verdict "swapping a domain that already left the pool is 409"
post pool '{"policy":"fixed","start":4,"max":4}' >/dev/null
[ "$(code llm/act '{"action":"grow_pool"}')" = 409 ]; verdict "grow_pool past the pool budget is 409"
post pool '{"policy":"fixed","start":4}' >/dev/null
post llm/act '{"action":"grow_pool"}' >/dev/null
N1=$(curl -s $URL/pool | py 's["cores"]')
post llm/act '{"action":"shrink_pool"}' | grep -q '"ok":true'; verdict "shrink_pool gives the added domain back"
[ "$(curl -s $URL/pool | py 's["cores"]')" -lt "$N1" ]; verdict "the pool is smaller"
[ "$(code llm/act '{"action":"shrink_pool"}')" = 409 ]; verdict "shrink_pool below the start size is 409"
post pool '{"policy":"off"}' >/dev/null

# Shedding (llmctl --shed): the degrade action writes the policy files; llmctl only sheds its --shed routes.
post llm/act '{"action":"degrade","daemon_id":"d","disabled":"x,y"}' | grep -q '"ok":true'; verdict "degrade sets the disabled routes"
[ "$(python3 -c "import json; print(','.join(json.load(open('$DIR/d.policy'))['disabled']))")" = x,y ]; verdict "d's policy file holds x,y"
grep -q "^degrade daemon_id=d set load=llm disabled=\[x,y\]" "$LOG"; verdict "the step is logged as a degrade line"
[ "$(code llm/act '{"action":"degrade","daemon_id":"../x","disabled":"x"}')" = 400 ]; verdict "degrade with a bad daemon id is 400"
SHED=$(python3 -c "
import json, sys
sys.path.insert(0, '$ROOT/tools'); import llmctl
m = llmctl.Mercury('$URL', 500, False, None, ['x', 'y'], 'd')
out = [llmctl.run_tool(m, 'shed_route', {'route': 'hotels'})['ok'], llmctl.run_tool(m, 'shed_route', {'route': 'x'})['ok']]
out.append(json.load(open('$DIR/d.policy'))['disabled'] == ['x'])
out.append(m.state()['degrade'] == {'sheddable': ['x', 'y'], 'shed': ['x']})
out.append(llmctl.run_tool(m, 'restore_route', {'route': 'x'})['ok'] and json.load(open('$DIR/d.policy'))['disabled'] == [])
print(out)")
[ "$SHED" = "[False, True, True, True, True]" ]; verdict_s $? "llmctl refuses a route outside --shed, sheds and restores x ($SHED)"

echo "--- llm events"; grep "^llm daemon_id" "$LOG" | sed 's/^/  /'
echo "llm_it: $PASS passed, $FAIL failed"
[ "$FAIL" = 0 ]
