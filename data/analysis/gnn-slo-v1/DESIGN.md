# gnn-slo-v1: per-thread GNN trained for low p95, low over-allocation and few changes on DSB (design v3, 2026-10-19)

Same model and C++ forward pass as v3/v5 (LocalGNN_DB, 17 features, K=3, H=16, 1,633 params, cpuset input zeroed),
initialised from `gnn_v5.json`. Output `models/gnn_slo_v1.json` (non-default, `MERCURY_GNN_MODEL`).
Trained on **hotel only**; social is an unseen test system. History:
- v1 (lowest p95 of a uniform-k sweep at 768 users) was dropped after the review check (REVIEW-CHECK.txt): 768 users is far
  past the knee, the min-of-windows reference was noisier than repeats of any configuration, p95 is log-bucketed (~8.7 %
  steps) and fallbacks to the largest k inflated labels (mean 2.3 cores with k <= 3, memcached at 0.1 core labelled 2.35).
  Its data (slo-v1, 768 users) is not used.
- v2 (below-knee per-service sweep, route-set attribution, no Jaeger; data `data/dsb/slo-v2`, 09:28-10:16) was stopped
  for v3 and is **not used for training** (`NOT-JAEGER.txt`). What it showed: the first two runs after an idle box were
  ~25 % slower than every later run (warm-up), and single runs scatter by up to ~30 %; v3 adds a discarded warm-up run,
  5 G repeats spread over the sweep and 2 passes of every sweep configuration.
- v3 (this): v2 + Jaeger critical-path attribution.

## Loads (below the knee)
Knee (throughput stops growing, latency ~ population/throughput): hotel ~8-32 clients/route, social ~16 (saturate, 1024 ramp).
The operator ramps users 1, 2, 4, ..., max, ..., 1 (4 x 1 s windows per level).
- Training (hotel): `-users 32`. Evaluation: hotel `-users 24` (that top level is never in training), social `-users 12`,
  and the default 1024 for both.

## Jaeger
Jaeger v2.22.0 all-in-one (`~/Benchmarks/jaeger`, operator patches applied to the working trees), flags
`-jaeger -jaeger-port-offset 20000 -jaeger-cpus 62 -trace-sample 0.02` (bare mode, 2 % probabilistic sampling, ports
24317-36686 on 127.0.0.1, none in 8080/18180-18199; CPU 62 is outside the Mercury pool 1-60, shared with threadtrace helpers).
Used in every sweep and evaluation run for every policy, so its overhead is the same everywhere. Per window the operator
writes `traces.csv` (window x route x daemon): crit_share (CRISP critical-path share of request time), crit_share_tail
(same for requests >= the route's p90), excl_ms_*, sample counts.

## Allocation sweep (hotel, one service at a time)
Operators take `-svc-fixed name=k,...` (per-program /scale min = max) and `-svc-min/-svc-max` for the rest.
- Generous configuration G: services with meaningful CPU (>= ~0.2 cores below the knee) at 3 cores, the others at 2.
  Hotel heavy: attractions, geo, mongod, profile, rate, recommendation, reservation, review, search, user; light:
  memcached 11211-11214, frontend (40 of 60 pool CPUs). Social G (evaluation reference only): PostStorageService,
  SocialGraphService, HomeTimelineService, rabbitmq-server, mongod at 3; the other 13 at 2. Split from per-daemon usage in
  the below-knee part of saturate runs (hotel-v5 / social-v5).
- `collect3.sh`: 1 warm-up run (discarded), then 2 passes of [G, each heavy service alone at k = 1, 2, 4 with a G run
  after the fifth service, all light daemons at 1], then a final G: 5 G runs, 2 runs of every sweep configuration.
  Then G x2 at the hotel evaluation load and x2 at 1024 (evaluation references, no threadtrace).
- Sweep runs record threadtrace at 10 ms on every daemon (mongod and loadgen at 50 ms: labelled, not used for training),
  `dsb_sample.py` (100 ms), window stamps (`slo/stamp_windows.py`, CLOCK_MONOTONIC) and traces.csv.

## Reference, attribution and label (`slo/slo_ref3.py`)
- p95 per window and route from the operator `route` lines. ref(L, r) = median over all G windows of level L (5 runs x 4).
- rel(run, L, r) = median of the run's 4 window p95s at L / ref(L, r).
- Critical-path attribution: w(run, L, r, d) = d's crit_share_tail on route r at level L in that run (sample-weighted over the
  level's windows; the G median if the run sampled no request of r at L). Routes with w < 0.02 are ignored.
  score(run, d, L) = sum_r w rel / sum_r w: a route whose p95 rose is charged to the daemons on its tail critical path, in
  proportion to their share, instead of averaging over every route that touches d. A daemon never on a critical path at L
  gets score 1 (its allocation does not move p95). Combined with the per-service sweep: in run `<d>-k<k>` only d's
  allocation changed, so its score measures the effect of d at k on the requests d is critical for.
- Margin m = 20 % (>= 2 p95 buckets); G-vs-G leave-one-out scatter is reported. C*(d, L) = smallest k (1, 2, 4 or G's) whose
  median score over the passes (G: over the 5 G runs) is <= 1.2. If none is, the level is unlabelled for d (no fallback).
  Light daemons: C* = 1 if the all-light-at-1 runs meet the margin, else 2 (G).
- Critical-path shares as slow edge weights: not used. The per-thread GNN has no daemon-to-daemon edges, so there is no
  edge to weight; the shares enter only through the attribution above.

## Loss (per daemon, per 10 ms tick inside a labelled level)
P = sum of thread predictions (the controller FFD-packs them; FFD >= ceil(P)), T = C* - 0.3.
L = (1 + e) relu(T - P)^2 + lambda_alloc relu(P - T) + lambda_chg * 100 * relu(|P(t) - P(t-1)| - 0.25) + mu mean_i (p_i - y_i)^2
- e = max(0, score - 1) of the run and level the tick came from (p95 above the reference weighs under-allocation more).
- lambda_alloc relu(P - T): over-allocation in cores per tick (core-seconds).
- Change term: the model is stateless, so changes/s is a train-time temporal-consistency hinge on |P(t) - P(t-1)| with a
  0.25-core deadband, x100 ticks/s (per-second rate). The controller is unchanged (20-tick scale-down hold).
- y_i = min(1, T (run_i + 0.01) / sum(run + 0.01)), mu = 0.1.
- lambda_alloc: largest in {0.03, 0.1, 0.3, 1} with held-out under-allocation rate (FFD(P) < C*) <= 1.5x that of 0.03
  (lambda_chg = 0). lambda_chg: then the largest in {0, 0.01, 0.03, 0.1} with under-allocation <= 1.2x (+0.5 pt) and
  core-seconds <= 1.05x of lambda_chg = 0.
- Cases: the swept daemon's trace in every sweep run, every daemon in the G runs, the light daemons in light-k1 runs
  (10 ms traces only). Held-out: pass 2's final G run (G-p2-c, all daemons) and all pass-2 runs of two services.

## Evaluation (`evaluate3.sh`)
saturate, v3 and gnn-slo-v1 (no pin, least_loaded, 2 repeats, Jaeger on for every policy) at the evaluation load and at
1024: hotel first, then social (unseen). p95 relative to G at the same load (hotel: from `collect3.sh`; social: G x2 per
load, run by `evaluate3.sh` before the social evaluation, evaluation only). Metrics: throughput, p95, p95/ref, p99,
core-seconds allocated and used, allocation changes/s, errors. Redis starts empty; MongoDB is not reset (caveat).

## What was used (2026-10-19, filled in after the sweep)
- Sweep: 72 runs 10:18-12:01 CEST (warm-up + 2 passes + 5 G + 4 eval references), every run rc=0, ~1,270 traces.csv rows per run.
- Attribution: tail critical-path weights with drift control on (default; `--no-control` gives mean label 1.21 instead of 1.12).
  The drift control was added after pass 1 showed whole-run p95 shifts of up to ~1.4x on routes that do not involve the swept service.
- Labels: 100 % of daemon-levels labelled (no unlabelled level); mean 1.12 cores; 154 x 1, 2 x 2, 9 x 3 (geo and user at
  8-32 clients, mongod at two levels). G leave-one-out median p95/ref 0.96-1.10 (G-p1-b is the slow one).
  Note: for geo and user, k = 4 also fails the margin while k = 3 (G) passes, so their 3s come from G being the only passing
  configuration, not from a monotone k-curve.
- Training: 114 train / 20 held-out cases (499k / 87k ticks), 20 epochs per fit, init v5.
  lambda_alloc = 0.03 picked (0.1 had 3.2 % under-allocation > 1.5 x 1.1 %); lambda_chg = 0.1 picked (under 1.36 %, core-s
  -2 %, changes/s 21.4 -> 4.5). Held-out: v5 under 7.9 %, over 54.5 %, 2062 core-s, 30.7 changes/s; gnn-slo-v1 under 1.36 %,
  over 52 %, 1866 core-s (label 1018, used 615), 4.5 changes/s (tick-level requested cores, before the controller hold).
