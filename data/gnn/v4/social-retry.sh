#!/bin/bash
M=~/Mercury; exec 9>/tmp/mercury-v4.lock; flock 9
for p in gnn-nopin@$M/models/gnn_v4.json gnn-nopin@$M/models/gnn_v3.json; do sleep 60; POLICIES="$p" $M/src/test/dsb_ab.sh social $M/data/dsb/social-v4-retry 1 ""; done
echo RETRY_DONE
