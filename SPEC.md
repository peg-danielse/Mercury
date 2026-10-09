# Mercury

Mercury places, starts, and deletes native programs on computers on one LAN. This system is for managing daemons. Each program is an executable compiled for that machine. Nothing runs in a container.

## Roles

The **controller** keeps the job table. The operator submits a set of `(exec_path, hostname)` tuples. That hostname is the node offering the file.

A **worker** runs on each computer. It listens on one TCP port, starts local processes, and reports when they exit.

Each node runs one process. Its argument `cpus` is an array of CPU numbers, the share of cores it may use. It pins itself to the lowest number in `cpus` for its whole life. The remaining numbers are divided round-robin over the other CPUs. Affinity for every CPU number here is that number minus one. On one node this process also holds the job table; on the others it is the worker. It is not a job. Round-robin does not place it, and delete does not stop it.

A worker registers with `{hostname, cpus}` when it connects. A tuple is schedulable when at least one registered worker reports that `exec_path` exists and is executable. Any other tuple is rejected and not stored.

## Job


| Field       | Meaning                           |
| ----------- | --------------------------------- |
| `job_id`    | Assigned by the controller        |
| `exec_path` | Absolute path on the worker       |
| `hostname`  | Worker selected to run it         |
| `cpu`       | Index chosen by round-robin       |
| `state`     | `pending`, `running`, or `exited` |
| `exit_code` | Set when the process exits        |
| `log`       | That process's stdout and stderr  |


`pending` is placed but not started. `running` means the worker holds a live pid. `exited` means `exit_code` and `log` are stored for that `job_id`.

Delete sends kill if the process is running, then removes the record. A deleted job keeps no exit record.

## Messages

TCP. Each message is a 4-byte big-endian length and one UTF-8 JSON object.

Worker to controller:

- `register` — `{hostname, cpus}`
- `exited` — `{job_id, exit_code, log}`
- `bound` — `{job_id, pid, cpu}`
- `unbound` — `{job_id, pid}`

Controller to worker:

- `stat` — `{exec_path}` returns `{ok}`
- `start` — `{job_id, exec_path, cpu}` returns `{pid}`
- `kill` — `{job_id}` returns `{ok}`

On `start`, the worker pins the child to CPU `cpu - 1` with `sched_setaffinity`, then runs `exec_path` with no arguments. It reads stdout and stderr until the child exits, then sends `exited` once.

`pthread_create` starts a new kernel PID. A Go server does the same for each OS thread in its runtime. A goroutine is not a PID. The worker reads `/proc/<pid>/task` and treats every new PID as a controlled job: it pins that PID to the next free CPU in `jobs` and sends `bound`. When the PID exits it sends `unbound`. If no CPU in `jobs` is free, the new PID is pinned to the job's own `cpu`. The exit code and log stored for the job stay those of the process that was `exec`'d.

## Round-robin

Workers are numbered from 1 in registration order. A cluster cursor `next_node` starts at 1. For each worker, `jobs` is `cpus` without its lowest number, sorted ascending. A cursor walks `jobs` from the first entry. A share of one CPU accepts no jobs.

Walk schedulable tuples in submission order. From `next_node`, take the next worker for which `stat` returns ok, store it as `hostname`, and set `next_node` to the worker after it, wrapping to 1. Then set `cpu` to the next entry of that worker's `jobs` and advance, wrapping to the first entry. Store the job as `pending`.

Activate starts each `pending` job whose cpu has no running PID. A job whose cpu is busy stays `pending`. When a PID leaves a cpu, by `exited` or `unbound`, the controller starts the oldest `pending` job already assigned to that cpu.

## Delivery

A job stays on the controller until activate. Delivery is a single `start` to the worker in `hostname`. Load balancing across nodes is round-robin, by `next_node`. The controller does not copy the binary. A worker is skipped when `stat` fails, so if only the offering node has the path, every job is delivered there.

## Actions

- **Submit** — place the given tuples as above.
- **Activate** — start placed jobs that have a free cpu.
- **Delete** — kill if running, then drop the job.

If a worker disconnects, each of its `running` jobs is stored as `exited` with `exit_code` -1 and the log `worker lost`. Those jobs are not restarted.