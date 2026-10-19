#!/bin/bash
# gnn-slo-v1 design v2 collection (DESIGN.md). Usage: collect2.sh hotel|social
#  sweep (training load, threadtrace 10 ms): G x3, each heavy service alone at k=1/2/4, all light daemons at 1
#  eval references (no traces): G x2 at the eval load and x2 at the default 1024 users
set -u; M=~/Mercury; S=$M/data/gnn/slo-v1; OUT=$M/data/dsb/slo-v2; B=$1
exec 9>/tmp/mercury-slo.lock; flock 9
guard() {  # wait while anything else could be running on dtn-01 (other agents included)
  while :; do
    busy=$(pgrep -af "build/mercury|goperate|deathstar-hotelReservations/bin/loadgen|operate|stress-ng|wrk|perturb|gnn_dataset|dsb_ab" | grep -v -e pgrep -e collect2.sh -e evaluate2.sh | head -3)
    ports=$(ss -ltnH | awk "{print \$4}" | grep -E ":(8080|181[89][0-9])\$")
    [ -z "$busy$ports" ] && return 0
    echo "$(date +%T) waiting: $busy $ports" | head -2; sleep 120
  done
}
case $B in
  hotel) HEAVY="attractions geo mongod profile rate recommendation reservation review search user"
         LIGHT="11211 11212 11213 11214 frontend"; UT="-users 32"; UE="-users 24"
         SL="hotel-mongod hotel-loadgen";;
  social) HEAVY="PostStorageService SocialGraphService HomeTimelineService rabbitmq-server mongod"
          LIGHT="11311 11312 11313 ComposePostService MediaService TextService UniqueIdService UrlShortenService UserMentionService UserService UserTimelineService WriteHomeTimelineService redis-server"
          UT="-users 16"; UE="-users 12"; SL="social-mongod social-loadgen social-rabbitmq-server";;
esac
fixed() { local k=$1 s=""; for d in $LIGHT; do s="$s,$d=$k"; done; echo ${s#,}; }
G="-svc-min 3 -svc-max 3 -svc-fixed $(fixed 2)"
run() {  # name users svc trace
  local D=$OUT/$B/$1; guard; mkdir -p $D; echo "$(date +%T) RUN $B $1 :: $3"
  python3 $S/stamp_windows.py $D & local WP=$!
  POLICIES=saturate SVC_ARGS="$3" TRACEALL=$4 TRACE_SLOW="$SL" $M/src/test/dsb_ab.sh $B $D 1 "$2" > $D/driver.log 2>&1
  echo "$(date +%T) END $B $1 rc=$? $(ls ~/Benchmarks/runtime/redis/6379/ | head -2)"
  touch $D/.stamp_stop; wait $WP; rm -f $D/.stamp_stop
}
for r in 1 2 3; do run G-r$r "$UT" "$G" 10; done
for d in $HEAVY; do for k in 1 2 4; do run $d-k$k "$UT" "$G,$d=$k" 10; done; done
run light-k1 "$UT" "-svc-min 3 -svc-max 3 -svc-fixed $(fixed 1)" 10
for r in 1 2; do run ref-eval-G-r$r "$UE" "$G" ""; done
for r in 1 2; do run ref-1024-G-r$r "" "$G" ""; done
echo COLLECT_DONE $B
