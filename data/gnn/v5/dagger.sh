#!/bin/bash
# v5 DAgger round: $1 = round name, $2 = model, $3 = seed base. Balanced: 30 old-pattern + 30 exp cases.
set -u; M=~/Mercury; V=$M/data/gnn/v5; G=$M/src/test/gnn_dataset.sh
exec 9>/tmp/mercury-v5.lock; flock 9
MERCURY_GNN_MODEL=$2 $G --out $V/dagger-$1-old --policy gnn --patterns "uniform hot geometric bursty ramp" --threads "4 8 16" --totals "2 4.5" --duration 15 --interval-ms 10 --phase-ms 500 --seed-base $3 --no-validate
MERCURY_GNN_MODEL=$2 $G --out $V/dagger-$1-exp --policy gnn --patterns "exp expraw" --threads "4 8 16" --totals "2 3 4.5 6 10" --duration 15 --interval-ms 10 --phase-ms 500 --seed-base $3 --no-validate
echo DAGGER_DONE $1
