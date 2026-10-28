#!/bin/bash
# Build the hotel operator, experiment runner, and every service into deathstar-hotelReservations/bin.
set -euo pipefail
root=$1
out=$2
export GOTOOLCHAIN=local
mkdir -p "$out"
cd "$root"
go build -mod=vendor -o "$out/loadgen" ./operate
go build -mod=vendor -o "$out/experiment" ./experiment
for dir in "$root"/cmd/*; do
  [ -d "$dir" ] || continue
  name=$(basename "$dir")
  go build -mod=vendor -o "$out/$name" "./cmd/$name"
done
