#!/bin/bash
# Build stress-ng inside benchmarks/stress-ng.
# If a config.h copied from another machine enables headers this libc does not ship
# (e.g. termio.h), clean the generated config before running make.
set -euo pipefail
root=$1
if [ -f "$root/config.h" ] && grep -q '^#define HAVE_TERMIO_H' "$root/config.h" && [ ! -f /usr/include/termio.h ]; then
  make -C "$root" clean cleanconfig
fi
# A small job count: this make runs inside the cmake build, which is already parallel.
make -C "$root" -j4
