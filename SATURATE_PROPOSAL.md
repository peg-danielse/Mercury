# Proposal: reliable saturate without per-thread rings

Status: proposal for review. `SPEC.md` is unchanged; `SATURATE_PROPOSAL.diff` is the unified diff that would apply it (`patch -p1 < SATURATE_PROPOSAL.diff` from `~/Mercury`). The SPEC.md this was drawn against is saved as `SPEC.md.bak-saturate`. The full proposed text of each changed section is in the diff; this file gives the rules, the thresholds, and why.

## Why the current rule fails

`SaturatePolicy::decide` in the working tree requires every one of the last 10 samples (100 ms) to show cgroup occupancy at least 0.90. Measured on dtn-01, a fully busy cgroup shows a 10 ms occupancy of 1.00 on average with a standard deviation of 0.15 to 0.18, and only 63 to 77 percent of 10 ms samples reach 0.90. Ten in a row then happens with probability about 0.7^10, roughly 3 percent per window, and less when samples are correlated. That is why a 6-thread spinner on one CPU did not grow and why `autoscale_test` and `autoscale_least_loaded` stop short. The cause is how `cpu.stat` is accounted (at scheduler ticks), not the workload.

## Rules

1. Input. One cgroup ring per daemon, sampled every 10 ms as today: `task-clock`, `user`, `system` from `cpu.stat`, plus a new `pressure` value, the gap of `cpu.pressure` `some total`. No perf fds and no per-thread rings. Occupancy is `task-clock / (gap * cores)`. Pressure is `pressure / gap`.
2. Window. `kScaleWindow` = 64 samples (640 ms). The decision uses the window mean (sum over the sum of the gaps), not every sample.
3. Add one CPU when mean occupancy >= 0.85 and mean pressure >= 0.10 and cores < max.
4. Remove one CPU when mean occupancy <= (cores - 1) / cores - 0.05 and mean pressure <= 0.02 and cores > min.
5. The immediate min and max rules are unchanged.
6. Cooldown. After any add or remove (and after `widen` or `shift`), the next decision waits for a fresh full window. This uses a single `changed_at_ns`, which replaces `added_at_ns`. Removal no longer sheds one CPU per sample.
7. Fallback when PSI is missing: count threads in state `R` from `/proc/<pid>/task/*/stat` once per tick and store that count as `runnable`. Add when the mean runnable count is >= cores + 0.5, remove when it is <= cores - 1.
8. Attach. `arm_daemon` attaches `saturate` with min 1 and max 16 to every daemon at start. `/scale` still updates the limits and the policy.

## Thresholds and why

| Constant | Value | Reason |
| --- | --- | --- |
| `kScaleWindow` | 64 | The 64-sample mean of a saturated cgroup stayed within 0.992 to 1.004 in every run, against 0.59 to 1.0 per 10 ms sample. 32 samples would roughly double the spread (about 0.03) and still clear 0.85. 64 is the margin-safe choice, and with one change per window a daemon goes from 1 to 16 CPUs in about 10 s. |
| `kAddOccupancy` | 0.85 | About 0.14 below the measured saturated mean (0.99 or more), which is about 30 times the spread of the window mean. A small amount of real idle time still allows growth when pressure says tasks are waiting. |
| `kAddPressure` | 0.10 | With idle or exactly-fitting threads (1 on 1, 4 on 4, 1 on 4), the measured pressure ratio was 0.000 to 0.004. With excess threads it was 0.50 (6 on 4) and 1.00 (6 on 1). 0.10 is 25 times the idle noise. It also means a daemon whose tasks wait more than 10 percent of the time gets a CPU, and one that waits less does not. |
| `kRemoveMargin` | 0.05 | The remove line is (cores - 1) / cores - 0.05. For example, 2 cores: 0.45; 4 cores: 0.70; 16 cores: 0.888. The spare CPU must be idle by a clear margin, far beyond the 64-sample noise, and the gap to the add line (0.85) leaves a dead band, so there is no flapping. |
| `kRemovePressure` | 0.02 | Five times the idle noise (0.004). A daemon whose tasks are still queuing keeps its CPUs even if occupancy dips. |
| `kRunnableAddMargin` | 0.5 | Only for the fallback. The mean runnable count was exactly the thread count for spinners (1.00, 4.00, 6.00), so half a thread above `cores` separates "fits" from "one too many". |

Expected settling: N busy threads started on 1 CPU. Each window with cores < N shows occupancy 1 and pressure > 0, so a CPU is added. At cores = N, pressure is about 0, so the daemon holds. It never removes, because occupancy is about 1. It settles at N after about N - 1 windows (about 0.65 s each, plus cpuset settle time).

## Measurements on dtn-01 (kernel 6.12.107+deb13-amd64)

PSI is available. `cpu.pressure` exists on `/sys/fs/cgroup/mercury` and on each daemon cgroup, for example `burn4/cpu.pressure`, with both `some` and `full` lines. I sampled a temporary cgroup `mercury/ptmeas` (removed afterwards) every 10 ms for 3 s after a 1.5 s warm-up, with `build/test/spinner` at 100 percent duty:

| cpuset | threads | 10 ms occ mean / sd / min | share of 10 ms >= 0.90 | 64-sample occ min..max | pressure ratio 10 ms mean | 64-sample pressure | avg10 | mean R threads |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 CPU (8) | 1 | 0.999 / 0.182 / 0.75 | 0.63 | 0.996..1.002 | 0.001 | 0.000..0.004 | 0.00 | 1.00 |
| 1 CPU (8) | 6 | 0.997 / 0.161 / 0.59 | 0.74 | 0.992..1.001 | 0.999 | 0.996..1.000 | 32.47 | 6.00 |
| 4 CPUs (8-9,40-41) | 1 | 0.250 / 0.046 / 0.19 | 0.00 | 0.249..0.250 | 0.001 | 0.000..0.004 | 0.00 | 1.00 |
| 4 CPUs | 4 | 0.999 / 0.165 / 0.71 | 0.72 | 0.995..1.004 | 0.000 | 0.000..0.001 | 0.00 | 4.00 |
| 4 CPUs | 6 | 0.998 / 0.152 / 0.71 | 0.77 | 0.995..1.001 | 0.500 | 0.500..0.501 | 16.17 | 6.00 |

`avg10` is a kernel 10 s exponential average. It lags far behind a 640 ms window (it read 32 percent while the actual ratio was 100 percent), so the proposal uses the `total` delta and not `avg10`.

## Notes and risks

- The archive format changes: `.mring` files gain a fourth counter (`pressure`, or `runnable`). Readers that assume three names need updating.
- `cpu.pressure` `some` also counts time a task waits because of `cpu.max` throttling. Mercury does not set `cpu.max`, so pressure here means waiting for the cpuset.
- Pressure measures CPU waiting, not the work itself. A daemon with many threads that block mostly on I/O but wake together can show short pressure spikes. The 64-sample mean and the 0.10 line absorb bursts shorter than about 64 ms per window.
- Hyperthreads. A CPU added on a sibling of a busy CPU raises occupancy accounting, but per-CPU throughput is lower. The rule is about scheduling capacity, not throughput, which matches today.
- Auto-attach changes behaviour for tests that start daemons and check their pins without `/scale`. For example, `daemon_test --place` "checks the pins", and `cpus_test` and `cpus_many_test` set cpusets explicitly. A busy daemon in those tests may now gain CPUs. Options: the tests post `/scale {max:1}` after start; or `/start` accepts an optional `scale` object. The diff specifies plain auto-attach as asked. Running those tests after implementation will show which need the `/scale {max:1}` line.
- `policies.md`: `widen` uses the same add test over the last 1 s (100 samples) instead of the `Threads:` count the current implementation approximates with. `shift` ranking stays on cgroup `task-clock` over 1 s. The "saturate always active" statement becomes literally true. No change to `policies.md` text is required beyond what the diff adds to SPEC.md, though its sentence "Occupancy is the same measurement as `measure_age`" should later say "the cgroup occupancy and pressure of `saturate`".

## Test approach

- `policy_test` gets `decide` cases built from synthetic windows (65 samples with given task-clock, pressure, and gaps) and from recorded `.mring` traces of the spinner runs above. The cases cover: hold below 65 samples, add at 6-on-1, hold at 4-on-4, remove at 1-on-4, no flapping across the dead band, min and max immediate, the fallback `runnable` path, and the fresh-window rule after a change.
- Acceptance on dtn-01: `autoscale_test` and `autoscale_least_loaded` settle at their expected cores. A new integration case starts `spinner` with N = 1, 3, 6 threads on one CPU and checks that it settles at N and holds for 10 windows. With N threads on 8 CPUs, it checks that the daemon shrinks to N.
