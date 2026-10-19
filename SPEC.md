# Mercury

Mercury places, starts, and deletes native programs on computers on one LAN. This system is for managing daemons. Each program is an executable compiled for that machine. Nothing runs in a container.

## Roles

The **controller** keeps the daemon table. The operator submits a set of `(exec_path, hostname)` tuples. That hostname is the node offering the file.

A **worker** runs on each computer. It listens on one TCP port, starts local processes, and reports when they exit.

Each node runs one process. Its share of cores is an array of Linux CPU numbers. Starting it with only a port uses every CPU in `/sys/devices/system/cpu/online`. An explicit list after the port is that share instead. It pins itself to the lowest number in the share for its whole life, so a port-only start pins it to CPU 0 when that CPU is online. The remaining numbers are the daemon pool. On one node this process also holds the daemon table; on the others it is the worker. It is not a daemon. Round-robin does not place it, and delete does not stop it.

A worker registers with `{hostname, cpus}` when it connects. A tuple is schedulable when at least one registered worker reports that `exec_path` exists and is executable. Any other tuple is rejected and not stored.

## Daemon


| Field       | Meaning                           |
| ----------- | --------------------------------- |
| `daemon_id` | Assigned by the controller        |
| `exec_path` | Absolute path on the worker       |
| `hostname`  | Worker selected to run it         |
| `cpu`       | Index chosen by round-robin       |
| `env`       | Variables added for that process  |
| `state`     | `pending`, `running`, or `exited` |
| `exit_code` | Set when the process exits        |
| `log`       | That process's stdout and stderr  |


`pending` is placed but not started. `running` means the worker holds a live pid. `exited` means `exit_code` and `log` are stored for that `daemon_id`.

Delete sends kill if the process is running, then removes the record. A deleted daemon keeps no exit record.

## Messages

The worker accepts HTTP/1.1 on its listen port. Request and response bodies are UTF-8 JSON.

Worker to controller:

- `register` — `{hostname, cpus}`
- `exited` — `{daemon_id, exit_code, log}`

Thread membership stays on the worker. A new thread is not reported as its own daemon, and it is not pinned to the next free CPU.

Controller to worker, as HTTP JSON on the worker's listen port:

- `POST /stat` — `{exec_path}` returns `{ok}`
- `POST /start` — `{daemon_id, exec_path, cpu, listen_port?, env?}` returns `{pid}`
- `POST /migrate` — `{daemon_id, cpu}` returns `{pid}`
- `POST /cpus` — `{daemon_id, cpus}` returns `{ok}`
- `POST /scale` — `{daemon_id, min?, max?, policy?}` returns `{ok, min, max, cores, threads}`
- `POST /kill` — `{daemon_id}` returns `{ok, finished}`

The worker also answers two reads for a page that draws where daemons sit:

- `GET /map` — resource-group tree, and the running daemons whose cpuset intersects each group
- `GET /rings` — recent samples from each resource-group ring. `?count=N` selects how many of the newest samples to return

CPU numbers in `start` and `migrate` are Linux CPU numbers, the same numbers `cpus` writes into a cpuset. The worker's own CPU is the lowest number in its share and is never part of a daemon cpuset.

On `start`, the worker places the daemon in a cgroup and sets that cgroup's cpuset to that Linux CPU. It pins the child, then the child widens its affinity to every online CPU except the worker. The cpuset is the limit. Later threads are not pinned one by one; they run on whatever the cpuset allows. The worker raises its own open-file soft limit to the hard limit before it listens, and each daemon does the same before `exec`. `exec_path` is run with no arguments unless `listen_port` is set. Then the worker binds `127.0.0.1` on that port, passes the socket as fd 3, and the arguments are `--listen-fd 3`. Stdout and stderr are read until the child exits, then `exited` is sent once. The exit code and log stay those of the process that was `exec`'d.

`env`, when present, is an object of strings. Each name matches `[A-Za-z_][A-Za-z0-9_]*`. Those names are added to the environment the worker itself inherited, replacing an inherited value with the same name. Every other inherited variable stays. A missing `env` changes nothing. A bad name or a value that is not a string is rejected and the process is not started.

`migrate` rewrites that daemon's `cpuset.cpus` to the requested Linux CPU and leaves the process running. The returned pid is the pid already running. Threads stay in the cgroup; the new cpuset is the limit, so each thread runs on that CPU once it is scheduled. A cpuset that had grown past one CPU is replaced by this single CPU.

`cpus` writes the daemon cpuset. `cpus` is a Linux cpuset string, such as `1-4`. The set must not contain the worker's Linux CPU.

## Monitoring

The worker reads the daemon cgroup once per tick. A sample is taken every 10 ms and the ring keeps 60 s. Scaling does not open a perf counter per thread and does not keep a ring per thread, so a daemon with thousands of threads costs the same two file reads as a daemon with one.

The cgroup `cpu.stat` total for every task in the daemon is written to `rings/<daemon_id>.mring`. Its `task-clock`, `user`, and `system` values are the nanoseconds the whole daemon was scheduled in that gap. Its fourth value, `pressure`, is the gap in the `some total` line of the cgroup `cpu.pressure` file: the nanoseconds in that gap during which at least one runnable task of the daemon waited for a CPU. This ring is the input to `saturate`.

Occupancy of one sample is `task-clock` divided by the gap and by the number of CPUs in the cpuset. Pressure of one sample is `pressure` divided by the gap. One busy thread on one CPU has occupancy 1 and pressure 0. Six busy threads on one CPU have occupancy 1 and pressure 1. Six busy threads on four CPUs have occupancy 1 and pressure 0.5, because some task is waiting about half the time.

When `cpu.pressure` cannot be read (PSI disabled in the kernel), the worker counts the threads of the daemon and its descendants whose state in `/proc/<tid>/stat` is `R` once per tick and stores that count instead of `pressure`, with the ring name `runnable`. This fallback opens no perf fd.

## Scaling

`saturate` is attached to every daemon when it starts, with `min` 1 and `max` 16. `scale` updates a running daemon: `min` defaults to 1 and `max` defaults to 16. `policy` defaults to `saturate`. An unknown policy is rejected. The policy name `none` detaches `saturate` from that daemon, for callers that hold a cpuset fixed; posting `saturate` again attaches it. The policy name `full` is the reference baseline for `gnn`: it detaches `saturate` (and `gnn`) like `none`, then `set_full_pool` writes the daemon's cpuset to the whole daemon pool (every CPU after the worker's own, through `write_cpus_and_mems`, polling `cpuset_applied`) and sets `changed_at_ns`. `min` and `max` do not apply to it; the response's `cores` is the pool size. Nothing resizes or pins the daemon afterwards, so the kernel places its threads anywhere in the pool. A failed write is HTTP 500. Several daemons on `full` share the same CPUs. Posting again updates the limits and the policy and does not clear the current window.

`saturate` decides on the mean of the last `kScaleWindow` (64) samples, which is 640 ms. Every sample in the window is newer than the last change of the cpuset by `saturate`.

- If the cpuset has fewer CPUs than `min`, add one immediately. If it has more than `max`, remove one immediately.
- Add one CPU when the mean occupancy is at least 0.85, the mean pressure is at least 0.10, and the cpuset is still under `max`.
- Remove one CPU when the cpuset is over `min`, the mean pressure is at most 0.02, and the mean occupancy is at or below `(cores - 1) / cores - 0.05` (margin rule) or at or below `(cores - 1) / cores` (relaxed rule). With pressure that low the relaxed rule covers the margin rule; both are kept as named tests. The relaxed rule lets N busy threads that picked up an (N+1)th CPU, at occupancy `N / (N + 1)`, give it back.
- Otherwise hold.

After any add or remove, the next decision waits for a fresh full window of 64 samples. One CPU changes per window, in either direction. Between the add line and the remove line no change is made. A daemon that just fits its cpuset (occupancy near 1) is above `(cores - 1) / cores` and is not shrunk; one that is short a CPU has pressure and is not shrunk either.

With the `runnable` fallback, the pressure test is replaced by the mean runnable count: add when it is at least `cores + 0.5`, remove when it is at most `cores - 1` and the occupancy passes the margin or the relaxed rule.

`scale` also takes an optional `placement`, the rule for the CPU `add_core` adds: `least_loaded` (the default, described next) or `nearest_first`. Omitting it keeps the daemon's current placement; an unknown name is rejected with 400. The response carries `placement`. A daemon starts with `least_loaded`. Placement is per daemon only: there is no worker-wide CLI or environment default, so one Mercury can run both and an experiment names the placement in the request.

`nearest_first` (`request_cpu_nearest`, `src/resources.cpp`) takes the allocatable CPU outside the cpuset and the exclude list that is closest to the current cpuset: a CPU on the same physical core as a held CPU (SMT sibling), else in the same L3, else the same NUMA node, else the same socket, else anywhere. Within a tier the lower `load_of` of the candidate's core group wins, then the lower Linux CPU, the same tie break as `least_loaded`. An empty cpuset falls back to `request_cpu`. On dtn-01, a daemon on CPU 1 grows 1 → 33 → 32 (L3 0-1,32-33 with CPU 0 reserved), then within node 0, then node 1-3 on socket 0, and only then onto socket 1.

`least_loaded`: the CPU added comes from `request_cpu` (`src/resources.cpp`). `request_cpu` calls `balanced_cpu`, which walks the group tree from the socket down through the node and the L3 to the core. At each level it keeps every group that still has an allocatable Linux CPU outside `exclude`, and only the children of the group chosen on the level above. The group with fewer of those excluded CPUs already in it wins, so sibling groups receive the same number of this daemon's CPUs before any sibling receives another. An equal count keeps the lower `load_of`, and an equal load keeps the group whose lowest Linux CPU is smaller. The CPU returned is the lowest allocatable Linux CPU on the chosen core that is not in `exclude`. A CPU another daemon already runs on is still offered. When every allocatable CPU is already in this cpuset, nothing is added. The CPU removed is the highest one in the cpuset.

## Round-robin

Workers are numbered from 1 in registration order. A cluster cursor `next_node` starts at 1. For each worker, `daemon_cpus` is `cpus` without its lowest number, sorted ascending. A cursor walks `daemon_cpus` from the first entry. A share of one CPU accepts no daemons.

Walk schedulable tuples in submission order. From `next_node`, take the next worker for which `stat` returns ok, store it as `hostname`, and set `next_node` to the worker after it, wrapping to 1. Then set `cpu` to the next entry of that worker's `daemon_cpus` and advance, wrapping to the first entry. Store the daemon as `pending`.

Activate starts every `pending` daemon. Several daemons may run on the same cpu.

## Delivery

A daemon stays on the controller until activate. Delivery is a single `start` to the worker in `hostname`. Load balancing across nodes is round-robin, by `next_node`. The controller does not copy the binary. A worker is skipped when `stat` fails, so if only the offering node has the path, every daemon is delivered there.

## Actions

- **Submit** — place the given tuples as above.
- **Activate** — start every placed daemon.
- **Delete** — kill if running, then drop the daemon.

If a worker disconnects, each of its `running` daemons is stored as `exited` with `exit_code` -1 and the log `worker lost`. Those daemons are not restarted.

## Subsystems

The `mercury` binary is the worker. CMake builds it from `src/mercury.cpp`, `src/daemons.cpp`, `src/counters.cpp`, `src/ring.cpp`, `src/ring_archive.cpp`, `src/policy.cpp`, `src/resources.cpp`, and `src/http.cpp`. Submit, activate, round-robin, and the controller daemon table are the contract above. These files are the worker that receives `start`, `migrate`, `cpus`, `scale`, and `kill`.

Four threads share one worker. `main` accepts HTTP and runs every route to completion before it accepts again. `arm_daemon` detaches `watch_daemon` and `monitor_daemon` for each live pid. `init_resources` detaches `sample_loop`, which samples host load into the resource-group rings. A critical section is a stretch those threads must not interleave on the same state.

### Process start

File: `src/mercury.cpp`.

`main` first strips `--numa-balancing=keep` from the arguments, then runs `setup_numa_balancing` (below), then reads `<port> [cpu...]`. With no CPU arguments, `online_cpus` reads `/sys/devices/system/cpu/online` and that list is the share; CPU 0 must be in it. `parse_cpu` accepts a Linux CPU from 0 through `CPU_SETSIZE - 1` and rejects duplicates. The lowest number is the worker CPU. `pin_to` calls `sched_setaffinity` on that CPU and the process stays there. The other numbers are sorted into `daemon_cpus`. `raise_nofile` (`src/daemons.cpp`) lifts the soft open-file limit to the hard limit, or to 65536 when the hard limit is infinite. `set_mercury_cpu` stores that Linux CPU. `set_daemon_cpus` stores the sorted daemon pool. `serve` then blocks in the accept loop.

#### numa_balancing

Mercury runs with `kernel.numa_balancing` set to 0 by default, so automatic NUMA balancing does not keep a daemon's threads on a preferred node after its cpuset moves or grows onto another node. It changes the live sysctl only (`/proc/sys/kernel/numa_balancing`) and never writes `/etc/sysctl.d`.

Startup, in `setup_numa_balancing`, before the port is parsed:
1. Read the current value. If the file is missing, log `numa_balancing unavailable` and change nothing.
2. If the state file `/run/mercury/numa_balancing.orig` exists, it holds `<original value> <owner pid>`. If that pid is alive and is not this process, another mercury owns the setting: log it and change nothing (this instance will not restore it either). Otherwise the state is stale (a crash or `SIGKILL`): write the saved original back to the sysctl, log `stale state ... restoring`, delete the file, and treat that value as the original.
3. Opt-out: with `MERCURY_NUMA_BALANCING=keep` in the environment or `--numa-balancing=keep` on the command line, log `numa_balancing kept at <v>` and change nothing further.
4. Otherwise write `<original> <pid>` to the state file (`/run` is tmpfs, so the file goes away on reboot together with the sysctl value), register the `atexit` restore and `SIGINT`/`SIGTERM` handlers, write 0, and log `numa_balancing <before> -> <after>`.

Shutdown: a normal return from `main` (including an exception, such as a port that cannot be bound) runs `restore_numa_at_exit`, which writes the original back, deletes the state file, and logs `numa_balancing restored to <v>`. `SIGINT` and `SIGTERM` run `on_terminate`, which does the same with only `open`, `write`, `close`, and `unlink` (async-signal-safe, no log line) and then `_exit(128 + signal)`. Before that, `on_terminate` calls `kill_daemon_cgroups_raw` (`src/daemons.cpp`): for every daemon cgroup this instance created (`launch` records each placement dir in a fixed, lock-free table), it writes `1` to `cgroup.kill`, which kills every task in it including descendants of the daemon, then retries `rmdir` every 10 ms for up to 2 s until each cgroup is gone. Cgroups from other Mercury instances are not touched. A normal return from `main` does not kill daemons. Only the owning instance restores. After `SIGKILL` or a crash the next mercury start restores the value in step 2.

`daemon_cpus` and `mercury_linux_cpu` are written on this thread before any other thread starts. Later readers do not lock them. `main` passes `daemon_cpus` to `init_resources` (`src/resources.cpp`) before `serve`.

`route` is the only dispatch. It tries `ping_route`, `stat_route`, `start_route`, `migrate_route`, `cpus_route`, `scale_route`, `map_route`, `rings_route`, then `kill_route`. The first function that recognizes the target returns the response. Anything else is HTTP 404 from `http_response`.

### HTTP accept loop

Files: `src/http.cpp`, `src/http.hpp`.

`serve` calls `listen_on`, which binds `127.0.0.1` with backlog 16, then `serve_fd`. `serve_fd` accepts one connection and calls `handle_client` on the same thread. `handle_client` calls `read_request`, then the `route` function, then `send_all`, then closes the socket. `read_request` stops at 64 KiB. `request_target` and `http_target` parse the request line.

This accept thread is a control-flow section. `launch`, `migrate_route`, and `kill_route` run inside it, so a placement wait, a cpuset wait, or a kill wait delays the next request. No second route runs until `handle_client` returns.

`Arrivals` and `Arrivals::mu` count requests inside `handle_client`. `/arrivals` reads that count. Daemon routes do not take this lock. `ping_route` answers `ping` and `GET /ping`. `relay_route` and `ping_reply` belong to the hello-server relay and are not on the `mercury` route list.

### Daemon table

File: `src/daemons.cpp`. Header: `src/daemons.hpp`.

The table is the vector `daemons` of `Daemon`. `find_daemon` walks it by `daemon_id`. The mutex is `daemons_mu`. The condition variable is `daemons_cv`. Every field of `Daemon` is read and written under `daemons_mu`: identity, `exec_path`, `pid`, `running`, `exit_code`, `log`, `finished`, `threads`, `scale`, `cpu`, `listen_fd`, `counters_done`, and `env`.

`ScaleState` holds `enabled`, `min_cores`, `max_cores`, `policy`, and `changed_at_ns`. `ThreadRing` holds a tid and a `shared_ptr<Ring>`.

Holders of `daemons_mu`:

| Function | What it does under the lock |
| --- | --- |
| `daemon_running` | True when this `daemon_id` still has this pid and `running` |
| `stat_route` | `{ok:true}` when some running daemon was started with this `exec_path` |
| `start_route` | Rejects the start when this `daemon_id` is already running |
| `migrate_route` | Reads `pid` and `cpu` of a running daemon whose cpu differs, then stores the new cpu after `cpuset.cpus` is written |
| `arm_daemon` | Inserts the record, or replaces it and closes a different previous `listen_fd` |
| `watch_daemon` | Sets `running` false, `exit_code`, `log`, and `finished`, then `notify_all` |
| `mark_counters_done` | Sets `counters_done` when the pid still matches, then `notify_all` |
| `remember_thread`, `forget_thread` | Push or erase a `ThreadRing` when the root pid still matches |
| `monitor_daemon` | Copies `ScaleState` and the ring pointers; after any add or remove, writes `changed_at_ns` |
| `scale_route` | Attaches, replaces, or (`none`) detaches the policy and updates the limits; never writes `changed_at_ns` |
| `kill_route` | Holds a `unique_lock` across the waits below |

`kill_route` is the long critical section. It takes `daemons_mu`, sends `SIGTERM`, and `wait_for`s up to 2 seconds until `watch_daemon` sets `finished`. On timeout it sends `SIGKILL` and `wait`s until `finished` is set. It then `wait_for`s up to 2 seconds for `counters_done`. It closes `listen_fd` before releasing the lock. `watch_daemon` and `mark_counters_done` take the same mutex only to publish and notify, so this wait can complete.

`arm_daemon` runs on the accept thread after `launch` returns. It publishes the record, then detaches the two threads. `watch_daemon` and `monitor_daemon` ignore the record when the pid no longer matches.

Work outside the lock: fork, cgroup writes, perf reads, `Ring::push`, `Ring::recent`, and `Policy::decide`. The monitor copies `shared_ptr<Ring>` under `daemons_mu`, drops the lock, then calls `recent`. It never takes `daemons_mu` while holding `Ring::mu_`.

### Start and migrate

File: `src/daemons.cpp`.

`start_route` parses `daemon_id`, `exec_path`, `cpu`, optional `listen_port`, and optional `env`. `parse_env` and `valid_env_name` accept an object of strings whose names match `[A-Za-z_][A-Za-z0-9_]*`. A bad name or a non-string value fails the request and skips `launch`. The cpu must be in `daemon_cpus`. `access(exec_path, X_OK)` must succeed. `bind_loopback` binds `127.0.0.1` when `listen_port` is set.

`launch` is the placement critical section. `prepare_placement` calls `ensure_daemon_cpuset`, which creates `/sys/fs/cgroup/mercury/<daemon_id>` and enables the cpuset controller through `ensure_cpuset_controller` and `controller_enabled`. It writes `cpuset.cpus` to that Linux CPU when that file is empty, then polls `cpuset_applied` for up to 50 times at 10 ms. `cpuset_applied` requires `cpuset.cpus` and `cpuset.cpus.effective` to both equal the requested list. An existing non-empty cpuset is left as it is, and the child is pinned to its first CPU.

The parent then pipes stdout, stderr, and, when placement succeeded, a sync pipe, and `fork`s.

Child, in order: read one byte from the sync pipe; `dup2` the log pipes onto stdout and stderr; `dup2` the listen socket onto fd 3 when present and `close_range` every fd above it; `widen_to_online` when placement exists, otherwise `sched_setaffinity` to that Linux CPU; `raise_nofile`; `exec_daemon`. `widen_to_online` sets the affinity to every CPU in `/sys/devices/system/cpu/online` except `mercury_linux_cpu`. `exec_daemon` calls `execv` when `env` is empty and `execve` otherwise, merging those names over the inherited environment. Arguments are empty, or `--listen-fd 3` when a listen socket was passed. Failure exits 127.

Parent, after the child is waiting on the sync pipe: `pin_pid` on the child, then `move_into_cgroup` via `cgroup.procs`, then write the byte `'1'`. The cgroup is in place before the child widens its mask. A failed pin, move, or write sends `SIGKILL`, `waitpid`, and `launch` returns empty.

`migrate_route` reads the live pid and cpu under `daemons_mu`. The cpu must be in `daemon_cpus` and must differ from the recorded cpu. It does not fork. It writes `cpuset.cpus` through `ensure_daemon_cpuset`, `write_sysfs`, and `cpuset_applied`, polling for up to 50 times at 10 ms. On success it stores the new cpu on the same record and returns that pid.

Every write of a daemon's `cpuset.cpus` goes through `write_cpus_and_mems` (start in `prepare_placement`, `migrate_route`, `cpus_route`, `add_core`, and `remove_core`). It sets `cpuset.mems` to the NUMA nodes that contain at least one CPU of the new `cpuset.cpus` (`nodes_for_cpus`, from `/sys/devices/system/node/node<N>/cpulist`, read once). Order: first `cpuset.mems` is widened to the union of the old and the new nodes, then `cpuset.cpus` is written, then `cpuset.mems` is shrunk to the new nodes. Growing therefore widens mems before cpus, shrinking narrows mems after cpus, and no intermediate state has a CPU without its node. A failed `cpuset.mems` write is logged and does not fail the cpuset write; a failed `cpuset.cpus` write does. In cgroup v2 the kernel migrates the daemon's pages when `cpuset.mems` changes.

`cpus_route` parses a Linux cpuset with `parse_cpuset`, rejects a set that contains `mercury_linux_cpu`, and rejects a set `resource_groups_see` does not know. It writes the set through `ensure_daemon_cpuset`, `write_sysfs`, and `cpuset_applied`. On success it logs the least-loaded socket, node, L3, and core that contain one of those CPUs, from `least_loaded_touching`, formatted by `resource_choice`. `add_core` and `remove_core` write that same `cpuset.cpus` from the monitor thread. Those three functions do not take `daemons_mu`. The accept thread and the monitor thread can both write one daemon's cpuset.

### Watch

File: `src/daemons.cpp`.

`watch_daemon` calls `read_pipes`, which `poll`s stdout and stderr until both ends close, then `waitpid`. It publishes `exit_code` (`WEXITSTATUS`, or -1 when the process did not exit normally), the log, and `finished` from `now_ms`, under `daemons_mu`, and notifies `daemons_cv`. The publish is skipped when `arm_daemon` has already replaced this pid.

### Monitoring

Files: `src/daemons.cpp`, `src/counters.cpp`, `src/counters.hpp`.

`monitor_daemon` opens the cgroup ring first: `RingArchive` at `rings/<daemon_id>.mring` and a `Ring` named `task-clock`, `user`, `system`. `rings_dir` uses `MERCURY_RINGS` or `rings`. `archive_name` keeps letters, digits, `.`, `_`, and `-`.

The loop condition is `daemon_running`. Each pass sleeps `kSampleMs` (10), then `read_cgroup_cpu` reads `cpu.stat` and `read_cgroup_pressure` reads the `some total` value of `cpu.pressure`. `usec_delta_ns` converts the `usage_usec`, `user_usec`, `system_usec`, and pressure `total` gaps into one sample of the cgroup ring. A tick where either read fails pushes nothing. When `cpu.pressure` is missing at the first read, the monitor uses `count_runnable`, which walks `descendants` (`child_pids` reads `/proc/<pid>/task/<pid>/children`) and `/proc/<pid>/task/*/stat`, for the whole life of that daemon. The monitor opens no perf counters.

On the way out, the loop flushes the cgroup ring and calls `mark_counters_done`.

### Scaling

Files: `src/policy.cpp`, `src/policy.hpp`, and the decision loop in `monitor_daemon`.

Thresholds are named constants in `src/policy.hpp`: `kScaleWindow` (64), `kAddOccupancy` (0.85), `kAddPressure` (0.10), `kRemoveMargin` (0.05, margin rule only), `kRemovePressure` (0.02, both remove rules), `kRunnableAddMargin` (0.5). `load_of` in `src/resources.cpp` keeps its own window, `kLoadWindow` (10 samples, 100 ms), independent of `kScaleWindow`. The cgroup ring counters are `task-clock`, `user`, `system`, and `pressure` (the `some total` delta of `cpu.pressure`, in ns) or, without PSI, `runnable` (the count of runnable threads in the cgroup); `kCgroupTaskClock` (0) and `kCgroupPressure` (3) index them.

`arm_daemon` sets `ScaleState` to `enabled`, `min_cores` 1, `max_cores` `kDefaultMaxCores` (16), and a `SaturatePolicy`, and sets `changed_at_ns` to the start time. `scale_route` runs on the accept thread. `scale_int` reads `min` (default 1) and `max` (default 16). `make_policy` returns a `SaturatePolicy` for `saturate` and empty for any other name. `scale_route` rejects an empty policy with 400 unless the name is `none`. Under `daemons_mu` it requires `running`, stores the limits and the policy, and sets `enabled` to whether a policy is attached. It does not touch `changed_at_ns`.

Each monitor pass, after the sample, copies `ScaleState` under `daemons_mu`. It builds an `OccupancyWindow` with `cores` from `cpuset_cores`, the limits, the pressure source, and the cgroup samples from `Ring::recent(kScaleWindow + 1)` whose `timestamp_ns` is greater than `changed_at_ns`. `SaturatePolicy::decide` is a pure function of that window. It returns `AddCore` when `cores < min_cores` and `RemoveCore` when `cores > max_cores`. It returns `Hold` until the window holds 65 samples, so each of the 64 has a gap. A gap of zero or less returns `Hold`. Mean occupancy is the sum of `task-clock` over the sum of the gaps, divided by `cores`. Mean pressure is the sum of `pressure` over the sum of the gaps. The add and remove tests are the ones under Scaling above.

Back in `monitor_daemon`, `Hold` does nothing. `AddCore` calls `add_core` and `RemoveCore` calls `remove_core`. Both poll `cpuset_applied`. After either succeeds, the monitor sets `changed_at_ns` under `daemons_mu`. Every saturate change therefore opens a fresh window.

`decide` is tested without a host: `policy_test` feeds it synthetic windows (including 6 busy threads on 7 CPUs, which must remove) and the recorded traces in `src/test/traces/` (CSV, `ts,task_clock,user,system,pressure`, header `# cores=N ... expect=X`; `TRACES_DIR` in CMake). Every sliding 65-sample window of a trace must give its expected decision. `src/test/record_traces.py` (root) re-records them with `spinner` in the cgroup `mercury/ptrec`. Acceptance on dtn-01: `spinner` with N busy threads started on one CPU settles at N CPUs and stays there for ten windows; `src/test/saturate_it.sh` checks N = 1, 3, 6 on the CPUs of NUMA nodes 0-2 (`saturate_integration` under `ctest`).

Known flaky (2026-10-19): `saturate_integration` passed 1 of 3 runs on dtn-01 against the working tree that became `gnn-dsb-slo` (ctest, then two reruns on port 18180, nothing else running). It fails in two ways. In one, N=3 reaches 3 CPUs after 1.5 s but moves to 3..4 CPUs during the ten-window hold, so one CPU too many is added. In the other, N=6 does not reach 6 CPUs and stays at 5. `policy_test` and `policies_integration` passed. The cause is not settled; timing near the add/remove thresholds (occupancy >= 0.85, pressure >= 0.10) and window freshness after a change are the first suspects. Main 8e75094 cannot serve as a baseline: there `arm_daemon` does not attach `saturate` to the daemons it starts (the auto-attach, min 1 max 16, is new in this change), so a Release build of main fails 3 of 3 runs with N=3 and N=6 stuck at 1 CPU.

Test expectations that follow from saturate being attached at start:
- Tests that pin a daemon (`main.cpp` after `hello` starts and after each "start failed for" probe) post `/scale` with `max` 1 so the cpuset is not grown.
- `scale_test` and the fixed-size steps of `autoscale_test` post `/scale` with policy `none` before writing `/cpus`, and `autoscale_test` re-attaches `saturate` afterwards and checks the default range 1-16.
- `autoscale_test` allows 60 s to grow from 1 to 16 cores (one CPU per window plus the cooldown), and in its ramp and down phases expects N `/cpu` clients, each burning one core, to settle at N cores (clamped to 1-16).
- `autoscale_nearest_first` mirrors `autoscale_least_loaded` (own ports 18133/18134 and daemon id `nearest`) with `placement` `nearest_first` and `partial_server` started with `PARTIAL_BURN_PCT=100`, so each served connection burns a whole core and N clients must settle at N cores (clamped to 1-16). Every CPU it sees added must be in the nearest tier any free allocatable CPU offered. `partial_server` reads `PARTIAL_BURN_PCT` (1-100, default 33).
- `resources_test` checks `nearest_first` on the live topology (each start CPU and a 20-step grow pick a CPU of the best tier) and, on dtn-01, the concrete picks: 1 → 33, {1,33} → 32, a full L3 stays on node 0, an excluded sibling falls to the L3, socket 0 excluded goes to socket 1.
- `saturate_it.sh` takes `SAT_PLACEMENT` and `autoscale_test` takes `AUTOSCALE_PLACEMENT`; when set, the test posts that placement to `/scale` (test-side only).
- `autoscale_least_loaded` still encodes the earlier 90% rule (`cores_for_clients`: 0.33 core per client) and does not match this rule.

The reactive policies `shift`, `restrict`/`widen` (the grant) and `degrade`, with `POST /policies`, `policy_loop` and `policies.md`, were removed on 2026-10-19. They were cluster baselines ported onto one host; gnn is to be compared against the best fixed allocation on the system instead (the daemon alone, with as many CPUs as possible).


## gnn policy (per-thread GNN placement controller)

**PARTLY IMPLEMENTED.** Phase 1 (data collection) and phase 1b (the closed-loop `gnn` policy) are built; see those subsections. What is still proposed is marked there and in Open questions. The Graph, Outputs, Training and Evaluation subsections are the original design; where the build differs, phase 1b says so.

### Purpose

`gnn` is a second core policy beside `saturate`. A daemon gets it per daemon, the same way it gets `saturate` or `none` (through `/scale` `policy`), so the two can be A/B tested on the same host. `saturate` only sets the size of the cgroup cpuset and lets CFS/EEVDF place threads inside it, and the kernel scheduler can then stack busy threads in ways nobody intended. `gnn` makes the placement decision itself: it picks a target CPU for each thread and derives the cpuset from those choices.

### Tick and signals

The decision tick is derived, not fixed: tick = 10 × the measured inference latency of the deployed model (configurable factor and floor). It is never shorter than the per-thread read cost allows: the read (below) must take well under the tick. This replaces the earlier ~1 s tick. Measured on 2026-10-17: the phase-1 model (2 delayed graph-filter layers, K = 3, 16 hidden, 1,633 parameters) runs a batch-1 single-tick forward in about 50 µs (median; p99 about 65 µs) in torch, 1 thread, on the user's PC, for 3-17 threads. That puts 10 × inference at about 0.5 ms. The per-thread read cost on dtn-01 is 0.4 ms (2 threads) to 2.5 ms (64 threads), and `cpu.stat`/PSI are only meaningful over a few ms. So the effective tick is the 10 ms sample period (`kSampleMs`) for daemons up to about 64 threads, and longer only for larger daemons (about 6 ms of read at 256 threads). Signals are read directly at decision time and do not go through the 10 ms rings. For each signal only the previous value is kept, to compute a delta.

- Thread discovery: `/sys/fs/cgroup/mercury/<daemon_id>/cgroup.threads`.
- Per-thread perf group: `task-clock`, `context-switches`, `cpu-migrations`, read in one `read()` with `PERF_FORMAT_GROUP`. Opened with the `open_event`/`open_group`/`read_group` code in `src/counters.cpp`.
- Per-thread wait: `/proc/<pid>/task/<tid>/schedstat` (run ns, run-queue wait ns, timeslices).
- Cgroup-level: the deltas of `cpu.stat` `usage_usec` and `cpu.pressure` `some total`. These are the same occupancy and pressure that `window_means` computes, over the tick.

### Phase 1 (implemented): per-thread data collection

Phase 1 is built. It records the controller's inputs and ground-truth labels. On its own it decides nothing; phase 1b uses the same reader in-process.

- `src/threadstat.{hpp,cpp}`, `ThreadStats(cgroup_dir, max_perf_threads = 1024)`. Each `tick()`:
  - reads `<cgroup>/cgroup.threads`. A new tid gets `open_software` (a software perf group: `task-clock`, `context-switches`, `cpu-migrations`, one `read()` with `PERF_FORMAT_GROUP`, through `open_group`/`read_group` in `src/counters.cpp`) and is reported `fresh` with zero deltas: that tick is its baseline. A tid that left `cgroup.threads` is dropped and its fds close (`CounterGroup` owns them).
  - reads `/proc/<tid>/schedstat` (run ns, run-queue wait ns, timeslices) and field 39 of `/proc/<tid>/stat` (last CPU), mapped to a NUMA node from `/sys/devices/system/node/node*/cpulist`.
  - reads the cgroup `cpu.stat` (`usage_usec`, `user_usec`, `system_usec`), `cpu.pressure` (`some` and `full` totals) and `cpuset.cpus.effective`.
  - keeps the cgroup files and each thread's `/proc/<tid>/stat` and `schedstat` open and re-reads them with `pread` at offset 0 (5 fds per thread: 3 perf + 2 proc). The first version opened an `ifstream` per read and cost about 100 µs per thread.
  - reports its own cost as `read_ns` per tick. On dtn-01 at a 10 ms tick, median (p99): 2 threads 0.41 ms (0.46), 8: 0.60 (0.62), 16: 0.86 (0.88), 32: 1.40 (1.42), 64: 2.47 (2.51), 128: 5.4 (5.5), 256: 6.3 (6.4) ms. A new thread's first tick also opens its perf group and proc files, so the first tick of a fresh daemon takes 25-80 ms. The 10 ms interval holds (median 10.00 ms) up to 128 threads; at 256 threads with the earlier reader it slipped to 13.9 ms.
  - keeps only the previous value of each counter and returns deltas. Perf groups are capped at `max_perf_threads` (3 fds each); past the cap, or when `perf_event_open` fails (for example `EMFILE`), a thread is read from schedstat only, with `perf` false. `raise_nofile_limit` lifts the soft fd limit to the hard limit.
- `open_software(pid)` is new in `src/counters.{hpp,cpp}`. `open_counters` and `open_clock` are unchanged.
- `threadtrace` (`src/threadtrace.cpp`, `build/threadtrace`) is a standalone recorder that attaches to a running daemon's cgroup. It needs root (Mercury daemons run as root). Usage: `threadtrace <daemon_id | cgroup dir> [--interval-ms 1000] [--duration-s 0] [--out FILE] [--max-perf 1024] [--format jsonl|csv]`. A bare `daemon_id` means `/sys/fs/cgroup/mercury/<daemon_id>`. Ticks are on absolute `CLOCK_MONOTONIC` deadlines. It stops at the duration, when the cgroup disappears, or on SIGINT/SIGTERM.
  - JSONL: one object per tick: `ts_ns`, `tick`, `interval_ns`, `cgroup{cpu_ok, usage_ns, user_ns, system_ns, psi, some_ns, full_ns, cpuset_cpus, cpuset}`, `exited`, `perf_open`, `read_ns`, `threads[{tid, comm, fresh, perf, task_clock_ns, context_switches, cpu_migrations, run_ns, wait_ns, timeslices, cpu, node}]`.
  - CSV: one row per thread per tick, the same fields, with the cgroup values repeated.
- `gnnload` (`src/test/workloads/gnnload.cpp`, `build/test/gnnload`) is a synthetic workload with a known per-thread demand. It is configured from the environment (Mercury `/start` `env`) or `--key=value`:
  - `GNN_THREADS` (4), `GNN_PATTERN`, `GNN_TOTAL` (total cores, 2.0; each worker capped at 1.0), `GNN_PHASE_MS` (1000), `GNN_RAMP_S` (20), `GNN_RATIO` (0.5), `GNN_SEED` (1), `GNN_RESAMPLE` (1), `GNN_EXP_RATE` (0), `GNN_DURATION_S` (0, run until killed), `GNN_LABELS` (labels CSV path).
  - Patterns: `uniform` (TOTAL/N each); `hot` (worker 0 at min(1, TOTAL), the rest share the remainder plus 0.05); `geometric` (weights RATIO^i normalised to TOTAL); `bursty` (each phase, each worker is on at 2·TOTAL/N or idle at 0.05, decided by a seeded hash); `ramp` (TOTAL·0→1→0 over `GNN_RAMP_S`, split evenly, 5% steps).
  - **v4 patterns (added 2026-10-18):** `exp` and `expraw` draw each worker's demand from an exponential distribution instead of splitting TOTAL evenly or by a fixed rule. A new draw happens every `GNN_RESAMPLE` phases (default 1). The draws are i.i.d. per worker and per draw, made with a hash RNG from (`GNN_SEED`, draw index, worker), so the workers and the label writer compute the same values. `exp`: X_i ~ Exp(1), demand_i = TOTAL·X_i/ΣX, then water-filled to the 1-core cap (the excess goes to uncapped workers in proportion), so the sum stays TOTAL while TOTAL <= N. `expraw`: demand_i = min(1, X_i·TOTAL/N/m) with X_i ~ Exp(1), so the mean per worker is TOTAL/(N·m) and the per-phase sum varies. m is `GNN_EXP_RATE`, or, when it is 0 (the default), m is drawn per draw log-uniform in [0.5, 2] (a randomized rate). Under `exp` the rate cancels in the normalisation. Both patterns are quantised to 1e-4 cores, so the label (`%.4f`) is exactly the CPU-time rate each worker burns. The label header adds `resample=` and `exp_rate=`. The ground truth is computable exactly in the same way as for the other patterns: per-thread demand comes from the labels, and the required cores are the FFD of the demands.
  - Each worker asks for demand × 10 ms of its own CPU time (`CLOCK_THREAD_CPUTIME_ID`) per 10 ms slice. Slices are staggered per thread. Demand that cannot be met by the end of a slice is dropped, so there is no backlog. This is the `spinner`/`PARTIAL_BURN_PCT` duty-cycle model measured in CPU time. A wall-clock burn would make preempted threads idle in lockstep and hide the demand from `saturate`.
  - Labels CSV: a `# gnnload ...` header, then `phase_start_ns,phase,thread,tid,demand` per worker per phase (`CLOCK_MONOTONIC`, the same clock as `threadtrace`), and a final `phase=-1` row when `GNN_DURATION_S` ends the run. The main thread is idle and is not labelled.
  - `partial_server` is not reused: it is a request server whose load follows clients, not a fixed per-thread schedule.
- `src/test/gnn_dataset.sh` re-executes with `sudo -n`. It starts its own `mercury` on the CPUs of NUMA nodes 0-2 on a free port in 18180-18199 (as `saturate_it.sh` does), with `saturate` attached by default. Then, for every pattern × threads × total, it `/start`s `gnnload` on the second CPU of the share, runs `threadtrace` on it for `--duration`, `/kill`s it, and removes its cgroup.
  - Flags: `--out` (`data/gnn`), `--patterns` (all five), `--threads` ("4 8"), `--totals` ("2.5"), `--duration` (15 s), `--interval-ms` (1000), `--phase-ms` (1000), `--ramp-s` (= duration), `--placement`, `--no-validate`, `--trace-cpu` (the CPU `threadtrace` is pinned to with `taskset`; default the highest online CPU, outside the share). The default run is 10 cases, about 3 minutes.
  - Layout: `data/gnn/<YYYYmmdd-HHMMSS>/{meta.txt, mercury.log, validation.txt, validation.json}` and `<pattern>-t<T>-c<C>/{trace.jsonl, labels.csv, case.txt, start.json, threadtrace.log}`. The output is chowned to the invoking user.
  - `--seed-base N` (added 2026-10-18 for v4): gnnload `GNN_SEED` = N + case index (default 0, as before), so DAgger rounds and evaluation runs use draws that do not appear in training.
- `src/test/gnn_validate.py <run_dir>` compares the measurements with the labels. For each non-fresh worker and each tick inside the labelled span, expected = the time-weighted label demand over the tick, and measured = `task_clock_ns / interval_ns` (and schedstat `run_ns`). It reports MAE, bias and max error in cores, overall and on uncontended ticks (`wait_ns / interval < 0.02`). Under contention `task-clock` is below demand by design (the workload drops unmet demand); `run + wait` is the demand estimate the expert uses.
- CMake targets `threadtrace` and `gnnload` build with the rest (`cmake --build build`). None of these are `ctest` tests.

### Phase 1b (implemented): closed-loop gnn policy

The `gnn` policy is built and selectable per daemon through `/scale`.`/scale` picks the core policy (`saturate`, `gnn` or `none`).

- Enable: `POST /scale {"daemon_id":"<id>","policy":"gnn"}`, with optional `"min"`, `"max"` (defaults 1 and 16) and `"tick_ms"` (default 10, at least `kSampleMs`, rounded down to a multiple of it). The response adds `"policy":"gnn","tick_ms":N` and the placement. `"placement"` works as for saturate (`least_loaded` | `nearest_first`); for gnn it **defaults to `least_loaded`**, Mercury's resource hand-out policy, when omitted (changed 2026-10-18; it was forced to nearest-first). The gnn branch of `monitor_daemon` calls `add_core(..., scale.placement)`, which goes through `request_cpu_for(placement, ...)`: `least_loaded` → `request_cpu` (least-loaded resource group walk), `nearest_first` → `request_cpu_nearest`. Posting `saturate` or `none` detaches gnn. A start still attaches `saturate`, as before. `"pin"` (default `true`; added 2026-10-18): with `"pin":false` the controller only sizes the cpuset (add/remove cores, breaker, hold) and never pins threads; each tick calls `GnnController::restore()` instead of `pin()`, so every thread keeps the cpuset-wide mask and the kernel places it. The response adds `"pin":true|false`. The DSB operators take `-pin=false` (sent only with `-policy gnn`).
- Model: `weights.json` exported by `gnn-example/mercury/train_gnn.py`, read from `$MERCURY_GNN_MODEL` or else `<dir of the mercury binary>/../models/gnn_v3.json` (that is, `~/Mercury/models/gnn_v3.json`). `gnn_weights` loads it once and reloads it when its mtime changes. `/scale` answers 500 when gnn is asked for and the model cannot be loaded. `make_policy("gnn")` returns `GnnPolicy`, whose `decide()` only holds; the work is done in `monitor_daemon`.
- `src/gnn.{hpp,cpp}`:
  - `GnnModel` is the C++ forward pass with no Python. It has the 17 features of `train_gnn.py` and two delayed graph-filter layers, z_0(t) = u(t), z_k(t) = S(t) z_{k-1}(t-1), layer = Σ_k z_k W_k + b. ReLU follows each layer, then a linear readout with softplus clamped to 1.
  - History is kept per node key (tid; -1 is the aggregate), and a node with no history reads 0.
  - S is row-normalised 0.5·(1 + 10/distance) with no self loops. Distances come from `/sys/devices/system/node/nodeN/distance`.
  - `gnnreplay <weights.json> <case_dir>` runs the forward pass over a recorded case with the training node set. Against the Python model on `bursty-t8-c3` (1,800 ticks × 9 nodes) the max abs difference is 3e-7.
  - `gnn.cpp` and `threadstat.cpp` are compiled with `-O2` even in the default unoptimised build. Inference is 25-65 µs for 2-17 nodes on dtn-01.
- `monitor_daemon` keeps sampling the cgroup ring every 10 ms as before. While `scale.gnn` is set, every `gnn_tick_ms` it runs a `GnnController`, created on the first gnn tick and destroyed when the policy changes away or the daemon stops, in place of `SaturatePolicy::decide`:
  1. Read through an in-process `ThreadStats` on the daemon cgroup.
  2. Nodes are threads whose run+wait share was at least 0.02 within the last 100 ticks (the hold avoids node-set churn). Every other thread is folded into one aggregate node, so nothing is dropped.
  3. Infer per-thread demand: the model output, with no general wait-as-demand. The earlier run+wait safety floor (in every form tried: always, in near-full FFD bins, on saturated shared CPUs) inflated demand and flapped, and was removed. **Stacking breaker** (the only exception): a group is two or more busy threads (prediction ≥ 0.1) whose last CPU is the same and which each waited ≥ 0.2 of the tick. When the same group (same CPU, same tids) persists for K = 5 consecutive ticks (50 ms), its threads get demand ≥ 0.51 for 50 ticks, so FFD cannot put them in one bin. That grants each extra thread its own CPU once. The group is then disarmed until it dissolves. One group is counted at a time. `breaks=1` in the log marks a firing. This replaces the floor's purpose: without it, two threads pinned to one CPU each run about 0.5, the model reads about 0.5, and the stacking holds itself in place.
  4. First-fit-decreasing bin packing of the thread demands (plus the aggregate's measured run+wait) into unit cores gives the required core count, clamped to `min`/`max`.
  5. Hysteresis: scale up immediately to the required count. **Scale-down hold:** remove one core only after the required count has been below the cpuset size for 20 consecutive ticks (200 ms); the count then restarts, so at most one core goes per hold. A tick at or above the cpuset size resets it.
  6. Change the cpuset one CPU at a time with the existing `add_core` (the daemon's `scale.placement`, default `least_loaded`) and `remove_core`, and set `changed_at_ns`.
  7. Pin: each FFD bin holding a thread with demand ≥ 0.1 gets one CPU of the cpuset. A bin keeps a CPU one of its threads is already pinned to; otherwise it takes the next free CPU in cpuset order. Only when cores are exhausted do bins share CPUs. Threads in a bin are pinned with `sched_setaffinity(tid, {cpu})`. Every other thread (idle threads, the aggregate, threads not seen before, which may have inherited a pin) gets the all-CPU mask, so the cpuset alone decides.
- Restore: "wide" is the all-CPU mask (the kernel intersects it with the cpuset, so later cpuset changes apply in full).
  - The controller's destructor widens every thread it pinned, on detach and when the daemon stops.
  - Pinned tids are also kept in a lock-free table that `gnn_restore_all_affinity_raw` (async-signal-safe) walks. `mercury` now installs its SIGINT/SIGTERM handler whether or not it owns `numa_balancing`, and registers the restore with `atexit`.
  - `SIGKILL` of mercury cannot be caught: pins then remain until the threads exit or are re-pinned.
  - Checked on dtn-01: pinned after attach, all back to the cpuset after `policy saturate`, pinned again after `policy gnn`, all back after SIGTERM of mercury.
- Log: one `gnn daemon_id=… cores=… required=… target=… demand=… pred=… breaks=… nodes=… threads=… ticks=… mean_us read=… infer=… cpuset=… act=…` line per second and on every cpuset change.
- `gnn_dataset.sh --workload stressng` (2026-10-18): runs `src/test/wrappers/stressng` (env-driven, Python; Mercury execs it with no arguments) as the daemon. `--threads` are stress-ng worker counts; `--patterns` are `steps` (20/50/80/100/50/20 % cpu-load, `--step-s` each), `ramp` (10→100 % over `--ramp-s`, 1 s steps), `vm` (`--vm N --vm-bytes 1G`) or `stream` (`--stream N`). stress-ng cannot change `--cpu-load` while running, so each step is a new `stress-ng --cpu N --cpu-load L --cpu-method int64` run: workers are new processes per step. Labels (`labels.csv`, header `# spans=1`) give each step's worker tids demand L/100 from their start and 0 at the step end; `result.json` has bogo-ops per step. Other new flags: `--placement` (now default `least_loaded`, sent for both policies), `--max N` (/scale max, default 16), `--fixed-cpus SPEC` (policy none plus `POST /cpus`, for fixed-placement runs). Every case also saves `numastat.before/after` (all nodes).
- `gnn_dataset.sh` takes `--policy gnn|saturate` and `--tick-ms`, and saves each case's `gnn`/`scale` log lines as `controller.log`. It passes `MERCURY_GNN_MODEL`, `MERCURY_GNN_HOLD_TICKS` and `MERCURY_GNN_STACK_K` through its `sudo` re-exec (process environment only, for experiments; recorded in `meta.txt`). `GnnController` reads the last two as overrides of `hold_ticks`/`stack_k`.
- **DAgger retraining (done, 2026-10-17/18):** gnnload cases are recorded with gnn in control (`gnn_dataset.sh --policy gnn`, `MERCURY_GNN_MODEL=<round model>`), labelled with the gnnload ground truth, aggregated with the saturate runs and earlier rounds, and retrained with `train_gnn.py a,b,c` in `gnn-example/mercury`. Data in `data/gnn/dagger/`: `r0` (v1 in control, 5 patterns × threads 4/8/16 × totals 2/3/6/10, 60 cases), `r1-nocpuset`/`r1-cpuset` (v2 trained on saturate runs + v1 runs + r0, in control), `r2` (v3 = + r1, in control), `r2-hold40` (v3 with a 400 ms hold) and `r2-extra` (v3, threads 8, totals 2/10). The cpuset-size feature (feature 7) was dropped: the closed loop was equal and offline MAE was better without it. `--drop-cpuset` zeroes its weights in the export, so the C++ input stays but is ignored. After r2 the gain flattened (under-allocation and demand met unchanged from r1, over-allocation 21 → 17 %), so the rounds stopped there. **Deployed model: `models/gnn_v3.json` (v3, no cpuset feature), now the default path.** `gnn_v1.json`, `gnn_v2_*.json` and `gnn_v3_nocpuset.json` are kept for comparison.
- **v4 model (2026-10-18, not the default):** `models/gnn_v4.json` has the same architecture as v3 (1,633 parameters, cpuset feature dropped). It was trained only on the v4 exponential-demand dataset in `data/gnn/v4`: `d0-s0`/`d0-s100` (saturate, `exp`/`expraw` × threads 2/4/8/16 × totals 1.5/3/6/10, 18 s, 64 cases) → `gnn_v4a.json`; + `dagger-r0` (v4a in control, seed base 200, 24 cases) → `gnn_v4b.json`; + `dagger-r1` (v4b in control, seed base 300) → `gnn_v4.json`. The default path is still `gnn_v3.json`; v4 is selected with `MERCURY_GNN_MODEL`. Evaluation is in `data/analysis/v4-*` (see `v4-README.md`). On unseen exp seeds v4 beats v3 (offline cores MAE 0.34 vs 0.46, too few 13 % vs 22 %; closed loop under 0.8 % vs 1.3 %). On the old patterns, stress-ng and hotel it is equal or worse, so it is not deployed. On social, every v4 run failed at startup (Redis LOADING; 4/4), and so did some saturate and v3 runs.
- **v5 model (2026-10-18, not the default):** `models/gnn_v5.json` has the v3 architecture and is trained on balanced data. It uses 112 v3 training cases (stratified, 22-23 per old pattern, never v3's held-out cases) and the 112 v4 exponential cases → `gnn_v5a.json`. The DAgger rounds are balanced, each with 30 old-pattern cases (threads 4/8/16 × totals 2/4.5) and 30 exp cases (threads 4/8/16 × totals 2/3/4.5/6/10): `data/gnn/v5/dagger-r0-*` (v5a in control, seed base 400) → `gnn_v5b.json`, and `dagger-r1-*` (v5b, seed base 500) → `gnn_v5.json`. Select it with `MERCURY_GNN_MODEL`. Results are in `data/analysis/v5-*`.
- **gnn-slo-v1 (design v3, 2026-10-19, `data/analysis/gnn-slo-v1/DESIGN.md`):** a hotel-only fine-tune of v5 with the same model and C++ forward pass, saved as `models/gnn_slo_v1.json` (non-default). Social is an unseen test system.
  - **History.**
    - v1 used the lowest p95 of a uniform-k sweep at 768 users. It was dropped: the load was past the knee.
    - v2 was a below-knee sweep without Jaeger. Its data is in `data/dsb/slo-v2` and is marked `NOT-JAEGER.txt`; it is not used.
  - **Loads.**
    - Training: hotel `-users 32`.
    - Evaluation: hotel 24 and social 12, plus the default 1024 for both.
  - **Sweep** (`data/gnn/slo-v1/collect3.sh`, data in `data/dsb/slo-v3/hotel`):
    - The generous configuration G puts heavy services at 3 cores and light ones at 2.
    - A warm-up run comes first and is discarded.
    - Then 2 passes of: G, each heavy service alone at k=1/2/4, all light daemons at 1. A G run is added mid-pass, and one more at the end.
    - Afterwards, G is run at the eval loads as references.
  - **Jaeger.** Every sweep and eval run uses Jaeger, for every policy.
  - **Label** (`slo_ref3.py`):
    - The reference is the median p95 per level and route over the 5 G runs.
    - Score is the mean of the route p95/ref values, weighted by the daemon's Jaeger tail critical-path share (`crit_share_tail` ≥ 0.02). A daemon that is never on a critical path scores 1.
    - C* is the smallest k whose median score over the passes is ≤ 1.2. If no k meets the margin, the level is unlabelled.
  - **Loss.** (1+e)·relu(T−P)² + λ_alloc·relu(P−T) + λ_chg·100·relu(|ΔP|−0.25) + 0.1·shape, with T = C*−0.3.
    - λ_alloc is the largest in {0.03…1} whose held-out under-allocation is at most 1.5× that of 0.03.
    - λ_chg is the largest in {0…0.1} whose under-allocation is at most 1.2× and core-seconds at most 1.05× those of λ_chg = 0.
    - The change term is train-time only.
  - **Window stamps.** `stamp_windows.py` stamps each operator window line with CLOCK_MONOTONIC.
  - **Drift control.** The score is divided by the run's median p95/ref over the (level, route) pairs on which d is never on the tail critical path, a difference-in-differences correction. Without it, hotel's mean label is 1.21 instead of 1.12.
  - **Labels** (hotel, 2026-10-19 12:00). Every level is labelled; the mean is 1.12 cores.
    - Most daemon-levels get 1 core. Exceptions: geo 3 at some levels with 8-32 clients, user 3 at 8-32-8 clients, mongod 2 at 2 levels.
    - G leave-one-out median is 0.96-1.10.
  - **Model `models/gnn_slo_v1.json`** (2026-10-19 12:09, non-default, same format as `gnn_v5.json`).
    - Trained on 114 hotel cases and held out on 20, picking λ_alloc = 0.03 and λ_chg = 0.1.
    - Held-out results: under-allocation 1.36 % (v5: 7.9 %), core-s 1866 (v5: 2062; label: 1018), requested-core changes 4.5/s (v5: 30.7/s, before the controller hold).
    - Training output: gnn-example `mercury/results/gnn-slo-v1/train`.
  - **Evaluation** (`evaluate3.sh`, 12:10-13:06, no pin, least_loaded, 2 repeats, Jaeger on for all policies). Each cell is gnn-slo-v1 vs v3 vs saturate; full tables are in `data/analysis/gnn-slo-v1/eval/summary.md`.
    - **Core-s allocated:**
      - hotel 24: 1835 / 2285 / 1562
      - hotel 1024: 4710 / 8330 / 3766
      - social 12: 1396 / 1819 / 1266
      - social 1024: 10490 / 13705 / 4687
    - **Changes/s:**
      - hotel 24: 3.9 / 13.7 / 0.3
      - hotel 1024: 5.8 / 23.6 / 0.2
      - social 12: 1.0 / 3.1 / 0.2
      - social 1024: 2.1 / 3.7 / 0.2
    - **p95 / G reference** (median; share of level-routes within 1.2×):
      - hotel 24: 0.87 (99.5 %) / 0.70 (83 %) / 0.75 (94 %)
      - hotel 1024: 0.70 / 0.75 / 0.70
      - social 12: 0.50 / 0.56 / 0.60
      - social 1024: 0.47 / 0.38 / 0.40
    - **Social Jaeger caveat.** `traces.csv` only holds window 0 for social; hotel is complete.
  - **Operator flags.**
    - `goperate` takes `-svc-min`/`-svc-max` (default 1/16), and its thread caps stay at 16.
    - Both operators take `-svc-fixed name=k,...`: per-program `/scale` min = max, keyed by the program basename. Unlisted daemons use `-svc-min`/`-svc-max`.
    - Hotel `operate` caps service threads at a constant `threadCapCores = 16` instead of `-svc-max`. Before this, `-svc-max 1` left mongod unable to start.
    - Backups: `~/Mercury-backup-slo-20261019-085421`.
- **Jaeger in the DSB operators (2026-10-19 10:17).** Patches are from `~/Benchmarks/jaeger/patches/*-jaeger-on-dtn01-worktree.patch` (Jaeger v2.22.0, `~/Benchmarks/jaeger/README.md`).
  - **How applied.** Applied to both operator working trees with plain `git apply`. `git apply --3way` refused because the files have uncommitted changes that do not match the index. The index is untouched and nothing was committed.
  - **Build.** Rebuilt hotel `bin/loadgen` and social `goperate/bin/{goperate,loadgen}`. vet and tests pass.
  - **Backup.** `~/Mercury-backup-jaeger-20261019-101702` holds the status, diff patch and HEAD of both repos, plus a tar of the operator sources.
  - **Flags.**
    - Off by default.
    - gnn-slo-v1 runs use `-jaeger -jaeger-port-offset 20000 -jaeger-cpus 62 -trace-sample 0.02` (bare mode).
    - Ports are 24317-36686 on 127.0.0.1, clear of 8080 and 18180-18199.
    - CPU 62 is outside the Mercury pool 1-60; `dsb_ab.sh`'s threadtrace helpers also use 62-63.
  - **Output.** Per run, `traces.csv` (window × route × daemon critical-path shares) and `traces.jaeger.log` sit next to `loadgen.out`.
- **Closed-loop A/B** (`eval_closed_loop.py`, 30 cases: 5 patterns × threads 4/8/16 × totals 3/6, 15 s, 10 ms; saturate baseline `data/gnn/ab-saturate/20261017-222708`):

  | metric | saturate | v1 + floor | r0 v1 | r1 v2 | r2 v3 | r2 v3 hold 400 ms |
  |---|---|---|---|---|---|---|
  | under % | 28.6 | 0.16 | 11.5 | 0.82 | 0.86 | 0.92 |
  | over % | 21.5 | 62.5 | 33.1 | 21.2 | 16.7 | 20.0 |
  | core-s alloc/req | 0.983 | 1.324 | 1.053 | 1.061 | 1.050 | 1.065 |
  | up lag p90 s | 0.46 | 0.03 | 0.04 | 0.05 | 0.06 | 0.05 |
  | up never met | 40 | 0 | 7 | 0 | 2 | 5 |
  | down lag p90 s | 1.22 | 0.72 | 0.83 | 1.14 | 0.94 | 1.35 |
  | down never met | 58 | 34 | 32 | 19 | 20 | 33 |
  | stacked busy % | 4.15 | 0.08 | 0.70 | 0.25 | 0.24 | 0.20 |
  | demand met % | 91.2 | 100.3 | 97.6 | 99.5 | 99.5 | 99.6 |
  | changes/s | 0.23 | 2.71 | 0.84 | 0.96 | 0.67 | 0.49 |
  | ctl read/infer/act µs | n/a | 298/52/132 | 286/56/121 | 284/56/118 | 297/57/122 | 284/56/117 |

  "v1 + floor" is the earlier run+wait floor with the 20-tick window. Columns r0 onward use the breaker and the 200 ms one-core-per-hold, except the last (400 ms hold: fewer changes, more over-allocation, so 200 ms stays the default). Caveat: the evaluation cases use the same seeds as the training configurations, so this is not a held-out test.
- **stress-ng held-out test (2026-10-18, v3, both policies `least_loaded`, max 32):** `data/gnn/stressng/ab-{saturate,gnn}`, 16 cases each: workers 2/4/6/8/12/16/24/32 × `steps` (20/50/80/100/50/20 %, 3 s each) and `ramp` (10→100 % over 15 s). Truth is cpu-load/100 per worker. All 16 cases / only ≤ 16 workers (the pool on nodes 0-2 has 23 CPUs, so 24 and 32 workers at high load cannot be met by either policy):

  | metric | saturate | gnn v3 | saturate ≤16 | gnn ≤16 |
  |---|---|---|---|---|
  | under % | 46.1 | 32.1 | 34.2 | 22.2 |
  | over % | 23.0 | 25.4 | 24.6 | 23.1 |
  | core-s alloc/req | 0.784 | 0.815 | 0.964 | 0.901 |
  | up lag p90 s | 5.59 | 3.16 | 5.64 | 3.26 |
  | up never met | 16 | 7 | 2 | 1 |
  | down lag p90 s | 2.99 | 1.74 | 2.99 | 1.55 |
  | down never met | 10 | 1 | 7 | 0 |
  | stacked busy % | 26.0 | 27.6 | 23.6 | 23.9 |
  | demand met % | 73.5 | 78.0 | 87.4 | 84.4 |
  | changes/s | 0.77 | 1.99 | 0.61 | 1.67 |
  | bogo-ops per core-s | 1,762 | 1,792 | 1,745 | 1,803 |
  | ctl read/infer/act µs | n/a | 424/98/285 | | |

  gnn generalises in direction (less under-allocation, faster up and down) but is far from its gnnload numbers. Two stress-ng properties hurt both policies: `--cpu-load` duty-cycles on wall time, so a contended worker also sleeps less work and its run share understates the target (demand is elastic downward, unlike gnnload); and every step restarts the workers (new tids every 1-3 s), so gnn's per-thread history restarts and changes/s is high. A DAgger round on stress-ng would leak the test set; it is kept held out.
- **Memory preview (informational, fixed cpuset, policy none, 4 workers, 20 s, `data/gnn/stressng/mem-*`):** `--vm 4 --vm-bytes 1G`: 442k bogo-ops/s on 4-7 (node 1) vs 499k on 4-5,8-9 (nodes 1+2, +13 %; numa_hit split evenly, other_node 0 %). `--stream 4`: 3,954 vs 2,870 bogo-ops/s (−27 % across two nodes). numastat counts allocations only, so stream (allocates once, before the 10 ms trace starts) shows almost no deltas; phase 2 needs per-access counters (the PROPOSED uncore/offcore events), not numastat.
- Fixed 2026-10-18 in `gnn_dataset.sh`: the wait for the daemon's `cgroup.threads` used `[ -s ]`, which is always false on cgroupfs, so every case waited the full 5 s before tracing. Earlier gnnload runs lost their first 5 s of trace (labels still covered the traced part, so results stand).
- **DSB tooling (2026-10-18):** `src/test/dsb_ab.sh hotel|social OUT REPEATS "<operator args>" [--trace]` runs alternating saturate/gnn repeats with the DeathStarBench operators (`deathstar-hotelReservations/bin/loadgen` = `operate`, `deathstar-socialNetwork/goperate/bin/goperate`; both take `-policy`, `-tick-ms`, `-placement` (default `least_loaded`) and `-window-ms`, and print p95 and p99). Each run starts its own `mercury 8080 0 1..60` (worker on CPU 0; note that the first CPU argument is Mercury's own CPU, so the operators' comment `mercury 8080 1 2 ... 60` put Mercury on CPU 1, the first daemon home), the operator's `autoscale` (which starts and kills every service), and `src/test/dsb_sample.py` on CPU 63. That samples every 100 ms per daemon cgroup: cpuset size, `cpu.stat` usage, thread count, busy threads (schedstat run ≥ 50 % of the interval), cross-daemon pin collisions (busy threads of different daemons whose last CPU is the same) and CPU 0 utilisation. Mercury kills and removes the daemon cgroups it created on SIGTERM (see Process start); `dsb_ab.sh` still writes `cgroup.kill` and removes the daemon cgroups after each run, which also covers a Mercury that was `SIGKILL`ed. `--trace` records threadtrace (50 ms) of five services in repeat 1 of saturate, for a later DSB DAgger round. Stale cgroups from an earlier run (same daemon ids, `cpuset.cpus` pointing at Mercury's CPU) make `/start` fail with exit 127, because the new daemon is moved into them; they were removed (list in `~/dsb-stale-cgroups-removed.txt`).
- **Social Redis state fix (2026-10-18):** social runs had begun failing with `loadgen error: compose-post: LOADING Redis is loading the dataset in memory`. Redis (wrapper `~/Benchmarks/runtime/redis/redis-server`) saved `6379/dump.rdb` on every shutdown, so the dump grew with every run's posts. It reached 1.06 GB, which took 18 s to load. During loading Redis accepts TCP but answers `-LOADING`, and goperate only checked the port and then gave the first route 10 s. Fixes:
  - The wrapper now passes `--save "" --appendonly no`, so every start is empty and runs start from equal state.
  - `goperate` `waitRedis` (`mercury.go`) PINGs after `waitPort` until it gets `+PONG`, with a 120 s timeout.
  - `waitReady` (`routes.go`) extends its 10 s window while the last error contains `LOADING`, up to 120 s.
  - The old dump and 22 orphaned `temp-*.rdb` files (about 4.7 GB, saves cut off by `cgroup.kill`) were deleted.
  - Backup: `~/Mercury-backup-redisfix-*`.
- **DSB A/B (2026-10-18, v3, least_loaded for both, 3 alternating repeats, users 1→1024→1, 4 × 2.5 s windows per step, `-conns 16`, loadgen on saturate; `data/dsb/{hotel/20261018-024007,social/20261018-031106}`):**

  | metric | hotel saturate | hotel gnn | social saturate | social gnn |
  |---|---|---|---|---|
  | throughput (req/s, mean over steps) | 37,047 | 37,162 | 8,385 | 5,848 |
  | p95 / p99 median over windows (ms) | 31.5 / 54.5 | 37.7 / 58.7 | 23.1 / 31.5 | 29.4 / 41.9 |
  | p99 at 1024 users (ms) | 940 | 671 | 554 | 671 |
  | errors | 0 | 0 | 0 | 0 |
  | core-s allocated / used | 7,290 / 3,675 | 13,823 / 3,349 | 7,002 / 3,043 | 11,000 / 2,399 |
  | used / allocated | 0.50 | 0.24 | 0.43 | 0.22 |
  | cpuset changes/s (all daemons) | 0.07 | 21.4 | 0.15 | 6.9 |
  | cross-daemon pin collisions | 0 | 0 | 0 | 0 |
  | Mercury CPU 0 utilisation | 30 % | 88 % | 31 % | 72 % |
  | gnn read / infer / act µs per tick | | 538 / 110 / 2,212 | | 703 / 138 / 4,091 |

  Max threads: hotel mongod 352, others ≤ 22; social rabbitmq 153 (kept on saturate with `goperate -saturate-only rabbitmq-server`, read ≈ 9 ms at 10 ms tick), mongod ~155, Thrift services 40-78. Findings: v3 is out of distribution on DSB. It allocates about 2× the core-seconds of saturate for equal (hotel) or lower (social, −30 %) throughput, flaps (scale-up immediate on bursty request-driven threads), and its `act` step (cpuset writes plus per-thread pins for 15-18 daemons every 10 ms) keeps Mercury's worker CPU at 70-90 %. Low-load latency is worse under gnn (1-2 users: hotel throughput 8-9k vs 15-16k req/s); peak-load hotel p99 is better. The collision metric (busy = ≥ 50 % run per 100 ms) found none in either policy; DSB threads are rarely that busy. Needed before gnn is competitive here: a DSB DAgger round (labels: run+wait under a generous fixed allocation, masked where contended), a longer tick or event-driven acting for many daemons, and down-weighting wait for request-driven threads. threadtrace (50 ms) of 5 services per system from saturate repeat 1 is kept for that.
- Not built: the 10 × inference tick is a measured guideline (10 ms default), not computed at runtime.
- **DSB tooling additions (2026-10-18, later):** `dsb_ab.sh` takes `POLICIES` (space-separated; `gnn-nopin` = gnn with `-pin=false`; a token `gnn@/abs/model.json` or `gnn-nopin@...` runs that mercury with `MERCURY_GNN_MODEL`; run dir `<r>-<policy>-<model stem>`), `TRACEALL=MS` (threadtrace every daemon of the system at MS ms from the load generator start; `TRACE_SLOW` daemons at 50 ms), `SVC_ARGS` (extra operator args, e.g. `-svc-min 8 -svc-max 8` = fixed allocation; hotel operator only) and `NOTRACE`. `dsb_sample.py` also records per daemon `wait_ns`/`slices` (summed schedstat run-queue wait and timeslice deltas): the wake-up delay proxy, because perf is not installed on dtn-01. Hotel `-svc-max 4` fails: mongod exits with code 2 at `MONGO_WT_*_TRANSACTIONS=4`.
- **No-pinning check (social 1->64->1, 1 repeat each, v3, least_loaded; `data/dsb/social-pin/20261018-120642`, analysis `data/analysis/dsb-pin-check`):** throughput saturate 7,200 / gnn pin 5,137 / gnn no-pin 5,555 req/s (no-pin +8 % over pin, -23 % vs saturate); p99 median 11.0 / 16.8 / 15.7 ms. Wake-up proxy (wait per timeslice, all service threads, 10 s at 2 users): up-leg 70 / 114 / 125 us, down-leg 129 / 242 / 157 us. Pinning explains a small part of the gnn loss; most remains with no-pin.
- **Hotel-trained GNN:** recording `data/dsb/hotel-record/20261018-122034` (saturate, every service fixed at 8 cores, cpusets overlap ~2x on the 60-CPU pool; users 1->1024->1; threadtrace 10 ms on 14 services, mongod/loadgen at 50 ms and not used). Label = each thread's own run share per tick, clipped 0..1 (gnn-example `mdata.load_self`, `selflabel` file per case, `diag/dsb_cases.py`). Caveat: wait/run is ~1 even at 1 user (about 30 us wake-up latency per slice) and up to ~5 (about 1 ms per slice) at 256-1024 users: the box saturates at peak, so peak labels under-state demand. hotel-r0 trained on it (v3 architecture, `--drop-cpuset`); DAgger r1 = hotel-r0 in control (gnn pin=false, `data/dsb/hotel-dagger1/20261018-122925`), relabelled only on uncontended thread-ticks (contended = wait share >= 0.05 and wait per slice >= 100 us, masked), trained on r0+r1 with fixed held-out daemons (`--test-cases`). Models `models/gnn_hotel_r0.json`, `models/gnn_hotel_r1.json`. Offline (held-out daemons, 101k ticks): thread MAE v3 0.076 / r0 0.041 / r1 0.039, cores (FFD) MAE 0.71 / 0.24 / 0.22, but busy-thread MAE 0.15 / 0.22 / 0.23 (hotel models under-predict busy threads). Closed loop hotel (`data/dsb/hotel-cmp/20261018-123812`, 2 repeats, gnn pin=false): throughput saturate 36,813 / v3 36,413 / hotel-r1 28,981 req/s; p99 median 54.5 / 54.5 / 92.3 ms; core-s allocated 7,155 / 15,303 / 3,763. Social held-out (`data/dsb/social-cmp/20261018-130417`, 2 repeats): v3 5,849 vs hotel-r1 2,263 req/s, p99 14.7 vs 32.5 ms. hotel-GNN under-allocates; v3 remains the default. A second DAgger round was not run.

### Graph

- Nodes: every thread whose CPU since the last tick (task-clock delta over the tick) is above 5% becomes its own node. The remaining threads of a cgroup are merged into one aggregate node per cgroup, with summed deltas, so no thread is silently dropped.
- Caps and costs: each per-thread node costs about 3 perf fds plus one `schedstat` read per tick. The number of per-thread nodes per daemon is capped (exact cap open, see below). Threads past the cap go into the aggregate node.
- Edges: (a) between nodes in the same cgroup; (b) between threads whose current CPUs share a NUMA node, weighted by the NUMA distance from `/sys/devices/system/node/nodeN/distance`.
- Node features: task-clock delta, wait delta, context switches, migrations, current CPU/node (one-hot or distance row), cgroup occupancy and pressure, and a short delayed history: a k-hop aggregation with delays, as in lab 5's `LocalGNN_DB`.

### Outputs and actuation

- Model output: a target CPU for each thread (or a node plus a per-CPU score). The cgroup core count is derived from it: `ceil(sum of thread demand / 0.75)`.
- Actuation, in two steps. First, the cgroup cpuset is set to the outer bound through the existing `add_core`/`remove_core`/`write_cpuset` path, with `cpuset.mems` following as today. Second, each thread is pinned inside that cpuset with `sched_setaffinity(tid, ...)`.
- Safety: Mercury clamps and validates every output before applying it:
  - core counts stay within `min_cores`/`max_cores`;
  - CPUs come only from the allocatable pool and never include the worker CPU;
  - two busy threads are never pinned on one CPU unless cores are exhausted.
- Clean state: when the policy is detached, the daemon stops, or Mercury shuts down, every thread's affinity is reset to the full cpuset. No hidden pinning is left behind.
- New and exited threads: a new tid keeps the cpuset-wide affinity it inherits until the next tick. An exited tid (`ESRCH`, or gone from `cgroup.threads`) has its fds closed and its state dropped.

### Training

- Method: imitation learning, as in the UPenn GNN lab 5.
- Expert: a centralized optimizer with global information:
  - demand per thread = run + wait time;
  - cores = `ceil(sum demand / 0.75)`;
  - packing nearest-first in NUMA order, one busy thread per CPU.
- Data: `partial_server` runs, `src/test/traces`, and later `~/Benchmarks`.
- Prototyping: in the Python/torch repo `/home/pager/Documents/gnn-example`, outside this repository.
- Deployment: the trained weights (small, about 1-2k parameters) are exported and run as a handwritten C++ forward pass, or through ONNX Runtime, inside Mercury. There is no Python in the loop. The lab 5 forward pass measured under 1 ms, which fits a 1 s tick.

### Evaluation

`gnn` is compared A/B against `saturate` with `least_loaded` and with `nearest_first` placement, using `autoscale_test`, `autoscale_nearest_first`, `saturate_it.sh` and `partial_server`. The metrics are:
- tier misses;
- stacking (busy threads per CPU);
- time to reach the target core count;
- overshoot;
- scale-down time;
- inference latency.

### Open questions

- Node granularity: the per-daemon cap on per-thread nodes, and when to fall back to cgroup-level nodes.
- How an aggregate node maps to CPUs (shared remainder of the cpuset, or its own CPU).
- Pinning and NUMA balancing: Mercury sets `kernel.numa_balancing` to 0 by default (`--numa-balancing=keep` opts out). It is open how per-thread pinning behaves when balancing is kept on.

### Code facts this design depends on

- `src/counters.cpp` has `open_counters` and `open_clock`, but nothing in `daemons.cpp` calls them; the record keeps no per-thread rings. Phase 1 added `open_software` (the software trio as one group), used only by `threadstat`/`threadtrace`, not by `mercury`.
- `open_counters` opens the hardware group first and the software group (`task-clock`, `page-faults`, `context-switches`, `cpu-migrations`) only as a fallback. When the hardware group is used, `task-clock` is a separate fd outside the group. `kMaxCounters` is 8.
- `sched_setaffinity` is used by `pin_to` (the worker CPU, `src/mercury.cpp`), `pin_pid`, the pin or `widen_to_online` in the start child (`src/daemons.cpp`), and since phase 1b by `GnnController` (per-tid pins and the all-CPU reset, `src/gnn.cpp`).

### Proposed: phase 2, NUMA/memory-aware scaling for distributed memory demand

**PROPOSED, NOT IMPLEMENTED.** The target workload is MongoDB (`mongod`, as in the `hotel-mongod` and `social-mongod` cgroups), whose demand is memory as much as CPU: a cpuset that grows across NUMA nodes without moving memory turns local accesses into remote ones.

Candidate signals, and what dtn-01 actually exposes (checked on 2026-10-17: AMD EPYC 7301, 8 NUMA nodes of 4 cores/8 CPUs, kernel 6.12.107+deb13-amd64; `perf` is not installed, so this is from sysfs):

- Core PMU (`cpu`, perf type 4): `cpu-cycles`, `instructions`, `cache-references`, `cache-misses`, `branch-*`, `stalled-cycles-frontend/backend`, plus raw events (`event` config:0-7,32-35, `umask` 8-15, `cmask`, `edge`, `inv`). These are per thread, through the same `open_group` path. `open_counters`' hardware group already opens most of them. Miss rate per instruction is a locality proxy.
- `amd_l3` (type 11): L3 PMU, per CCX (cpumask 0,2,...,30). Format `event`, `umask`, `slicemask`, `threadmask`. There are no named events in sysfs, so raw codes from the Zen 1 PPR are needed (for example L3 accesses and misses). Counted per CCX, not per thread.
- `amd_df` (type 10): Data Fabric PMU, one counter set per die (cpumask 0,4,...,28). Format `event` (config:0-7,32-35,59-60), `umask`. No named events. DRAM channel and remote-link traffic need raw Zen 1 DF event codes. This is the per-node memory bandwidth signal, system-wide per die, not per daemon.
- There is no `amd_umc` PMU (that is Zen 4 and later) and no `ibs_op`/`ibs_fetch` PMU on this kernel, so there is no per-access sampling of remote accesses.
- `msr` (aperf/mperf/tsc) and `power` (`energy-pkg`) also exist.
- `/sys/devices/system/node/nodeN/numastat`: `numa_hit`, `numa_miss`, `numa_foreign`, `interleave_hit`, `local_node`, `other_node`. These are system-wide page-allocation counts per node, not per daemon and not accesses.
- `/sys/devices/system/node/nodeN/distance`: 10 local, 16 within the socket (nodes 0-3 and 4-7), 32 across sockets.
- cgroup `memory.numa_stat`: the memory controller is available (`/sys/fs/cgroup/mercury/cgroup.controllers` lists `cpuset cpu memory pids`), but `mercury/cgroup.subtree_control` enables only `cpuset`, so the daemon cgroups have no `memory.*` files apart from `memory.pressure`. Reading per-node resident memory per daemon needs `+memory` in `mercury/cgroup.subtree_control`, at runtime when Mercury creates the hierarchy. `memory.pressure` (PSI) is there today.
- Per process: `/proc/<pid>/numa_maps` (pages per node per mapping; expensive on large heaps) and `move_pages(2)` with a NULL nodes argument (queries the node of given pages).

Actuations:
- `cpuset.mems` already follows `cpuset.cpus`. Today it only grows and shrinks with the CPUs' nodes; a policy could keep it at the node(s) holding the memory instead.
- Page migration: `cpuset.memory_migrate` (cgroup v1 only; not in v2), `migrate_pages(2)` / `migratepages` (installed at `/usr/bin/migratepages`), and `move_pages(2)` for hot ranges.
- Interleave across a set of nodes (`set_mempolicy(MPOL_INTERLEAVE)` inherited at start, or `numactl --interleave`, installed) for bandwidth-bound daemons.
- Mercury keeps `kernel.numa_balancing` at 0 while it runs. Phase 2 has to decide whether the policy or the kernel moves pages.

Synthetic memory workloads (a `gnnload` sibling, still to be built): a configurable working set per thread, a local-vs-remote access ratio (memory bound with `mbind` to chosen nodes), streaming vs random (pointer-chase) access, threads per node, and a labels file with the ground truth (bytes per second per thread per node) as in phase 1.

Open: there is no named event list for `amd_df`/`amd_l3` on this kernel, so the raw codes must be validated against a known STREAM-like load before they are used as features.
