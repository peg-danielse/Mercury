#!/bin/bash
set -u; M=~/Mercury; V=$M/data/gnn/v4
while ! grep -q PHASE1_DONE $V/phase1.log; do sleep 10; done
MERCURY_GNN_MODEL=$M/models/gnn_v4a.json $M/src/test/gnn_dataset.sh --out $V/dagger-r0 --policy gnn --patterns "exp expraw" --threads "4 8 16" --totals "2 3 6 10" --duration 15 --interval-ms 10 --phase-ms 500 --seed-base 200 --no-validate
echo R0_DONE
