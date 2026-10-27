#!/bin/bash
# Install what cmake needs to build Mercury and the DeathStarBench services (Debian or Ubuntu).
#
#   tools/setup/install-deps.sh [PREFIX]        PREFIX defaults to ~/opt
#   DRY_RUN=1 tools/setup/install-deps.sh       print the steps without running them
#
# apt (needs sudo): build tools, Boost, OpenSSL, nlohmann-json, yaml-cpp, libmemcached, libevent, Thrift 0.19, Go.
# Built from source into PREFIX (no sudo), each only if missing, from pinned tags:
#   mongo-c-driver 2.5.6   libmongoc 2 (pkg-config mongoc2; Debian ships only libmongoc 1.x)
#   AMQP-CPP v4.3.27       libamqpcpp (Debian does not package it)
# Writes PREFIX/env.sh: source it before cmake so pkg-config, find_library and the compiler see PREFIX.
# The other dependencies (hiredis, cpp_redis, jaeger, rabbitmq-c, ...) are fetched by cmake at pinned tags.
set -euo pipefail

PREFIX=${1:-${PREFIX:-$HOME/opt}}
RUN=${DRY_RUN:-}
WORK=${WORK:-$HOME/src/mercury-deps}

say() { echo "== $*"; }
run() { if [ -n "$RUN" ]; then echo "+ $*"; else "$@"; fi; }

APT_PKGS=(build-essential cmake make pkg-config git curl ca-certificates
  libboost-all-dev libssl-dev nlohmann-json3-dev libyaml-cpp-dev
  libmemcached-dev libevent-dev libthrift-dev golang-go)

install_apt() {
  say "apt packages"
  if ! command -v apt-get >/dev/null; then
    echo "apt-get not found: install these by hand: ${APT_PKGS[*]}" >&2
    return 1
  fi
  run sudo apt-get update
  run sudo apt-get install -y "${APT_PKGS[@]}"
}

# fetch URL DEST: a tarball extracted into WORK/NAME, or a git tag cloned shallowly.
fetch_tar() { # name url
  local dir=$WORK/$1
  if [ ! -d "$dir" ]; then
    run mkdir -p "$WORK"
    run curl -fsSL -o "$WORK/$1.tar.gz" "$2"
    run tar xzf "$WORK/$1.tar.gz" -C "$WORK"
  fi
  echo "$dir"
}

# build NAME CHECKFILE SOURCE_DIR [cmake args...]: cmake install into PREFIX/NAME unless CHECKFILE exists
build() {
  local name=$1 check=$2 src=$3; shift 3
  if [ -e "$PREFIX/$name/$check" ]; then
    say "$name: already in $PREFIX/$name"
    return
  fi
  say "$name: building into $PREFIX/$name"
  run cmake -S "$src" -B "$src/build-install" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX/$name" "$@"
  run cmake --build "$src/build-install" -j"$(nproc)"
  run cmake --install "$src/build-install"
}

install_mongoc() {
  local ver=2.5.6
  local src
  src=$(fetch_tar mongo-c-driver-$ver \
    "https://github.com/mongodb/mongo-c-driver/releases/download/$ver/mongo-c-driver-$ver.tar.gz")
  build mongoc-$ver lib/pkgconfig/mongoc2.pc "$src" \
    -DENABLE_TESTS=OFF -DENABLE_EXAMPLES=OFF -DENABLE_MAN_PAGES=OFF -DENABLE_SHM_COUNTERS=OFF
}

install_amqpcpp() {
  local tag=v4.3.27 src=$WORK/AMQP-CPP-4.3.27
  if [ ! -d "$src" ]; then
    run git clone --depth 1 --branch $tag https://github.com/CopernicaMarketingSoftware/AMQP-CPP.git "$src"
  fi
  build amqpcpp-4.3.27 lib/libamqpcpp.so "$src" \
    -DAMQP-CPP_BUILD_SHARED=ON -DAMQP-CPP_LINUX_TCP=ON -DAMQP-CPP_BUILD_EXAMPLES=OFF \
    -DCMAKE_INSTALL_LIBDIR=lib
}

check_go() {
  local want=1.24 have
  have=$(go version 2>/dev/null | awk '{print $3}' | sed 's/^go//')
  if [ -z "$have" ]; then
    echo "go not found after apt (goperate needs Go $want or later)" >&2
    return 1
  fi
  if [ "$(printf '%s\n%s\n' "$want" "$have" | sort -V | head -1)" != "$want" ]; then
    echo "go $have is older than $want (benchmarks/deathstar-socialNetwork/goperate/go.mod)" >&2
    return 1
  fi
  say "go $have"
}

write_env() {
  run mkdir -p "$PREFIX"
  if [ -n "$RUN" ]; then echo "+ write $PREFIX/env.sh"; return; fi
  cat > "$PREFIX/env.sh" <<EOF
# Source before cmake: PREFIX holds the libraries built by tools/setup/install-deps.sh.
export PKG_CONFIG_PATH="$PREFIX/mongoc-2.5.6/lib/pkgconfig:\${PKG_CONFIG_PATH:-}"
export CMAKE_PREFIX_PATH="$PREFIX/amqpcpp-4.3.27:\${CMAKE_PREFIX_PATH:-}"
export CPATH="$PREFIX/amqpcpp-4.3.27/include:\${CPATH:-}"
export LD_LIBRARY_PATH="$PREFIX/amqpcpp-4.3.27/lib:\${LD_LIBRARY_PATH:-}"
EOF
  say "wrote $PREFIX/env.sh"
}

install_apt
check_go
install_mongoc
install_amqpcpp
write_env
say "done. Next: source $PREFIX/env.sh && cmake -S . -B build && cmake --build build"
