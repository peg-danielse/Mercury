#!/bin/bash
# goperate is both the operator (bin/goperate) and the load generator (bin/loadgen).
set -euo pipefail
root=$1
export GOTOOLCHAIN=local
mkdir -p "$root/bin"
cd "$root"
go build -mod=vendor -o bin/goperate .
cp -af bin/goperate bin/loadgen
