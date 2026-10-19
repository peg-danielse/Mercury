#!/bin/bash
set -u; M=~/Mercury; V=$M/data/gnn/v5; G=$M/src/test/gnn_dataset.sh; MD=$M/models
while [ ! -s $MD/gnn_v5.json ] || ! grep -q DAGGER_DONE $V/r1.log 2>/dev/null; do sleep 10; done
exec 9>/tmp/mercury-v5.lock; flock 9
for sb in 1000 2000; do MERCURY_GNN_MODEL=$MD/gnn_v5.json $G --out $V/eval-v5-s$sb --policy gnn --patterns "exp expraw" --threads "4 8 16" --totals "3 6" --duration 15 --interval-ms 10 --phase-ms 500 --seed-base $sb --no-validate; done
MERCURY_GNN_MODEL=$MD/gnn_v5.json $G --out $V/oldab-v5 --policy gnn --patterns "uniform hot geometric bursty ramp" --threads "4 8 16" --totals "3 6" --duration 15 --interval-ms 10 --phase-ms 500 --no-validate
MERCURY_GNN_MODEL=$MD/gnn_v5.json $G --out $V/sng-v5 --policy gnn --workload stressng --patterns "steps ramp" --threads "2 4 6 8 12 16" --placement least_loaded --max 32 --interval-ms 10 --no-validate
echo SYNTH_DONE
POLICIES="saturate gnn-nopin@$MD/gnn_v3.json gnn-nopin@$MD/gnn_v4.json gnn-nopin@$MD/gnn_v5.json" $M/src/test/dsb_ab.sh hotel $M/data/dsb/hotel-v5 2 ""
echo ALL_DONE
