#!/bin/bash
# The first launch skipped the model sweeps because model_seek.sh is not
# executable. Luna's three seeks are already running; these two passes start
# once that process exits and the machine is clear.
set -u
M=$(cd "$(dirname "$0")/../../.." && pwd)
BASE=${BASE:-$M/data/dsb/compare}
LOG=${LOG:-$BASE/seek-repeats-20261105.log}
PARENT=${1:?parent pid}
mkdir -p "$BASE"

echo "$(date -Is) model sweeps r2 r3 will start after pid $PARENT" | tee -a "$LOG"
while kill -0 "$PARENT" 2>/dev/null; do
  sleep 30
done

settle() {
  sleep 2
  if ss -ltnH | grep -q ':8080 '; then
    echo "$(date -Is) stopping leftover mercury" | tee -a "$LOG"
    sudo -n pkill -f "$M/build/mercury " || true
    sleep 2
  fi
  if pgrep -x mongod >/dev/null; then
    echo "$(date -Is) stopping leftover mongod" | tee -a "$LOG"
    sudo -n pkill -x mongod || true
    sleep 2
  fi
}
settle

for rep in r2 r3; do
  echo "$(date -Is) model seek $rep" | tee -a "$LOG"
  env TAG=model-seek-20261105-$rep LOG="$LOG" \
    bash "$M/src/test/experiments/model_seek.sh" || \
    echo "$(date -Is) model seek $rep failed" | tee -a "$LOG"
  settle
done
echo "$(date -Is) model sweeps r2 r3 finished" | tee -a "$LOG"
