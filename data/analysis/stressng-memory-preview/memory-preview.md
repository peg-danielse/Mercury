# stress-ng memory preview (fixed cpuset, policy none, 4 workers, 20 s)
| case | cpuset | bogo-ops/s | numastat other_node % | numa_hit by node (delta) |
|---|---|---|---|---|
| vm --vm 4 --vm-bytes 1G | 4-7 (node1) | 442,420 | 0.00 | node1 816k |
| vm --vm 4 --vm-bytes 1G | 4-5,8-9 (node1+2) | 499,374 | 0.00 | node1 279k, node2 278k |
| stream --stream 4 | 4-7 (node1) | 3,954 | 0.00 | <100k (allocated before trace) |
| stream --stream 4 | 4-5,8-9 (node1+2) | 2,870 | 0.00 | <100k |
