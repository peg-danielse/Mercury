#!/bin/bash
# Split a social seed's single mongod snapshot into one data directory per database.
#
#   social_mongo_split.sh SEED
#
# SEED/mongodb/ is a combined dbpath. The script dumps it and restores each database onto its own port, then writes
# SEED/mongodb/<port>/ and moves the combined files to SEED/mongodb.combined/. Ports stay clear of hotel (27017) and
# media (27018-27026). A seed that already has mongodb/27040/ is left alone. Another mongod (the hotel seek) may be
# running; this only starts ports that are free.
#
# mongodump and mongorestore are taken from benchmarks/runtime/mongodb/bin.
set -eu
SEED=${1:-}
[ -n "$SEED" ] && [ -d "$SEED" ] || { echo "usage: social_mongo_split.sh SEED"; exit 2; }
SEED=$(realpath "$SEED")
M=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)
B=$M/benchmarks
BIN=$B/runtime/mongodb/bin
WRAP=$B/runtime/mongodb/mongod
SRC_PORT=27140
# social-graph, user (also user-mention), user-timeline, post-storage, url-shorten.
PORTS="27040 27041 27042 27043 27044"
dbs_for() {
  case $1 in
    27040) echo social-graph;;
    27041) echo user;;
    27042) echo user-timeline;;
    27043) echo post;;
    27044) echo url-shorten;;
    *) echo "no databases for port $1" >&2; exit 1;;
  esac
}

if [ -d "$SEED/mongodb/27040" ]; then
  echo "$SEED is already split (mongodb/27040)"
  exit 0
fi
[ -d "$SEED/mongodb/journal" ] || { echo "$SEED/mongodb is not a combined mongod data directory"; exit 1; }
[ -x "$BIN/mongodump" ] && [ -x "$BIN/mongorestore" ] || { echo "mongodump and mongorestore are missing in $BIN"; exit 1; }
for p in $SRC_PORT $PORTS; do
  if ss -ltnH | grep -q ":$p "; then
    echo "port $p is busy"
    exit 1
  fi
done

work=$(mktemp -d)
cleanup() {
  if [ -n "${MPID:-}" ] && kill -0 "$MPID" 2>/dev/null; then
    kill -TERM "$MPID" 2>/dev/null || true
    wait "$MPID" 2>/dev/null || true
  fi
  rm -rf "$work" "$B/runtime/mongodb/$SRC_PORT"
}
trap cleanup EXIT

start_mongo() {
  port=$1
  rm -rf "$B/runtime/mongodb/$port/db"
  mkdir -p "$B/runtime/mongodb/$port/db"
  if [ -n "${2:-}" ]; then
    rsync -a "$2/" "$B/runtime/mongodb/$port/db/"
  fi
  "$WRAP" --advertise-port "$port" > "$work/mongod-$port.log" 2>&1 &
  MPID=$!
  for _ in $(seq 150); do
    if ss -ltnH | grep -q ":$port "; then
      return 0
    fi
    kill -0 "$MPID" 2>/dev/null || { echo "mongod $port exited"; tail -20 "$work/mongod-$port.log"; exit 1; }
    sleep 0.2
  done
  echo "mongod $port did not listen"; tail -20 "$work/mongod-$port.log"; exit 1
}
stop_mongo() {
  port=$1
  kill -TERM "$MPID"
  wait "$MPID" 2>/dev/null || true
  MPID=""
  for _ in $(seq 50); do
    ss -ltnH | grep -q ":$port " || return 0
    sleep 0.2
  done
  echo "mongod $port still listening"; exit 1
}

echo "dumping $SEED/mongodb through port $SRC_PORT"
start_mongo $SRC_PORT "$SEED/mongodb"
"$BIN/mongodump" --uri "mongodb://127.0.0.1:$SRC_PORT" --out "$work/dump"
stop_mongo $SRC_PORT

known=" social-graph user user-timeline post url-shorten "
for db in "$work/dump"/*; do
  name=$(basename "$db")
  case $name in
    admin|local|config) continue;;
  esac
  case "$known" in
    *" $name "*) ;;
    *) echo "combined seed has database $name, which no social mongod is assigned"; exit 1;;
  esac
done

for port in $PORTS; do
  echo "restoring port $port ($(dbs_for $port))"
  start_mongo $port
  for db in $(dbs_for $port); do
    if [ ! -d "$work/dump/$db" ]; then
      echo "  $db is not in the seed"
      continue
    fi
    "$BIN/mongorestore" --uri "mongodb://127.0.0.1:$port" --nsInclude "$db.*" --drop "$work/dump"
  done
  stop_mongo $port
done

mv "$SEED/mongodb" "$SEED/mongodb.combined"
mkdir -p "$SEED/mongodb"
for port in $PORTS; do
  mkdir -p "$SEED/mongodb/$port"
  rsync -a "$B/runtime/mongodb/$port/db/" "$SEED/mongodb/$port/"
done
echo "split $SEED into mongodb/{${PORTS// /,}}. combined data kept at $SEED/mongodb.combined"
