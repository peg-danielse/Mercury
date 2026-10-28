#!/bin/bash
# The Go operator (social: goperate; media: mediaoperate) is also the load generator (bin/loadgen).
set -euo pipefail
root=$1
name=${2:-goperate}
export GOTOOLCHAIN=local
mkdir -p "$root/bin"
cd "$root"
go build -mod=vendor -o "bin/$name" .
cp -af "bin/$name" bin/loadgen
