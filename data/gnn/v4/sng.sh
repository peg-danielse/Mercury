#!/bin/bash
# stress-ng <=16 workers, saturate | gnn with model $2 ; serialized by flock
set -u; M=~/Mercury; V=$M/data/gnn/v4; NAME=$1; MODEL=${2:-}
exec 9>/tmp/mercury-v4.lock; flock 9
if [ -z "$MODEL" ]; then $M/src/test/gnn_dataset.sh --out $V/sng-$NAME --workload stressng --patterns "steps ramp" --threads "2 4 6 8 12 16" --placement least_loaded --max 32 --interval-ms 10 --no-validate
else MERCURY_GNN_MODEL=$MODEL $M/src/test/gnn_dataset.sh --out $V/sng-$NAME --policy gnn --workload stressng --patterns "steps ramp" --threads "2 4 6 8 12 16" --placement least_loaded --max 32 --interval-ms 10 --no-validate; fi
echo SNG_DONE $NAME
