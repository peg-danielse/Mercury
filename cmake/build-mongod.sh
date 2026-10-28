#!/bin/bash
# Ensure benchmarks/runtime/mongodb/bin/mongod exists for the mongod wrapper.
# Uses /usr/bin/mongod if installed on the host; otherwise downloads MongoDB 7.0.16 into bin/.
set -euo pipefail
root=$1
work=$2
bin="$root/bin/mongod"
if [ -x "$bin" ]; then
  exit 0
fi
if [ -x /usr/bin/mongod ]; then
  mkdir -p "$root/bin"
  ln -sf /usr/bin/mongod "$bin"
  exit 0
fi
ver=7.0.16
archive="mongodb-linux-x86_64-ubuntu2204-$ver"
mkdir -p "$work" "$root/bin"
tarball="$work/$archive.tgz"
if [ ! -f "$tarball" ]; then
  curl -fsSL -o "$tarball" "https://fastdl.mongodb.org/linux/$archive.tgz"
fi
tar xzf "$tarball" --strip-components=2 -C "$root/bin" "$archive/bin/mongod" "$archive/bin/mongos"
chmod +x "$root/bin/mongod" "$root/bin/mongos"
