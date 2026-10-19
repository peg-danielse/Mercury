# Policies

This file is the contract for resource and application policies beyond `saturate`. `make_policy` still returns only `SaturatePolicy`.

Status on branch `infrabench-policies`: `policy_loop`, `POST /policies`, `GET /policies`, `shift`, `degrade`, `tune`, and `restrict.ceiling` are built (see `RUNNING_POLICIES.md`). `shift` additionally accepts `{trigger, util, theta, level}` so the `scale_nodes_ind` (mean) and `reactive_move` (hotspot) rows and the `combined` thresholds can be expressed. The grant (`widen`, `max_groups`, grant groups leaving after five empty intervals) is not built: `widen` answers HTTP 501. `least_allocated` exists as a pure decision only. `zeroctl` (an external model step, `MERCURY_ZEROCTL`) is described in `RUNNING_POLICIES.md`.

A policy watches load that Mercury already records and changes one of three things: which resource groups a daemon may use, the CPUs in its cpuset, or a small file the application reads. The process stays running. The pid stays the same. Policies do not submit daemons, do not register workers, and do not start computers.

The group tree, the rings, `load_of`, `request_cpu`, `add_core`, `remove_core`, and `SaturatePolicy::decide` stay as `SPEC.md` describes them. A daemon with no policy document keeps today's saturate behavior: `request_cpu` may return any allocatable CPU.

## Baselines

The baseline table uses a cluster node as the unit of capacity and a virtual machine as the unit that moves. On a Mercury worker the unit of capacity is a resource group (`socket`, `node`, `l3`, `core` from `init_resources`), and the unit that moves is a running daemon. A NUMA node is `ResourceLevel::Node`. A computer joins the system when its worker registers.

| Baseline | What it does there | What Mercury does |
| --- | --- | --- |
| none | No adaptation | No `/scale` and no `/policies` |
| scale nodes, individual | Add a machine when one node's 75 s mean load exceeds 0.8 | `shift` leaves a hot NUMA node. No policy starts a machine |
| scale nodes, aggregate | Add a machine when the mean of node loads exceeds 0.8 | Each NUMA node is judged on its own by `shift` |
| reactive move | A hotspot of 3 intervals in the last 5 above 0.75 moves the workload to the least-loaded legal node | `shift` rewrites the daemon cpuset onto the least-loaded legal NUMA node |
| least allocated | Place on the group with the most unallocated requested capacity | `widen` picks the group to add by unallocated allocatable CPUs |
| scale out | Raise a replica count from load | `saturate` adds one CPU to the running process. Another copy is another submit |
| degrade optional | Disable optional parts until load minus their cost is at or below 0.8 | `degrade` writes the disabled set for that program |
| tune app | Choose parameters for the workload | `tune` steps a declared ladder for that program |
| combined | Apply the fixed rules together | One loop runs the attached rules in the order below |

`shift` keeps the hotspot test: five intervals, hot when load is above 0.75, a hotspot when at least three are hot. The destination is the legal node with the lowest load. `degrade` keeps the brownout test: measured load minus the cost of disabled optional parts is brought down to 0.8 or below. Critical parts stay enabled. The 75 s machine-autoscaler window does not fit the resource-group ring, which holds 60 s, so the policies below use five 1 s intervals taken from that ring.

## Grant

A daemon with a policy document has a grant: the resource groups whose allocatable CPUs it may run on. The grant has a level, one of `core`, `l3`, `node`, or `socket`. At attach, and again after a successful `shift`, the grant is every group at that level which contains a CPU of the current cpuset.

`add_core` keeps calling `request_cpu`. When a grant is present, the exclude list is the daemon's current cpuset plus every allocatable CPU that belongs to no granted group. `request_cpu` is unchanged, so a granted CPU another daemon already runs on can still be returned. `remove_core` still drops the highest CPU in the cpuset and does not look at the grant.

The ceiling is an optional ancestor group named by the operator. Every granted group must sit inside it. `widen` and `shift` both refuse a destination outside the ceiling. Attach succeeds only when every CPU already in the cpuset belongs to that ceiling. A daemon restricted to `node.0` can gain L3 domains and cores inside `node.0`. It stays on `node.0` while that node is hot; `degrade` and `tune` are the adaptations left on that daemon.

A group other than the last one leaves the grant when the cpuset has contained none of its CPUs for five intervals. The last group stays. Emptied groups are the result of `remove_core` dropping CPUs, so the grant follows the cpuset back down and does not take a CPU away by itself.

Without `restrict` and without `widen`, there is no grant, and `add_core` passes only the current cpuset as `exclude`.

## Adding a group

`widen` adds one group to the grant when all three hold:

- Over the last 1 s interval, the daemon meets the saturate add test: occupancy at least 0.90 and more busy threads than CPUs in the cpuset. Occupancy is the same measurement as `measure_age`.
- Every allocatable CPU in the grant is already in the cpuset.
- The grant has fewer groups than `max_groups`, and a legal group remains inside the ceiling.

The legal groups are the groups at the grant level that are inside the ceiling, are not yet granted, and contain at least one allocatable CPU. `least_allocated` picks among them. The score of a group is the number of its allocatable CPUs that appear in no running daemon cpuset, divided by the number of its allocatable CPUs. A group with no allocatable CPU scores 0. The highest score wins. An equal score keeps the lower `load_of`, and an equal load keeps the lower Linux CPU, the same tie break as `least_loaded`.

`widen` then adds that group to the grant and appends one CPU from it: the CPU `request_cpu` would return if exclude contained every allocatable CPU outside that group. The cpuset write is the same path as `add_core` (`ensure_daemon_cpuset`, `write_sysfs`, `cpuset_applied`). Further CPUs inside the new group come from later `saturate` adds. One `widen` per daemon per five intervals. A successful widen sets `added_at_ns`, so `SaturatePolicy::decide` waits for a fresh window.

`max_groups` defaults to the number of groups at that level inside the ceiling. The default level, when `widen` is set and `restrict` is omitted, is `core`.

## Moving off a hot node

`shift` is the policy that moves a running daemon off an overloaded NUMA node. It rewrites `cpuset.cpus` for the existing pid. It does not fork, and it does not use `migrate_route`: `migrate_route` replaces the cpuset with one CPU, and a shift keeps the daemon's current CPU count.

`policy_loop` wakes once a second. For each `Node` group it takes the samples of the last five 1 s stretches from that group's ring. Interval load is `busy` divided by `busy + idle + iowait` over the samples whose timestamps fall in that second, the same ratio as `load_of`. A second with no samples is a missing interval. `node_hot` is true when at least three of the five loads are above 0.75. Fewer than five intervals means the node is cold.

For a hot node, the candidates are running daemons with `shift` set, whose cpuset intersects that node's `linux_cpus`, whose ceiling contains some other node, and whose `shifted_at_ns` is at least five intervals old. The rank is the sum of `task-clock` on the daemon's thread rings over that last 1 s. A daemon with no `task-clock` sample in that second is skipped. One daemon moves: the highest rank. An equal rank keeps the smaller `daemon_id`.

The destination is a `Node` group other than the source. It is legal when it is not itself hot, it lies inside the daemon's ceiling, and its allocatable CPUs can supply the daemon's current CPU count. Among legal nodes, the lowest interval load wins, with the `least_loaded` tie break. No legal node means this daemon stays, and the next candidate on the same node is tried in rank order.

The new cpuset has the same number of CPUs. They are chosen on the destination node, inside the least-loaded L3 on that node when the grant level is `core` or `l3`, and anywhere on that node when the grant level is `node` or `socket`. Each CPU is the result of `request_cpu` limited to the CPUs still eligible for this move. If those CPUs come up short, the daemon stays and the next candidate is tried. The chosen CPUs are written in one `cpuset.cpus` update. On success the stored `cpu` becomes the lowest of them, the grant becomes the groups at the grant level that contain the new cpuset, `added_at_ns` and `shifted_at_ns` are set, and the daemon is not eligible again for five intervals.

At most one daemon leaves a given node per five intervals. A cpuset that already spans two nodes is a candidate on each hot node it touches, and a successful shift places the whole set on the destination, so the daemon stops spanning. The worker's own CPU is never written. A failed `cpuset_applied` leaves the previous cpuset in place and logs the daemon id; the next wake may try again.

A daemon with `shift` and a ceiling of its current node never moves. The ceiling is the restriction. `shift` is how a daemon that is allowed to leave a node actually leaves it.

## Application

`degrade` and `tune` are per daemon because the component names and the parameter ladders belong to that program. The worker checks `exec_path` on attach and rejects a document whose `exec_path`, if present, differs from the path the daemon was started with.

The worker writes `rings/<daemon_id>.policy`, with `daemon_id` passed through `archive_name` and the directory from `rings_dir`. The write is a temp file in that directory renamed into place. The application reads the file on its own schedule. Mercury does not signal the process and does not change its environment after `exec`. The path is well known, and `POST /policies` repeats it in the response so a later start can pass it in `env` as `MERCURY_POLICY`.

The file is JSON:

```json
{"components":[{"name":"trace","enabled":false}],"parameters":{"concurrency":4}}
```

Names match `[A-Za-z_][A-Za-z0-9_]*`. On attach the file is written immediately with every component enabled and every parameter at the last entry of its ladder.

`degrade` lists optional components, each with a `cost` in `(0, 1]`. Anything the operator leaves off the list stays enabled; that is the critical part. The load `U` is the latest 1 s interval load of the ceiling group, or of the NUMA node that contains the cpuset when no ceiling is set. A cpuset that spans several groups at that level uses the maximum of their interval loads.

`degrade_set` disables optional components in descending `cost` until `U` minus the cost of the disabled set is at or below 0.8. One component changes per five intervals. A disabled component is enabled again, smallest cost first, when all five intervals satisfy that inequality with its cost added back. The brownout costs are the operator's fractions. Mercury subtracts them from the measured load and does not observe the inside of the process.

`tune` lists parameters. Each has `values`, ordered from the smallest footprint to the largest, with at least two entries. The step starts at the last entry. While the latest interval has `U` above 0.8, or `degrade` has a component disabled, `tune_step` moves one parameter one entry toward the front of its list. While all five intervals have `U` at or below 0.8 and every optional component is enabled, it moves one parameter one entry toward the end. One parameter steps per five intervals, in the order the parameters are listed. The ladder is the operator's set of settings. The loop does not search for a measured optimum.

Both updates are folded into the one file write at the end of the application step.

## Loop

`init_resources` detaches `policy_loop` beside `sample_loop`. The thread lives with the daemon table in `src/daemons.cpp` and calls the pure decisions in `src/policy.cpp`: `node_hot`, `least_allocated`, `degrade_set`, and `tune_step`.

Each wake, one second apart:

1. Read group rings through `resource_rings` and append one interval load per group. The thread keeps the last five.
2. Copy running daemon ids, pids, grants, ceilings, and thread-ring pointers under `daemons_mu`, then drop the lock before any ring read or cpuset write.
3. Run `shift` for hot nodes.
4. Run `widen` for daemons whose grant is exhausted, then drop grants that have been empty of this daemon for five intervals.
5. Run `tune_step` and `degrade_set`, and write the policy file.

`shift` and `widen` that change a cpuset set `added_at_ns` under `daemons_mu` after `cpuset_applied` succeeds. `monitor_daemon` keeps running `saturate` on its 100 ms window. The accept thread may still write the same cpuset through `cpus_route` and `migrate_route`. `policy_loop` does not take `daemons_mu` across the sysfs write.

A daemon is left alone for five intervals after attach, after `shift`, and after `widen`, counting from that moment. Application steps wait the same five intervals after attach. The first file write still happens at attach.

## Attach

`POST /policies` is handled by `policies_route` and registered in `route` after `scale_route`. The body is JSON.

| Field | Meaning |
| --- | --- |
| `daemon_id` | A running daemon |
| `exec_path` | Optional. When set, it must equal the daemon's `exec_path` |
| `shift` | When true, this daemon may be moved off a hot NUMA node |
| `restrict.level` | Grant level. Default `core` when `widen` is set and `restrict` is omitted |
| `restrict.ceiling` | Optional group name. The grant and any move stay inside it |
| `widen.max_groups` | How many groups the grant may hold. Default is every group at that level inside the ceiling |
| `degrade.components` | `{name, cost}` optional parts |
| `tune.parameters` | `{name, values}` ladders |

An unknown field, an unknown policy shape, a bad name, a cost outside `(0, 1]`, a ladder of fewer than two values, an unknown ceiling, or a cpuset that sticks out of the ceiling is HTTP 400 and the stored document is left as it was. A daemon that is not running is HTTP 400. Missing resource groups are HTTP 500, the same gate as `scale_route`.

Posting again replaces the document, resets `shifted_at_ns`, the disabled set, and the tune indexes, and writes a fresh policy file. It does not clear the saturate window. An empty document `{}` with only `daemon_id` removes the grant and the shift flag, leaves saturate attached, and deletes the policy file.

The response is `{ok, path}` with `path` set when a file was written.

`/scale` is unchanged. `saturate` remains the policy that adds and removes a single CPU. `shift`, the grant, `degrade`, and `tune` are this document, not names passed to `make_policy`.
