# Running the infrabench policies on Mercury

Branch `infrabench-policies` ports the controller baselines from infrabench
`rca/controller/policy/` onto a Mercury worker, following the contract in
`policies.md`. This file is how to build it on a Linux server and reproduce
infrabench-style results (one scored adaptation per policy arm).

## Policy map

| infrabench `--policy` | status in infrabench | Mercury form on this branch |
| --- | --- | --- |
| `none` | developed (control arm) | no `/policies`, no `/scale` |
| `scale_nodes_ind` (CMT20) | developed: add a worker when one node's 75 s mean CPU > 0.80 | `shift` with `trigger: "mean"`, `theta: 0.80`: a running daemon leaves a group whose mean load over the last five 1 s intervals is above 0.80. Mercury does not start machines; leaving the overloaded group is the equivalent (`policies.md` Baselines) |
| `reactive_move` (WSVY07, Sandpiper) | developed: hotspot = 3 of the last 5 samples > 0.75, move to least-loaded legal node | `shift` with `trigger: "hotspot"`, `util: 0.75`: same test on five 1 s intervals, cpuset rewritten onto the least-loaded legal group (same CPU count, same pid) |
| `degrade_optional` (XDB17, XNTB19) | developed: shed the first optional function on a node above 0.80 | `degrade`: operator-listed optional components with a cost; disabled costliest-first until `U - cost(disabled) <= 0.80`, restored cheapest-first; written to `rings/<daemon_id>.policy`, read by the app via `MERCURY_POLICY` |
| `scale_out` | developed: 2 replicas, hostname pin kept | `/scale {"min":2,"max":2}` (saturate) plus `restrict.ceiling` = the group the daemon sits in, so the 2nd CPU comes from the same group |
| `scale_out_spread` | developed: drop pin, 2 replicas | `/scale {"min":2,"max":2}` with no ceiling: 2nd CPU from the least-loaded core anywhere in the pool |
| `combined` | developed: scale 0.80, move 0.85, degrade 0.90 | one document: `shift` `trigger: "either"` (`theta: 0.80`, `util: 0.85`) + `degrade` `limit: 0.90` |
| `zeroctl` (ours) | LLM path in `adapt.py`; `decide()` raises NotImplementedError | **not ported.** The harness reports it as skipped |
| scale nodes, aggregate | planned (Baselines table / `policies.md`), no infrabench module | not built; documented only |
| least allocated | planned, no infrabench module | pure `least_allocated()` decision in `src/policy.cpp` + test; the `widen` grant around it is **not built** (`POST /policies` with `widen` returns HTTP 501) |
| tune app | planned, no infrabench module | built per `policies.md` (`tune.parameters` ladders, written to the policy file). Treat it as experimental: infrabench has no reference implementation to compare against |

Thresholds come from the infrabench modules (`THETA`, `HOTSPOT_UTIL`,
`HOTSPOT_HITS`, `UTIL_LIMIT`, the combined staggering). The window is
five 1 s intervals from the resource-group rings instead of 75 s of
Prometheus, as `policies.md` specifies (the ring holds 60 s).

## Build

```bash
sudo apt-get update && sudo apt-get install -y g++ cmake make python3 git
git clone https://github.com/peg-danielse/Mercury.git && cd Mercury
git checkout infrabench-policies
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
(cd build && ctest --output-on-failure)     # policy_test: unprivileged unit tests
```

Needs Linux with cgroup v2 mounted at `/sys/fs/cgroup` with the `cpuset`
controller available, and root (Mercury writes `/sys/fs/cgroup/mercury/...`).
`-march=native` is set by the CMake file, so build on the server you run on.

## Reproduce the results (all arms)

```bash
sudo python3 scripts/policy_experiment.py --label server-$(date +%Y%m%d-%H%M)
```

Per arm (~55 s with defaults, ~7 min for all arms) it:

1. starts `build/mercury 18200` (all online CPUs; CPU 0 is Mercury's own),
2. starts `--services` (2) `hello_server` daemons on the victim group, each with
   `MERCURY_POLICY` pointing at its policy file (component `extra` = an
   optional half burn on every `/cpu` request),
3. attaches the arm's `/policies` and/or `/scale` document,
4. drives closed-loop `/cpu` load (`--clients` per service) and warms up (`--warmup` 15 s),
5. injects the fault: the `burn` stressor (2 threads per victim CPU) pinned to the victim group,
6. waits `--hold` (15 s, stabilization), then scores `--measure` (20 s) with the fault still up.

Level: `--level auto` uses NUMA nodes when the host has 2 or more, otherwise L3
domains, otherwise cores. Pin it with `--level node` to match `policies.md`
exactly. `--victim node.1` picks the injected group. Use `--cpus 0,1,2,...` to
give Mercury only part of the machine. `--arms reactive_move,combined` runs a subset.

Quick smoke run (about 30 s per arm):

```bash
sudo python3 scripts/policy_experiment.py --label smoke --warmup 8 --hold 8 --measure 8
```

## Outputs

`output/<label>/` (git-ignored):

- `summary.csv`: one row per arm: `policy, level, victim, actions, baseline_p50_us,
  baseline_p95_us, p50_us, p95_us, error_rate, requests, throughput_rps, cpus_before, cpus_after, note`
- `summary.json`: the full rows
- `<arm>/adaptations.jsonl` and `<arm>/last_adaptation.json`: the same fields as infrabench
  `rca/output/<label>/controller/adaptations.jsonl`: `policy`, `applied` (Mercury policy
  events: `move` with `from`/`to`/`cpus`, `degrade`/`restore` with `component`, `tune`),
  `errors`, `services`, `cost_before`, `cost_after`, `score {p50_us, p95_us, error_rate,
  latency_traces}`, plus `baseline` (pre-fault window) and `stabilization`
- `<arm>/policies.json`: `GET /policies` at the end (documents, five interval loads per group, events)
- `<arm>/mercury.log`: the worker log (`policy {...}` lines are the events)

What to expect: `none` shows the fault as a p95 rise against its baseline.
`reactive_move`/`scale_nodes_ind`/`combined` show `move` events and p95 back near
baseline once the services left the victim group. `degrade_optional` shows a
`degrade` event for `extra` and lower latency (smaller requests); cost stays at the
same CPUs. `scale_out*` raise `cpus_after` to 2 per service; `scale_out` keeps the
second CPU on the victim group (so it still competes with the fault),
`scale_out_spread` does not. With a single NUMA node and one L3 the arms run at
core level; that exercises the same rules on smaller groups.

Smoke run on an 8-CPU single-node VM (core level, 8 s windows; that VM's
`/proc/stat` per-CPU accounting is virtualized, so expect cleaner numbers on bare metal):

```
policy,level,victim,actions,baseline_p50_us,baseline_p95_us,p50_us,p95_us,error_rate,requests,throughput_rps,cpus_before,cpus_after
none,core,core.1,0,95814,110171,146797,167314,0.0,216,27.0,2,2
reactive_move,core,core.1,6,52363,108612,53788,110941,0.0,448,56.0,2,2
degrade_optional,core,core.1,2,75125,107896,100853,118236,0.0,317,39.62,2,2
combined,core,core.1,11,34715,105634,34415,99785,0.0,655,81.88,2,2
scale_out,core,core.1,0,25730,53767,48872,55561,0.0,645,80.62,2,4
scale_out_spread,core,core.1,0,49448,55666,71960,82864,0.0,440,55.0,2,4
```

## Driving it by hand

```bash
sudo MERCURY_RINGS=/tmp/rings ./build/mercury 18200 &
curl -s -XPOST localhost:18200/start -d '{"daemon_id":"svc","exec_path":"'$PWD'/build/test/hello_server","cpu":1,"listen_port":18300,"env":{"MERCURY_POLICY":"/tmp/rings/svc.policy"}}'
# reactive_move
curl -s -XPOST localhost:18200/policies -d '{"daemon_id":"svc","shift":{"trigger":"hotspot","util":0.75,"level":"node"}}'
# combined
curl -s -XPOST localhost:18200/policies -d '{"daemon_id":"svc","shift":{"trigger":"either","util":0.85,"theta":0.8},"degrade":{"limit":0.9,"components":[{"name":"extra","cost":0.3}]}}'
curl -s localhost:18200/policies          # documents, interval loads, events
curl -s -XPOST localhost:18200/policies -d '{"daemon_id":"svc"}'   # detach
```

Document fields: `shift` (`true` or `{trigger: hotspot|mean|either, util, theta, level}`),
`restrict` (`{level, ceiling}`), `degrade` (`{components:[{name,cost}], limit, level}`),
`tune` (`{parameters:[{name, values:[...]}], limit}`), `exec_path`. Unknown fields are
HTTP 400; `widen` is HTTP 501.

## Differences from infrabench, on purpose

- No RCA in the loop: infrabench applies the baseline to the services the isolation
  forest blames; here the documents are attached to every service daemon up front, and
  `policy_loop` acts continuously (one action per group per five intervals) instead of
  once per anomaly batch.
- Unit of capacity is a resource group (node/L3/core), unit that moves is a daemon cpuset;
  "replicas" are CPUs in the cpuset.
- `policy_loop` is started from `main` (`start_policy_loop()`) right after `init_resources`,
  not from inside `init_resources`.
- Not built from `policies.md`: the grant (`widen`, `max_groups`, grant groups dropping
  after five empty intervals) and the L3 refinement inside the destination node on a shift.
