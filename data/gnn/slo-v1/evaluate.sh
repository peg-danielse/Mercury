#!/bin/bash
# gnn-slo-v1 evaluation: saturate, v3, v5, gnn-slo-v1 (no pin, least_loaded, 2 repeats, dsb_ab.sh protocol, -users 1024)
set -u; M=~/Mercury; S=$M/data/gnn/slo-v1; OUT=$M/data/dsb/slo-v1; MD=$M/models
[ -s $MD/gnn_slo_v1.json ] || { echo "missing gnn_slo_v1.json"; exit 1; }
exec 9>/tmp/mercury-slo.lock; flock 9
guard() { while :; do
    busy=$(pgrep -af "build/mercury|goperate|deathstar-hotelReservations/bin/loadgen|operate|stress-ng|wrk|perturb|gnn_dataset|dsb_ab" | grep -v -e pgrep -e collect.sh -e evaluate.sh | head -3)
    ports=$(ss -ltnH | awk "{print \$4}" | grep -E ":(8080|181[89][0-9])\$")
    [ -z "$busy$ports" ] && return 0; echo "$(date +%T) waiting: $busy $ports" | head -2; sleep 120; done; }
for B in hotel social; do guard; mkdir -p $OUT/eval-$B
  python3 $S/stamp_windows.py $OUT/eval-$B & WP=$!
  POLICIES="saturate gnn-nopin@$MD/gnn_v3.json gnn-nopin@$MD/gnn_v5.json gnn-nopin@$MD/gnn_slo_v1.json" $M/src/test/dsb_ab.sh $B $OUT/eval-$B 2 ""
  touch $OUT/eval-$B/.stamp_stop; wait $WP; rm -f $OUT/eval-$B/.stamp_stop
done
echo EVAL_DONE
