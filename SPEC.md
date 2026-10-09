# Mercury

Mercury places, starts, and deletes native programs on computers on one LAN. This system is for managing daemons. Each program is an executable compiled for that machine. Nothing runs in a container.

## Roles

The **controller** keeps the daemon table. The operator submits a set of `(exec_path, hostname)` tuples. That hostname is the node offering the file.

A **worker** runs on each computer. It listens on one TCP port, starts local processes, and reports when they exit.

Each node runs one process. Its argument `cpus` is an array of CPU numbers, the share of cores it may use. It pins itself to the lowest number in `cpus` for its whole life. The remaining numbers are divided round-robin over the other CPUs. Affinity for every CPU number here is that number minus one. On one node this process also holds the daemon table; on the others it is the worker. It is not a daemon. Round-robin does not place it, and delete does not stop it.

A worker registers with `{hostname, cpus}` when it connects. A tuple is schedulable when at least one registered worker reports that `exec_path` exists and is executable. Any other tuple is rejected and not stored.

## Daemon


| Field       | Meaning                           |
| ----------- | --------------------------------- |
| `daemon_id` | Assigned by the controller        |
| `exec_path` | Absolute path on the worker       |
| `hostname`  | Worker selected to run it         |
| `cpu`       | Index chosen by round-robin       |
| `state`     | `pending`, `running`, or `exited` |
| `exit_code` | Set when the process exits        |
| `log`       | That process's stdout and stderr  |


`pending` is placed but not started. `running` means the worker holds a live pid. `exited` means `exit_code` and `log` are stored for that `daemon_id`.

Delete sends kill if the process is running, then removes the record. A deleted daemon keeps no exit record.

## Messages

TCP. Each message is a 4-byte big-endian length and one UTF-8 JSON object.

Worker to controller:

- `register` — `{hostname, cpus}`
- `exited` — `{daemon_id, exit_code, log}`
- `bound` — `{daemon_id, pid, cpu}`
- `unbound` — `{daemon_id, pid}`

Controller to worker:

- `stat` — `{exec_path}` returns `{ok}`
- `start` — `{daemon_id, exec_path, cpu}` returns `{pid}`
- `kill` — `{daemon_id}` returns `{ok}`

On `start`, the worker pins the child to CPU `cpu - 1` with `sched_setaffinity`, then runs `exec_path` with no arguments. It reads stdout and stderr until the child exits, then sends `exited` once.

`pthread_create` starts a new kernel PID. A Go server does the same for each OS thread in its runtime. A goroutine is not a PID. The worker reads `/proc/<pid>/task` and treats every new PID as a controlled daemon: it pins that PID to the next free CPU in `daemon_cpus` and sends `bound`. When the PID exits it sends `unbound`. If no CPU in `daemon_cpus` is free, the new PID is pinned to the daemon's own `cpu`. The exit code and log stored for the daemon stay those of the process that was `exec`'d.

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
