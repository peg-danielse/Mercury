#!/bin/bash
# gnn-slo-v1 hotel sweep with Jaeger (DESIGN.md v3). Training load -users 32, threadtrace 10 ms, Jaeger 2 % sampling.
# warm-up run (discarded) ; two passes: G, then each heavy service alone at k=1/2/4, light daemons all at 1; G again
# at mid-pass and pass end (drift check). Then G x2 at the hotel eval load (-users 24) and x2 at 1024 (no threadtrace).
set -u; M=~/Mercury; S=$M/data/gnn/slo-v1; OUT=$M/data/dsb/slo-v3; B=hotel
JG="-jaeger -jaeger-port-offset 20000 -jaeger-cpus 62 -trace-sample 0.02"
exec 9>/tmp/mercury-slo.lock; flock 9
guard() {
  while :; do
    busy=$(pgrep -af "build/mercury|goperate|deathstar-hotelReservations/bin/loadgen|operate|stress-ng|wrk|perturb|gnn_dataset|dsb_ab|jaeger/bin/jaeger" | grep -v -e pgrep -e collect3.sh -e evaluate3.sh | head -3)
    ports=$(ss -ltnH | awk "{print \$4}" | grep -E ":(8080|181[89][0-9]|26831|26832|34268|34250|24317|24318|36686|36685|33133)\$")
    [ -z "$busy$ports" ] && return 0
    echo "$(date +%T) waiting: $busy $ports" | head -2; sleep 120
  done
}
HEAVY="attractions geo mongod profile rate recommendation reservation review search user"
LIGHT="11211 11212 11213 11214 frontend"; UT="-users 32"; SL="hotel-mongod hotel-loadgen"
fixed() { local k=$1 s=""; for d in $LIGHT; do s="$s,$d=$k"; done; echo ${s#,}; }
G="-svc-min 3 -svc-max 3 -svc-fixed $(fixed 2)"
run() {  # name users svc trace
  local D=$OUT/$B/$1; guard; mkdir -p $D; echo "$(date +%T) RUN $B $1 :: $3"
  python3 $S/stamp_windows.py $D & local WP=$!
  POLICIES=saturate SVC_ARGS="$3 $JG" TRACEALL=$4 TRACE_SLOW="$SL" $M/src/test/dsb_ab.sh $B $D 1 "$2" > $D/driver.log 2>&1
  local rc=$?; echo "$(date +%T) END $B $1 rc=$rc traces=$(cat $D/*/1-saturate/traces.csv 2>/dev/null | wc -l)"
  touch $D/.stamp_stop; wait $WP; rm -f $D/.stamp_stop
}
run warmup "$UT" "$G" 10
for p in 1 2; do
  run G-p$p-a "$UT" "$G" 10; i=0
  for d in $HEAVY; do
    for k in 1 2 4; do run $d-k$k-p$p "$UT" "$G,$d=$k" 10; done
    i=$((i+1)); [ $i = 5 ] && run G-p$p-b "$UT" "$G" 10
  done
  run light-k1-p$p "$UT" "-svc-min 3 -svc-max 3 -svc-fixed $(fixed 1)" 10
done
run G-p2-c "$UT" "$G" 10
for r in 1 2; do run ref-eval-G-r$r "-users 24" "$G" ""; done
for r in 1 2; do run ref-1024-G-r$r "" "$G" ""; done
echo COLLECT_DONE hotel
