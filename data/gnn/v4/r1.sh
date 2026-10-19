#!/bin/bash
set -u; M=~/Mercury; V=$M/data/gnn/v4
sleep 20; exec 9>/tmp/mercury-v4.lock; flock 9
MERCURY_GNN_MODEL=$M/models/gnn_v4b.json $M/src/test/gnn_dataset.sh --out $V/dagger-r1 --policy gnn --patterns "exp expraw" --threads "4 8 16" --totals "2 3 6 10" --duration 15 --interval-ms 10 --phase-ms 500 --seed-base 300 --no-validate
echo R1_DONE
