#!/bin/bash
# gnn-slo-v1 data collection (DESIGN.md). Usage: collect.sh train|ref
#  train: -users 768, fixed k sweep + saturate + v5, threadtrace 10 ms (TRACEALL)
#  ref:   default -users 1024, fixed k sweep only (lowest-p95 reference for evaluation), no traces
set -u; M=~/Mercury; S=$M/data/gnn/slo-v1; OUT=$M/data/dsb/slo-v1; PHASE=$1; KS=${KS:-1 2 3 4 8}
exec 9>/tmp/mercury-slo.lock; flock 9
guard() {  # wait while anything else could be running on dtn-01 (other agents included)
  while :; do
    busy=$(pgrep -af "build/mercury|goperate|deathstar-hotelReservations/bin/loadgen|operate|stress-ng|wrk|perturb|gnn_dataset|dsb_ab" | grep -v -e pgrep -e collect.sh -e evaluate.sh | head -3)
    ports=$(ss -ltnH | awk "{print \$4}" | grep -E ":(8080|181[89][0-9])\$")
    [ -z "$busy$ports" ] && return 0
    echo "$(date +%T) waiting: $busy $ports" | head -2; sleep 120
  done
}
run() {  # bench name policies users svc
  local B=$1 N=$2 P=$3 U=$4 SV=$5 T=""; guard
  [ "$PHASE" = train ] && T=10
  case $B in hotel) SL="hotel-mongod hotel-loadgen";; social) SL="social-mongod social-loadgen social-rabbitmq-server";; esac
  mkdir -p $OUT/$PHASE-$B-$N
  python3 $S/stamp_windows.py $OUT/$PHASE-$B-$N & WP=$!
  POLICIES="$P" SVC_ARGS="$SV" TRACEALL=$T TRACE_SLOW="$SL" $M/src/test/dsb_ab.sh $B $OUT/$PHASE-$B-$N 1 "$U"
  touch $OUT/$PHASE-$B-$N/.stamp_stop; wait $WP; rm -f $OUT/$PHASE-$B-$N/.stamp_stop
  ls ~/Benchmarks/runtime/redis/6379/ | head -2
}
U=""; [ "$PHASE" = train ] && U="-users 768"
for B in hotel social; do
  for k in $KS; do run $B k$k saturate "$U" "-svc-min $k -svc-max $k"; done
  if [ "$PHASE" = train ]; then run $B saturate saturate "$U" ""; run $B v5 "gnn-nopin@$M/models/gnn_v5.json" "$U" ""; fi
done
echo COLLECT_DONE $PHASE
