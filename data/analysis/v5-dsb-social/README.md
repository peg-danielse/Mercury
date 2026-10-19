Social-network, no pin, least_loaded, 2 repeats (src/test/dsb_ab.sh social, same protocol as dsb-social-cmp), after the Redis
state fix (wrapper --save "" --appendonly no, goperate PING-until-not-LOADING, old dump deleted). 6/6 runs started (336 routes each).
Raw: dtn-01 ~/Mercury/data/dsb/social-v5/20261018-174651. Caveat: MongoDB (runtime/mongodb/27017, 3.6 GB) still persists between runs.
