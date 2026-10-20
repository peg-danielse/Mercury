#!/bin/bash
# Seed snapshot for a DSB app: start the app on an empty MongoDB, fill it, stop it, and copy MongoDB's data directory
# and redis's dump.rdb into SNAP. dsb_ab.sh SEED=SNAP restores it before every run, so every run starts from the same
# filled state whatever the previous run wrote (posts, reservations, timelines).
#
#   dsb_seed.sh social|hotel SNAP [--posts N]
#
# social: goperate seed (register the -graph users and follows, compose N posts per user, wait for the home-timeline
# fan-out, SAVE redis) with every service up. hotel: operate start then stop (the services insert their data on start).
# Both apps share MongoDB's data directory (runtime/mongodb/27017/db); the directory in place is moved aside to
# db.aside-<time> (kept), so the snapshot holds only this app's seed. The seeded state is left in place.
# Env: DSB_ROOT [/home/paul/Benchmarks]  POOL [1-60]  PORT [8080]
set -u
APP=$1; SNAP=$(realpath -m "$2"); shift 2
POSTS=10; [ "${1:-}" = --posts ] && POSTS=$2
B=${DSB_ROOT:-/home/paul/Benchmarks}; M=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)
POOL=${POOL:-$(seq 1 60)}; PORT=${PORT:-8080}
MDB=$B/runtime/mongodb/27017/db; RDIR=$B/runtime/redis/6379
case $APP in
  social) WD=$B/deathstar-socialNetwork; OP=$WD/goperate/bin/goperate; FILL="-seed-posts $POSTS seed";;
  hotel)  WD=$B/deathstar-hotelReservations; OP=$WD/bin/loadgen; FILL="start";;
  *) echo "usage: dsb_seed.sh social|hotel SNAP [--posts N]"; exit 2;;
esac
[ -e "$SNAP" ] && { echo "$SNAP exists"; exit 1; }
pgrep -x mongod >/dev/null || pgrep -x redis-server >/dev/null && { echo "mongod or redis-server is running"; exit 1; }
ss -ltnH | grep -q ":$PORT " && { echo "port $PORT busy"; exit 1; }
TS=$(date +%Y%m%d-%H%M%S)
[ -d "$MDB" ] && sudo -n mv "$MDB" "$MDB.aside-$TS" && echo "moved $MDB aside to $MDB.aside-$TS"
[ -f "$RDIR/dump.rdb" ] && sudo -n mv "$RDIR/dump.rdb" "$RDIR/dump.rdb.aside-$TS"
mkdir -p "$SNAP"
(cd "$WD" && exec sudo -n env MERCURY_RINGS=$SNAP/rings $M/build/mercury $PORT 0 $POOL) > "$SNAP/mercury.log" 2>&1 &
sleep 1; MPID=$(pgrep -f "^$M/build/mercury $PORT 0" | head -1)
for i in $(seq 50); do curl -s -m 1 127.0.0.1:$PORT/ping >/dev/null && break; sleep 0.2; done
echo "$(date +%T) $APP: $FILL"
(cd "$WD" && DSB_ROOT=$B $OP -mercury 127.0.0.1:$PORT -cpu 2 $FILL) > "$SNAP/fill.log" 2>&1; rc=$?
[ "$APP" = hotel ] && [ $rc = 0 ] && { sleep 5; (cd "$WD" && DSB_ROOT=$B $OP -mercury 127.0.0.1:$PORT stop) >> "$SNAP/fill.log" 2>&1; rc=$?; }
tail -3 "$SNAP/fill.log"
sudo -n kill -TERM "$MPID"; for i in $(seq 50); do kill -0 "$MPID" 2>/dev/null || break; sleep 0.2; done
for c in /sys/fs/cgroup/mercury/$APP-*; do [ -d "$c" ] && echo 1 | sudo -n tee "$c/cgroup.kill" >/dev/null; done
sleep 2; pgrep -x mongod >/dev/null && { echo "mongod still running: snapshot not taken"; exit 1; }
[ $rc = 0 ] || { echo "fill failed (rc $rc): snapshot not taken; see $SNAP/fill.log"; exit 1; }
sudo -n mkdir -p "$SNAP/mongodb" "$SNAP/redis"
sudo -n rsync -a "$MDB/" "$SNAP/mongodb/"
[ -f "$RDIR/dump.rdb" ] && sudo -n cp -p "$RDIR/dump.rdb" "$SNAP/redis/dump.rdb"
sudo -n chown -R "$(id -u):$(id -g)" "$SNAP"
cat > "$SNAP/meta.txt" <<EOF
app $APP seeded $(date -Is) on $(hostname); fill: $FILL
mongodb $(du -sh "$SNAP/mongodb" | cut -f1) (from $MDB), redis $(du -sh "$SNAP/redis" 2>/dev/null | cut -f1) (from $RDIR)
mercury $(git -C "$M" log --oneline -1)
restore: dsb_ab.sh with SEED=$SNAP
EOF
cat "$SNAP/meta.txt"
