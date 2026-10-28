#!/bin/bash
# Ensure benchmarks/runtime/memcached/local/bin/memcached exists for the memcached-server wrapper.
# Uses /usr/bin/memcached if installed on the host; otherwise builds memcached 1.6.38 from source.
set -euo pipefail
root=$1
work=$2
bin="$root/local/bin/memcached"
if [ -x "$bin" ]; then
  exit 0
fi
if [ -x /usr/bin/memcached ]; then
  mkdir -p "$root/local/bin"
  ln -sf /usr/bin/memcached "$bin"
  exit 0
fi
ver=1.6.38
mkdir -p "$work"
tarball="$work/memcached-$ver.tar.gz"
src="$work/memcached-$ver"
if [ ! -f "$tarball" ]; then
  curl -fsSL -o "$tarball" "https://memcached.org/files/memcached-$ver.tar.gz"
fi
rm -rf "$src"
tar xzf "$tarball" -C "$work"
cd "$src"
./configure --prefix="$root/local"
make -j"$(nproc)"
make install
