#!/bin/bash
# gnn-slo-v1 evaluation (DESIGN.md v2): saturate, v3, gnn-slo-v1; no pin, least_loaded, 2 repeats;
# Jaeger on in every run (all policies). eval load (hotel -users 24, social -users 12) and default 1024; hotel first, then social (unseen in training).
# Social has no sweep: its p95 reference = the generous configuration G, 2 repeats per load (evaluation only). No traces.
set -u; M=~/Mercury; S=$M/data/gnn/slo-v1; OUT=$M/data/dsb/slo-v3-eval; JG="-jaeger -jaeger-port-offset 20000 -jaeger-cpus 62 -trace-sample 0.02"
exec 9>/tmp/mercury-slo.lock; flock 9
guard() {
  while :; do
    busy=$(pgrep -af "build/mercury|goperate|deathstar-hotelReservations/bin/loadgen|operate|stress-ng|wrk|perturb|gnn_dataset|dsb_ab|jaeger/bin/jaeger" | grep -v -e pgrep -e collect3.sh -e evaluate3.sh | head -3)
    ports=$(ss -ltnH | awk "{print \$4}" | grep -E ":(8080|181[89][0-9]|26831|26832|34268|34250|24317|24318|36686|36685|33133)\$")
    [ -z "$busy$ports" ] && return 0
    echo "$(date +%T) waiting: $busy $ports" | head -2; sleep 120
  done
}
one() {  # dir policies reps users svc
  local D=$1; guard; mkdir -p $D; echo "$(date +%T) RUN $D"
  python3 $S/stamp_windows.py $D & local WP=$!
  POLICIES="$2" SVC_ARGS="$5 $JG" $M/src/test/dsb_ab.sh $B $D $3 "$4" > $D/driver.log 2>&1
  echo "$(date +%T) END $D rc=$? redis: $(ls ~/Benchmarks/runtime/redis/6379/ | head -2)"
  touch $D/.stamp_stop; wait $WP; rm -f $D/.stamp_stop
}
P="saturate gnn-nopin@$M/models/gnn_v3.json gnn-nopin@$M/models/gnn_slo_v1.json"
SOCIAL_LIGHT="11311 11312 11313 ComposePostService MediaService TextService UniqueIdService UrlShortenService UserMentionService UserService UserTimelineService WriteHomeTimelineService redis-server"
SG="-svc-min 3 -svc-max 3 -svc-fixed $(for d in $SOCIAL_LIGHT; do printf "%s=2," $d; done | sed 's/,$//')"
for B in hotel social; do
  case $B in hotel) UE="-users 24";; social) UE="-users 12";; esac
  for LD in eval 1024; do
    U=""; [ $LD = eval ] && U="$UE"
    if [ $B = social ]; then for r in 1 2; do one $M/data/dsb/slo-v3/social/ref-$LD-G-r$r saturate 1 "$U" "$SG"; done; fi
    one $OUT/$B-$LD "$P" 2 "$U" ""
  done
done
echo EVAL_DONE
