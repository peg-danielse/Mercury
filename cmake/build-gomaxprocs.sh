#!/bin/bash
# Build the GOMAXPROCS server and its operator into gomaxprocs/bin.
set -euo pipefail
root=$1
export GOTOOLCHAIN=local
mkdir -p "$root/bin"
cd "$root"
go build -o bin/server ./cmd/server
go build -o bin/operate ./operate
