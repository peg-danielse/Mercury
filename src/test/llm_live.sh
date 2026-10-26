#!/usr/bin/env bash
# Live run of tools/llmctl.py with a real model against a scratch mercury: about 3 minutes, three
# phases of 60 s with spinner daemons, then a report of every episode (trigger, model latency,
# actions) and of Mercury's placement events. Not part of ctest: it needs a model key and network.
#   phase 1  a, b: one thread each on the same CPU (a shared hot domain: one should move);
#            solo: one thread alone on its CPU (busy but not short: should be left alone)
#   phase 2  wide: 6 threads on 2 CPUs (short of CPUs, threads waiting: should gain CPUs)
#   phase 3  wide exits; p, q: 2 threads each sharing the same 2 CPUs (hot and short: split them)
# Mercury runs as root (sudo -n); llmctl runs as the calling user. The key is read from
# GEMINI_API_KEY, else one line on stdin; it is never written to disk or put in argv.
# Env: PYTHON (default python3; needs nothing beyond the stdlib for gemini), LLM (default gemini),
#      MODEL, PORT (default 18180), PHASE_S (default 60), EVERY (default 0.5), KEEP=1 keeps the logs.
set -u
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)
BIN=$ROOT/build/mercury; SPIN=$ROOT/build/test/spinner; CTL=$ROOT/tools/llmctl.py
PYTHON=${PYTHON:-python3}; LLM=${LLM:-gemini}; PORT=${PORT:-18180}; PHASE_S=${PHASE_S:-60}; EVERY=${EVERY:-0.5}
if [ "$LLM" = gemini ] && [ -z "${GEMINI_API_KEY:-}" ]; then read -r GEMINI_API_KEY; fi
export GEMINI_API_KEY
NCPU=$(nproc --all); [ "$NCPU" -ge 12 ] || { echo "llm_live needs at least 12 CPUs"; exit 1; }
URL=http://127.0.0.1:$PORT; CG=/sys/fs/cgroup/mercury; IDS="a b solo wide p q"
DIR=$(mktemp -d /tmp/mercury-live-XXXXXX); chmod 755 $DIR
post() { curl -s -m 10 -X POST -d "$2" $URL/$1 >/dev/null; }
spin() {  # spin ID CPUS THREADS: fixed cpuset, no scaling policy, so only the model moves it
  post start "{\"daemon_id\":\"$1\",\"exec_path\":\"$SPIN\",\"cpu\":${2%%[-,]*},\"env\":{\"SPIN_THREADS\":\"$3\",\"SPIN_DUTY\":\"100\"}}"
  post scale "{\"daemon_id\":\"$1\",\"policy\":\"none\"}"; post cpus "{\"daemon_id\":\"$1\",\"cpus\":\"$2\"}"
}
cleanup() {
  [ -n "${CPID:-}" ] && kill $CPID 2>/dev/null
  [ -n "${MPID:-}" ] && sudo -n kill $MPID 2>/dev/null; sleep 0.5
  for id in $IDS; do [ -d $CG/$id ] && sudo -n sh -c "echo 1 > $CG/$id/cgroup.kill; sleep 0.2; rmdir $CG/$id" 2>/dev/null; done
  if [ -n "${KEEP:-}" ]; then echo "kept $DIR"; else sudo -n rm -rf "$DIR"; fi
}
trap cleanup EXIT
sudo -n env MERCURY_RINGS=$DIR "$BIN" $PORT 0 $(seq 1 $((NCPU - 1))) >$DIR/mercury.log 2>&1 &
MPID=$!
for i in $(seq 50); do curl -s -m 1 $URL/map >/dev/null 2>&1 && break; sleep 0.1; done
C0=$(curl -s "$URL/llm/state" | python3 -c "import json,sys; print(json.load(sys.stdin)['cursor'])")
T0=$(date +%s.%N)
echo "mercury port $PORT, $NCPU CPUs, llm $LLM, ${PHASE_S}s phases, check every ${EVERY}s"

"$PYTHON" -I "$CTL" --llm $LLM ${MODEL:+--model $MODEL} --url $URL --every $EVERY --duration $((PHASE_S * 3)) \
  --log $DIR/ctl.jsonl >$DIR/ctl.out 2>/dev/null &
CPID=$!
echo "phase 1: a, b share CPU 8; solo alone on CPU 3";       spin a 8 1; spin b 8 1; spin solo 3 1
sleep $PHASE_S
echo "phase 2: wide, 6 threads on CPUs 10-11";                spin wide 10-11 6
sleep $PHASE_S
echo "phase 3: wide exits; p, q, 2 threads each on CPUs 5-6"; post kill '{"daemon_id":"wide"}'; spin p 5-6 2; spin q 5-6 2
wait $CPID; CPID=
echo "final cpusets: $(for id in a b solo p q; do printf '%s=%s ' $id "$(cat $CG/$id/cpuset.cpus 2>/dev/null)"; done)"
curl -s "$URL/llm/state?cursor=$C0" >$DIR/final.json
python3 - $DIR/ctl.jsonl $DIR/final.json $T0 <<'PY'
import json, sys
rows = [json.loads(line) for line in open(sys.argv[1])]
final = json.load(open(sys.argv[2])); t0 = float(sys.argv[3])
episodes, start, latencies = 0, None, []
for r in rows:
    if r["event"] == "episode":
        episodes += 1; start = r["t"]
        print(f"\n[{r['t'] - t0:6.1f}s] episode {episodes}: {r['trigger']}")
    elif r["event"] == "turn":
        latencies.append(r["t"] - start)
        print(f"          model {r['t'] - start:.2f}s (finish {r.get('finish')}, {r.get('input_tokens')} in / {r.get('output_tokens')} out)")
    elif r["event"] == "tool":
        res = r["result"]
        what = res.get("detail") or res.get("error") or ""
        print(f"          {r['name']} {r['input'].get('daemon_id', '')} {('ok' if r['ok'] else 'FAILED')} {what}  ({r['input'].get('reason', '')})")
    elif r["event"] == "error":
        print(f"          ERROR {r}")
checks = sum(r["event"] == "check" for r in rows)
troubled = sum(r["event"] == "check" and bool(r["trouble"]) for r in rows)
tools = [r for r in rows if r["event"] == "tool"]
print(f"\nchecks {checks} ({troubled} with trouble), episodes {episodes}, tool calls {len(tools)} "
      f"({sum(r['name'] == 'hold' for r in tools)} hold, {sum(not r['ok'] for r in tools)} failed)")
if latencies:
    latencies.sort()
    print(f"model latency: median {latencies[len(latencies) // 2]:.2f}s, max {latencies[-1]:.2f}s")
kinds = {}
for e in final.get("since", {}).get("events", []):
    kinds[e["kind"]] = kinds.get(e["kind"], 0) + 1
print("mercury events:", kinds)
PY
