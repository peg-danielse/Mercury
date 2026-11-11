#!/bin/bash
# One repeat of hotel, then social, then media, repeated until STOP_AT.
# Same arms as social_eval.sh / media_eval.sh / hotel_eval.sh. KINDS adds load,
# so the antagonist list includes the 1.5x and 2x request-rate phases
# (interfere.py --kinds, level 1 and 2 of kind load).
set -u
M=$(cd "$(dirname "$0")/../../.." && pwd)
export KINDS=same,smt,l3,membw,load
STOP_AT=${STOP_AT:-2026-11-01 12:00}
MAX=${MAX:-8}
BASE=$M/data/dsb/compare
echo "$(date -Is) night eval: hotel, social, media, up to $MAX rounds, stop before $STOP_AT, kinds $KINDS"
for r in $(seq 1 "$MAX"); do
  now=$(date +%s)
  stop=$(date -d "$STOP_AT" +%s)
  if [ "$now" -ge "$stop" ]; then
    echo "$(date -Is) STOP_AT $STOP_AT reached before round $r"
    break
  fi
  echo "$(date -Is) round $r"
  env OUT=$BASE/hotel-eval-20261101 FROM=$r MAX=$r STOP_AT="$STOP_AT" "$M/src/test/experiments/hotel_eval.sh"
  env OUT=$BASE/social-eval-20261101 FROM=$r MAX=$r STOP_AT="$STOP_AT" "$M/src/test/experiments/social_eval.sh"
  env OUT=$BASE/media-eval-20261101 FROM=$r MAX=$r STOP_AT="$STOP_AT" "$M/src/test/experiments/media_eval.sh"
done
echo "$(date -Is) night eval finished"
