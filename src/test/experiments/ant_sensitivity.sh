#!/bin/bash
# Sensitivity of hotel, social, and media to each injected antagonist, at a low rate.
# Five saturate repeats per application, on a fixed pool of 8, the operator's
# autoscaler left as it is. The schedule level is 100 (the low end of the
# 100/120/150 cycle): hotels, read-home-timeline, or compose-review, and the
# operator's dsb mix sets the other routes. After a 60 s warmup the run records
# 120 s with no antagonist, then same, smt, l3, and membw, level 1 then level 2,
# 60 s on and 30 s off. Every phase hits the daemon that was busiest during the
# baseline. A repeat that already has DONE is skipped.
# Output: data/dsb/compare/ant-sensitivity-20261110/<app>
set -u
M=$(cd "$(dirname "$0")/../../.." && pwd)
BASE=${BASE:-$M/data/dsb/compare/ant-sensitivity-20261110}
LOG=$BASE/campaign.log
mkdir -p "$BASE"

say() { echo "$(date -Is) $*" | tee -a "$LOG"; }

settle() {
  sleep 2
  if ss -ltnH | grep -q ':8080 '; then
    say "stopping leftover mercury"
    sudo -n pkill -f "$M/build/mercury " || true
    sleep 2
  fi
  if pgrep -x mongod >/dev/null; then
    say "stopping leftover mongod"
    sudo -n pkill -x mongod || true
    sleep 2
  fi
  if pgrep -x stress-ng >/dev/null; then
    say "stopping leftover stress-ng"
    sudo -n pkill -x stress-ng || true
    sleep 1
  fi
}

routes_of() {
  case $1 in
    hotel) echo "hotels,recommendations,user,review,restaurants,museums,cinema,reservation";;
    social) echo "compose-post,read-home-timeline,read-user-timeline,follow";;
    media) echo "read-page,compose-review,read-movie-reviews";;
  esac
}

REPEATS=${REPEATS:-5}
say "ant sensitivity: hotel, social, media at level 100, $REPEATS repeats, baseline then each kind"
if [ -z "${SKIP_SNAPSHOT:-}" ]; then
  SNAP=$("$M/scripts/backup.sh") || { say "snapshot failed"; exit 1; }
  say "snapshot $SNAP"
fi
for app in hotel social media; do
  settle
  say "start $app"
  env SKIP_SNAPSHOT=1 DB=seed SYSTEMS=saturate POOL_START=8 REPEATS="$REPEATS" \
    LEVELS=100 ROUTE="$(routes_of "$app")" \
    WARMUP=60 BASELINE=120 PHASES=8 OFF=30 ON=60 \
    KINDS=same,smt,l3,membw SEQUENTIAL=1 PIN_BUSIEST=1 VICTIMS= \
    "$M/src/test/compare_systems.sh" "$BASE/$app" "$app" || { say "FAILED $app"; exit 1; }
  for r in $(seq 1 "$REPEATS"); do
    if [ ! -f "$BASE/$app/saturate/r$r/DONE" ]; then
      say "FAILED $app: no r$r DONE"
      exit 1
    fi
  done
  say "end $app"
done
say "report"
python3 "$M/src/test/ant_sensitivity_report.py" "$BASE" | tee "$BASE/report.txt"
say "campaign finished"
