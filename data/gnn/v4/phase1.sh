#!/bin/bash
# v4 phase 1: saturate dataset on exp/expraw patterns, then saturate + v3 closed loop on held-out exp seeds
set -u; M=~/Mercury; G=$M/src/test/gnn_dataset.sh; V=$M/data/gnn/v4
C="--patterns exp\ expraw --duration 18 --interval-ms 10 --phase-ms 500"
for sb in 0 100; do $G --out $V/d0-s$sb --patterns "exp expraw" --threads "2 4 8 16" --totals "1.5 3 6 10" --duration 18 --interval-ms 10 --phase-ms 500 --seed-base $sb; done
for sb in 1000 2000; do
  $G --out $V/eval-saturate-s$sb --patterns "exp expraw" --threads "4 8 16" --totals "3 6" --duration 15 --interval-ms 10 --phase-ms 500 --seed-base $sb --no-validate
  MERCURY_GNN_MODEL=$M/models/gnn_v3.json $G --out $V/eval-v3-s$sb --policy gnn --patterns "exp expraw" --threads "4 8 16" --totals "3 6" --duration 15 --interval-ms 10 --phase-ms 500 --seed-base $sb --no-validate
done
echo PHASE1_DONE
