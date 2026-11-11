#!/bin/bash
# Split a media seed's single mongod snapshot into one data directory per database.
#
#   media_mongo_split.sh SEED
#
# SEED/mongodb/ is a combined dbpath (the layout from before one-mongod-per-database). The script dumps it and
# restores each database onto the docker-compose host port, then writes SEED/mongodb/<port>/ and moves the combined
# files to SEED/mongodb.combined/. social-graph is restored with movie-info, because MovieInfoHandler opens it on
# the movie-info client. A seed that already has mongodb/27018/ is left alone.
#
# mongodump and mongorestore are taken from benchmarks/runtime/mongodb/bin, and downloaded there when missing.
set -eu
SEED=${1:-}
[ -n "$SEED" ] && [ -d "$SEED" ] || { echo "usage: media_mongo_split.sh SEED"; exit 2; }
SEED=$(realpath "$SEED")
M=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)
B=$M/benchmarks
BIN=$B/runtime/mongodb/bin
WRAP=$B/runtime/mongodb/mongod
SRC_PORT=27118
# port, then the databases that belong on it. 27023 is unused in docker-compose.yml.
PORTS="27018 27019 27020 27021 27022 27024 27025 27026"
dbs_for() {
  case $1 in
    27018) echo movie-id;;
    27019) echo user;;
    27020) echo review;;
    27021) echo user-review;;
    27022) echo movie-review;;
    27024) echo cast-info;;
    27025) echo plot;;
    27026) echo "movie-info social-graph";;
    *) echo "no databases for port $1" >&2; exit 1;;
  esac
}

if [ -d "$SEED/mongodb/27018" ]; then
  echo "$SEED is already split (mongodb/27018)"
  exit 0
fi
[ -d "$SEED/mongodb/journal" ] || { echo "$SEED/mongodb is not a combined mongod data directory"; exit 1; }
pgrep -x mongod >/dev/null && { echo "mongod is running: stop it before splitting"; exit 1; }

if [ ! -x "$BIN/mongodump" ] || [ ! -x "$BIN/mongorestore" ]; then
  ver=100.10.0
  arc=mongodb-database-tools-ubuntu2204-x86_64-$ver
  tmp=$(mktemp -d)
  echo "downloading $arc"
  curl -fsSL -o "$tmp/tools.tgz" "https://fastdl.mongodb.org/tools/db/$arc.tgz"
  tar xzf "$tmp/tools.tgz" -C "$tmp"
  mkdir -p "$BIN"
  cp "$tmp/$arc/bin/mongodump" "$tmp/$arc/bin/mongorestore" "$BIN/"
  chmod +x "$BIN/mongodump" "$BIN/mongorestore"
  rm -rf "$tmp"
fi

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
  kill -TERM "$MPID"
  wait "$MPID" 2>/dev/null || true
  MPID=""
  for _ in $(seq 50); do
    pgrep -x mongod >/dev/null || return 0
    sleep 0.2
  done
  echo "mongod still running"; exit 1
}

echo "dumping $SEED/mongodb through port $SRC_PORT"
start_mongo $SRC_PORT "$SEED/mongodb"
"$BIN/mongodump" --uri "mongodb://127.0.0.1:$SRC_PORT" --out "$work/dump"
stop_mongo

known=" movie-id user review user-review movie-review cast-info plot movie-info social-graph "
for db in "$work/dump"/*; do
  name=$(basename "$db")
  case $name in
    admin|local|config) continue;;
  esac
  case "$known" in
    *" $name "*) ;;
    *) echo "combined seed has database $name, which no media mongod is assigned"; exit 1;;
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
  stop_mongo
done

mv "$SEED/mongodb" "$SEED/mongodb.combined"
mkdir -p "$SEED/mongodb"
for port in $PORTS; do
  mkdir -p "$SEED/mongodb/$port"
  rsync -a "$B/runtime/mongodb/$port/db/" "$SEED/mongodb/$port/"
done
echo "split $SEED into mongodb/{${PORTS// /,}}. combined data kept at $SEED/mongodb.combined"
