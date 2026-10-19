#!/bin/bash
set -u; M=~/Mercury; V=$M/data/gnn/v4; G=$M/src/test/gnn_dataset.sh; V4=$M/models/gnn_v4.json; V3=$M/models/gnn_v3.json
exec 9>/tmp/mercury-v4.lock; flock 9
for sb in 1000 2000; do MERCURY_GNN_MODEL=$V4 $G --out $V/eval-v4-s$sb --policy gnn --patterns "exp expraw" --threads "4 8 16" --totals "3 6" --duration 15 --interval-ms 10 --phase-ms 500 --seed-base $sb --no-validate; done
MERCURY_GNN_MODEL=$V4 $G --out $V/oldab-v4 --policy gnn --patterns "uniform hot geometric bursty ramp" --threads "4 8 16" --totals "3 6" --duration 15 --interval-ms 10 --phase-ms 500 --no-validate
MERCURY_GNN_MODEL=$V4 $G --out $V/sng-v4 --policy gnn --workload stressng --patterns "steps ramp" --threads "2 4 6 8 12 16" --placement least_loaded --max 32 --interval-ms 10 --no-validate
echo SYNTH_DONE
POLICIES="saturate gnn-nopin@$V3 gnn-nopin@$V4" $M/src/test/dsb_ab.sh hotel $M/data/dsb/hotel-v4 2 ""
POLICIES="saturate gnn-nopin@$V3 gnn-nopin@$V4" $M/src/test/dsb_ab.sh social $M/data/dsb/social-v4 2 ""
echo ALL_DONE
