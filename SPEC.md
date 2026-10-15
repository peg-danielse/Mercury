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

CPU numbers in `start` and `migrate` are Linux CPU numbers, the same numbers `cpus` writes into a cpuset. The worker's own CPU is the lowest number in its share and is never part of a daemon cpuset.

On `start`, the worker places the daemon in a cgroup and sets that cgroup's cpuset to that Linux CPU. It pins the child, then the child widens its affinity to every online CPU except the worker. The cpuset is the limit. Later threads are not pinned one by one; they run on whatever the cpuset allows. The worker raises its own open-file soft limit to the hard limit before it listens, and each daemon does the same before `exec`. `exec_path` is run with no arguments unless `listen_port` is set. Then the worker binds `127.0.0.1` on that port, passes the socket as fd 3, and the arguments are `--listen-fd 3`. Stdout and stderr are read until the child exits, then `exited` is sent once. The exit code and log stay those of the process that was `exec`'d.

`env`, when present, is an object of strings. Each name matches `[A-Za-z_][A-Za-z0-9_]*`. Those names are added to the environment the worker itself inherited, replacing an inherited value with the same name. Every other inherited variable stays. A missing `env` changes nothing. A bad name or a value that is not a string is rejected and the process is not started.

`migrate` starts a new process with the same `exec_path`, the same listen socket, and the same `env`, then stops the old one. It requires a listen socket. The returned pid is the new process.

`cpus` writes the daemon cpuset. `cpus` is a Linux cpuset string, such as `1-4`. The set must not contain the worker's Linux CPU.

## Monitoring

The worker reads `/proc/<pid>/task` for the daemon and its descendants. Each thread gets its own ring, owned by that daemon, up to 64 threads. Past that, only the 64 threads with the most CPU time stay open, so idle threads do not hold descriptors. A core is not removed on a tick where a counter failed to open. A sample is taken every 10 ms and the ring keeps 60 s. The value used for scaling is task-clock, the nanoseconds the thread was scheduled in that gap. The first 16 monitored threads also record the hardware group and an archive file. Later monitored threads record task-clock only.

The cgroup `cpu.stat` total for every task in the daemon is written to `rings/<daemon_id>.mring`. Its `task-clock`, `user`, and `system` values are the nanoseconds the whole daemon was scheduled in that gap. That ring is not a thread and is not part of the scaling sum.

A thread counts as busy when its task-clock, divided by its own gap and capped at 1, is at least the larger of one eighth of the busiest thread and 2% of the gap. Occupancy is the sum of those per-thread fractions divided by the number of CPUs in the cpuset. One busy thread on one CPU is full. Several busy threads squeezed onto fewer CPUs are not.

## Scaling

`scale` attaches a policy to a running daemon. `min` defaults to 1 and `max` defaults to 16. `policy` defaults to `saturate`. An unknown policy is rejected. Posting again updates the limits and the policy and does not clear the window opened by the first attach.

`saturate` decides from the last 10 samples, which is 100 ms:

- If the cpuset has fewer CPUs than `min`, add one immediately. If it has more than `max`, remove one immediately.
- Add one CPU when every sample in the window has occupancy of at least 0.90 and more busy threads than CPUs granted, and the cpuset is still under `max`. After an add, the next decision waits for a fresh window, so one saturated window cannot add every CPU.
- Remove one CPU when every sample has fewer busy threads than CPUs granted and the occupancy shows that spare CPU idle: at or below `(cores - 1) / cores`, plus 0.02. A removal does not clear the window. While those samples still show spare CPUs, one CPU is dropped per sample until the cpuset matches the busy threads, and not below `min`.

The CPU added comes from `request_cpu` (`src/resources.cpp`). That walk is round-robin across socket, NUMA node, L3, and core. It skips a CPU this daemon already holds, and it skips a CPU whose core load is at least `kCoreFull` (0.90). Below that line the same Linux CPU may be added to another daemon, so two daemons can run on one CPU while the core is not overloaded. When every remaining CPU is already in this cpuset or sits on an overloaded core, nothing is added. The CPU removed is the highest one in the cpuset.

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

`main` reads `<port> [cpu...]`. With no CPU arguments, `online_cpus` reads `/sys/devices/system/cpu/online` and that list is the share; CPU 0 must be in it. `parse_cpu` accepts a Linux CPU from 0 through `CPU_SETSIZE - 1` and rejects duplicates. The lowest number is the worker CPU. `pin_to` calls `sched_setaffinity` on that CPU and the process stays there. The other numbers are sorted into `daemon_cpus`. `raise_nofile` (`src/daemons.cpp`) lifts the soft open-file limit to the hard limit, or to 65536 when the hard limit is infinite. `set_mercury_cpu` stores that Linux CPU. `set_daemon_cpus` stores the sorted daemon pool. `serve` then blocks in the accept loop.

`daemon_cpus` and `mercury_linux_cpu` are written on this thread before any other thread starts. Later readers do not lock them. `main` passes `daemon_cpus` to `init_resources` (`src/resources.cpp`) before `serve`.

`route` is the only dispatch. It tries `ping_route`, `stat_route`, `start_route`, `migrate_route`, `cpus_route`, `scale_route`, then `kill_route`. The first function that recognizes the target returns the response. Anything else is HTTP 404 from `http_response`.

### HTTP accept loop

Files: `src/http.cpp`, `src/http.hpp`.

`serve` calls `listen_on`, which binds `127.0.0.1` with backlog 16, then `serve_fd`. `serve_fd` accepts one connection and calls `handle_client` on the same thread. `handle_client` calls `read_request`, then the `route` function, then `send_all`, then closes the socket. `read_request` stops at 64 KiB. `request_target` and `http_target` parse the request line.

This accept thread is a control-flow section. `launch`, `migrate_route`, and `kill_route` run inside it, so a placement wait, an image wait, or a kill wait delays the next request. No second route runs until `handle_client` returns.

`Arrivals` and `Arrivals::mu` count requests inside `handle_client`. `/arrivals` reads that count. Daemon routes do not take this lock. `ping_route` answers `ping` and `GET /ping`. `relay_route` and `ping_reply` belong to the hello-server relay and are not on the `mercury` route list.

### Daemon table

File: `src/daemons.cpp`. Header: `src/daemons.hpp`.

The table is the vector `daemons` of `Daemon`. `find_daemon` walks it by `daemon_id`. The mutex is `daemons_mu`. The condition variable is `daemons_cv`. Every field of `Daemon` is read and written under `daemons_mu`: identity, `exec_path`, `pid`, `running`, `exit_code`, `log`, `finished`, `threads`, `scale`, `cpu`, `listen_fd`, `counters_done`, and `env`.

`ScaleState` holds `enabled`, `min_cores`, `max_cores`, `policy`, and `added_at_ns`. `ThreadRing` holds a tid and a `shared_ptr<Ring>`.

Holders of `daemons_mu`:

| Function | What it does under the lock |
| --- | --- |
| `daemon_running` | True when this `daemon_id` still has this pid and `running` |
| `stat_route` | `{ok:true}` when some running daemon was started with this `exec_path` |
| `start_route` | Rejects the start when this `daemon_id` is already running |
| `migrate_route` | Copies `exec_path`, `listen_fd`, `pid`, `cpu`, and `env` of a running daemon that has a listen socket and a different cpu |
| `arm_daemon` | Inserts the record, or replaces it and closes a different previous `listen_fd` |
| `watch_daemon` | Sets `running` false, `exit_code`, `log`, and `finished`, then `notify_all` |
| `mark_counters_done` | Sets `counters_done` when the pid still matches, then `notify_all` |
| `remember_thread`, `forget_thread` | Push or erase a `ThreadRing` when the root pid still matches |
| `monitor_daemon` | Copies `ScaleState` and the ring pointers; after a scale-up, writes `added_at_ns` |
| `scale_route` | Attaches or replaces the policy and limits; the first attach sets `added_at_ns` |
| `kill_route` | Holds a `unique_lock` across the waits below |

`kill_route` is the long critical section. It takes `daemons_mu`, sends `SIGTERM`, and `wait_for`s up to 2 seconds until `watch_daemon` sets `finished`. On timeout it sends `SIGKILL` and `wait`s until `finished` is set. It then `wait_for`s up to 2 seconds for `counters_done`. It closes `listen_fd` before releasing the lock. `watch_daemon` and `mark_counters_done` take the same mutex only to publish and notify, so this wait can complete.

`arm_daemon` runs on the accept thread after `launch` returns. It publishes the record, then detaches the two threads. `watch_daemon` and `monitor_daemon` ignore the record when the pid no longer matches, so a migrate that replaces the record leaves the old threads to exit without writing the new daemon.

Work outside the lock: fork, cgroup writes, perf reads, `Ring::push`, `Ring::recent`, and `Policy::decide`. The monitor copies `shared_ptr<Ring>` under `daemons_mu`, drops the lock, then calls `recent`. It never takes `daemons_mu` while holding `Ring::mu_`.

### Start and migrate

File: `src/daemons.cpp`.

`start_route` parses `daemon_id`, `exec_path`, `cpu`, optional `listen_port`, and optional `env`. `parse_env` and `valid_env_name` accept an object of strings whose names match `[A-Za-z_][A-Za-z0-9_]*`. A bad name or a non-string value fails the request and skips `launch`. The cpu must be in `daemon_cpus`. `access(exec_path, X_OK)` must succeed. `bind_loopback` binds `127.0.0.1` when `listen_port` is set.

`launch` is the placement critical section. `prepare_placement` calls `ensure_daemon_cpuset`, which creates `/sys/fs/cgroup/mercury/<daemon_id>` and enables the cpuset controller through `ensure_cpuset_controller` and `controller_enabled`. It writes `cpuset.cpus` to that Linux CPU when that file is empty, then polls `cpuset_applied` for up to 50 times at 10 ms. `cpuset_applied` requires `cpuset.cpus` and `cpuset.cpus.effective` to both equal the requested list. An existing non-empty cpuset is left as it is, and the child is pinned to its first CPU.

The parent then pipes stdout, stderr, and, when placement succeeded, a sync pipe, and `fork`s.

Child, in order: read one byte from the sync pipe; `dup2` the log pipes onto stdout and stderr; `dup2` the listen socket onto fd 3 when present and `close_range` every fd above it; `widen_to_online` when placement exists, otherwise `sched_setaffinity` to that Linux CPU; `raise_nofile`; `exec_daemon`. `widen_to_online` sets the affinity to every CPU in `/sys/devices/system/cpu/online` except `mercury_linux_cpu`. `exec_daemon` calls `execv` when `env` is empty and `execve` otherwise, merging those names over the inherited environment. Arguments are empty, or `--listen-fd 3` when a listen socket was passed. Failure exits 127.

Parent, after the child is waiting on the sync pipe: `pin_pid` on the child, then `move_into_cgroup` via `cgroup.procs`, then write the byte `'1'`. The cgroup is in place before the child widens its mask. A failed pin, move, or write sends `SIGKILL`, `waitpid`, and `launch` returns empty.

`migrate_route` copies the live record under `daemons_mu`, calls `launch` with the same `exec_path`, listen socket, and `env`, then `wait_until_image` (`image_is` reads `/proc/<pid>/exe`) for up to 2 seconds. Failure calls `discard_launch`. Success calls `arm_daemon` for the new pid, then `SIGTERM` on the old pid, polling for 2 seconds before `SIGKILL`.

`cpus_route` parses a Linux cpuset with `parse_cpuset`, rejects a set that contains `mercury_linux_cpu`, and rejects a set `resource_groups_see` does not know. It writes the set through `ensure_daemon_cpuset`, `write_sysfs`, and `cpuset_applied`. On success it logs the least-loaded socket, node, L3, and core that contain one of those CPUs, from `least_loaded_touching`, formatted by `resource_choice`. `add_core` and `remove_core` write that same `cpuset.cpus` from the monitor thread. Those three functions do not take `daemons_mu`. The accept thread and the monitor thread can both write one daemon's cpuset.

### Watch

File: `src/daemons.cpp`.

`watch_daemon` calls `read_pipes`, which `poll`s stdout and stderr until both ends close, then `waitpid`. It publishes `exit_code` (`WEXITSTATUS`, or -1 when the process did not exit normally), the log, and `finished` from `now_ms`, under `daemons_mu`, and notifies `daemons_cv`. The publish is skipped when `arm_daemon` has already replaced this pid.

### Monitoring

Files: `src/daemons.cpp`, `src/counters.cpp`, `src/counters.hpp`.

`monitor_daemon` opens the cgroup ring first: `RingArchive` at `rings/<daemon_id>.mring` and a `Ring` named `task-clock`, `user`, `system`. `rings_dir` uses `MERCURY_RINGS` or `rings`. `archive_name` keeps letters, digits, `.`, `_`, and `-`.

The loop condition is `daemon_running`. Each pass calls `daemon_tasks`, which walks `descendants` (`child_pids` reads `/proc/<pid>/task/<pid>/children`) and `task_ids` (`/proc/<pid>/task`). Above `kMaxMonitoredThreads` (64), `busiest_threads` keeps the 64 tids with the largest `cpu_ticks` delta from `/proc/<tid>/stat`. An equal delta keeps the thread already open.

Threads that leave the keep-set are `flush`ed and removed with `forget_thread`. A new tid calls `open_counters` while fewer than `kFullCounterThreads` (16) live groups are open, and `open_clock` after that. `open_counters` opens the hardware group (`cycles`, `instructions`, `cache-references`, `cache-misses`, `branches`, `branch-misses`) and falls back to the software group (`task-clock`, `page-faults`, `context-switches`, `cpu-migrations`). It adds a separate task-clock fd when the group has none. `open_clock` opens task-clock alone. `CounterGroup::arm` resets and enables the group. The first 16 also get a per-tid `.mring`. One failed open sets `unmonitored` and stops opening for that tick, so a later `RemoveCore` is dropped.

The loop then sleeps `kSampleMs` (10). `CounterGroup::pull` reads the delta into a `Sample`. A failed pull flushes and drops that thread. `Ring::push` stores the sample. `read_cgroup_cpu` reads `cpu.stat`, and `usec_delta_ns` converts the `usage_usec`, `user_usec`, and `system_usec` gaps into the cgroup ring. That ring is not a `ThreadRing` and is not copied into the occupancy window.

On the way out, the loop flushes every live ring and the cgroup ring, `forget_thread`s them, and `mark_counters_done`.

`CounterGroup::sample` is the single-thread sampler. The monitor loop uses `arm` and `pull` directly.

### Rings

Files: `src/ring.cpp`, `src/ring.hpp`, `src/ring_archive.cpp`, `src/ring_archive.hpp`.

`Ring` holds `kRingSlots` samples (60 s at 10 ms). `Ring::mu_` covers `slots_`, `head_`, `size_`, `pushed_`, `archived_lo_`, and `archived_hi_`.

`push` writes one slot under `Ring::mu_`. When the ring is full and `head_` is on a third boundary, it copies that settled third, drops the lock, and calls `RingArchive::append`. `recent` copies the newest samples under the same lock; `monitor_daemon` asks for `kScaleWindow + 1` (11). `flush` copies every slot the archive has not committed, appends outside the lock, then sets `archived_lo_` and `archived_hi_` under the lock again.

`RingArchive::append` writes records, `fsync`s, advances `committed_`, writes the header count, and `fsync`s again. The archive object has no mutex. One monitor thread owns each ring, and it calls `append` only after releasing `Ring::mu_`.

### Resource groups

Files: `src/resources.cpp`, `src/resources.hpp`. Called from `main` in `src/mercury.cpp` and from `add_core`, `cpus_route`, and `scale_route` in `src/daemons.cpp`.

`init_resources` reads the host once and builds four nested partitions. Every online Linux CPU from `/sys/devices/system/cpu/online` belongs to one group at each level. `ResourceLevel` names the levels: `Socket`, `Node`, `L3`, and `Core`.

| Level | Sysfs | Group name |
| --- | --- | --- |
| Socket | `cpuN/topology/physical_package_id` | `socket.<id>` |
| NUMA node | `nodeN/cpulist` | `node.<id>` |
| L3 | cache index with `level == 3` and a type other than `Instruction`, `shared_cpu_list` | `l3.<n>` in order of the lowest CPU |
| Core | `cpuN/topology/thread_siblings_list`, or `core_cpus_list` when that file is empty | `core.<n>` in order of the lowest CPU |

`ensure_group` creates a group the first time a CPU names it and leaves the parent alone after that. A socket's children are its nodes, a node's children are its L3 domains, and an L3's children are its cores. `finish_groups` sorts each CPU list and sorts children by their lowest CPU. A missing L3 or sibling list makes that CPU its own group, so a host without those files still has a partition.

The argument to `init_resources` is the daemon pool as Linux CPU numbers. Those CPUs are allocatable. Every other online CPU stays in the groups so `/cpus` can see it, and `request_cpu` does not return it. The worker's own CPU is not in the pool.

Each group gets one `Ring` of `kRingSlots` and no archive file. The counter names are `user`, `nice`, `system`, `idle`, `iowait`, `irq`, `softirq`, and `busy`. `init_resources` detaches `sample_loop`. That thread reads `/proc/stat` through `read_proc_stat`, sleeps `kSampleMs`, and `sample_once` pushes the delta of each group's member CPUs. Deltas are nanoseconds, using the kernel tick from `sysconf(_SC_CLK_TCK)`. `busy` is user + nice + system + irq + softirq. The first read is the baseline and is not pushed.

`load_of` is the group's state. It sums `busy`, `idle`, and `iowait` over `Ring::recent(kScaleWindow)` and returns busy divided by that sum. No samples yet is load 0. `state_of` copies that into a `ResourceGroupState` with `level`, `name`, `linux_cpus`, `load`, and `samples`. `resource_states` returns one state per group. `least_loaded` returns the lowest load at one level. An equal load keeps the group whose lowest Linux CPU is smaller. `least_loaded_touching` is the same among groups that contain one of the given CPUs.

`resources_ready` is true once at least one group exists. `resource_groups_see` is true when every given Linux CPU is online and landed in a group.

`build_order` walks the tree with `step_cpu_walk` and records the first visit to each allocatable CPU. One step takes the next socket, then that socket's next node, then that node's next L3, then that L3's next core, then the next logical CPU on the core. `request_cpu` walks the recorded order under `mu`, skips any Linux CPU in `exclude`, and skips a CPU whose core group load from `core_load` is at least `kCoreFull` (0.90). `core_load` is `load_of` for the `Core` group that contains the CPU. No samples yet is load 0, so the CPU is offered. The CPU it stops on is returned even when another daemon already runs there. A full pass that offers nothing returns empty, and the cursor is back where the pass started. `mu` covers that cursor only. `core_load` takes `Ring::mu_` through `recent` while `mu` is held. `sample_loop` and `least_loaded` take `Ring::mu_` inside `push` and `recent`, and they do not take `mu`.

`scale_route` calls `resources_ready` and `least_loaded(Core)` before it attaches a policy. If either is empty it returns HTTP 500. After the attach it logs the least-loaded socket, node, L3, and core through `resource_choice`. Those numbers are the state of the groups. The CPU `add_core` actually appends is still the one `request_cpu` returns.

`add_core` passes the daemon's current cpuset as `exclude` and appends the CPU `request_cpu` returns. A daemon does not keep a private pool. The same Linux CPU can be added to a second daemon when that core's load is under `kCoreFull`. `remove_core` still drops the highest CPU in the cpuset and does not ask `request_cpu`.

### Scaling

Files: `src/policy.cpp`, `src/policy.hpp`, and the decision loop in `monitor_daemon`.

`scale_route` runs on the accept thread. `scale_int` reads `min` (default 1) and `max` (default `kDefaultMaxCores`, 16). `make_policy` returns a `SaturatePolicy` for `saturate` and empty for any other name. Under `daemons_mu` it requires `running`, stores the limits and the policy, and sets `added_at_ns` only on the first attach. The response counts cores with `cpuset_cores` and threads from `Daemon::threads`.

Each monitor pass, after the sample, copies `ScaleState` and the thread rings under `daemons_mu`. It builds an `OccupancyWindow` from `cpuset_cores` and from `Ring::recent`, keeping samples with `timestamp_ns` greater than `added_at_ns`. `SaturatePolicy::decide` calls `measure_age` once per sample in the window of `kScaleWindow` (10). `measure_age` reads `task-clock` through `named_counter`, divides by the gap from `span_before`, and caps each thread at 1. A thread is busy at or above `max(busiest / 8, 0.02)`. Occupancy is the sum of those fractions divided by `window.cores`.

`decide` returns `AddCore` when `cores < min_cores`. It returns `RemoveCore` when `cores > max_cores`. It returns `Hold` until the deepest thread has 11 samples. A thread shorter than the age being measured is skipped. A tick with no `task-clock` sample, or with no thread counted, returns `Hold` for the whole decision. Otherwise every one of the last 10 ticks must show occupancy at least 0.90 and more busy threads than cores to add, or fewer busy threads than cores and occupancy at or below `(cores - 1) / cores + 0.02` to remove. The add also requires `cores < max_cores`. The remove also requires `cores > min_cores`.

Back in `monitor_daemon`, `Hold` does nothing. `RemoveCore` does nothing on a tick where a counter failed to open. `AddCore` calls `add_core`. `RemoveCore` calls `remove_core`. `add_core` asks `request_cpu` for the Linux CPU to append, then writes `cpuset.cpus`. `remove_core` drops the highest CPU. Both poll `cpuset_applied`. After an add that happened while `cores` was already at least `min_cores`, the monitor sets `added_at_ns` under `daemons_mu`, so the next decision waits for a new window. A removal leaves `added_at_ns` unchanged.
