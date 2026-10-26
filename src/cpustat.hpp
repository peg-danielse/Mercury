#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Per logical CPU telemetry for interference-aware placement: one ring per online CPU, sampled every period_ms, archived to
// <dir>/cpu-<n>.mring, and <dir>/placement.jsonl with every daemon cpuset change, so each CPU sample can be joined
// to the daemons on that CPU at that moment. Off unless init_cpustat runs (mercury: MERCURY_CPUSTAT=1, period
// MERCURY_CPUSTAT_MS [50], sampler pinned to MERCURY_CPUSTAT_CPU if set). One sample of every CPU costs a few ms
// (/proc/schedstat alone is 1.5 ms on a 64-CPU EPYC), hence the coarser default than the 10 ms daemon rings.
// Each CPU's perf group is disabled and re-enabled before every read (MERCURY_CPUSTAT_KICK=0 skips it): on dtn-01
// (EPYC 7301, kernel 6.12) groups on CPUs running DSB services stayed "running" (time_running == time_enabled) but
// counted 0 cycles until something reprogrammed the PMU; the kick does that. It costs the sampler ~13% of its CPU.
//
// Columns: busy (ns tasks ran on this CPU), run_delay (ns tasks waited on its run queue) and slices (/proc/schedstat,
// exact), irq (/proc/stat irq+softirq, ns in clock ticks), cycles, instructions, llc_misses (perf on that CPU, pid -1;
// 0 when the PMU refuses), daemons (Mercury cpusets that contain the CPU).
inline const std::vector<std::string> kCpustatNames{"busy",   "irq",          "run_delay",  "slices",
                                                    "cycles", "instructions", "llc_misses", "daemons"};

// cpu: pin the sampling thread there; -1 leaves it on Mercury's CPU.
void init_cpustat(const std::string& dir, int period_ms, int cpu);

bool cpustat_enabled();

// One CPU over the samples newer than from_ns (CLOCK_MONOTONIC): busy and run_delay as shares of the span,
// IPC (instructions over cycles) and LLC misses per millisecond; perf is false when the PMU gave no cycles.
struct CpuWindow {
  int cpu = 0;
  double busy = 0;
  double wait = 0;
  double ipc = 0;
  double llc_per_ms = 0;
  bool perf = false;
};

// Every online CPU's window; empty when cpustat is off or no sample is new enough.
std::vector<CpuWindow> cpustat_window(std::int64_t from_ns);

// Record a daemon's cpuset; empty when the daemon is gone. Writes a placement line only when the set changed.
void note_cpuset(const std::string& daemon_id, const std::vector<int>& cpus);

// /proc/stat per CPU, in clock ticks.
struct ProcStatCpu {
  std::uint64_t busy = 0;
  std::uint64_t irq = 0;
};
std::unordered_map<int, ProcStatCpu> parse_proc_stat(std::string_view text);

// /proc/schedstat per CPU (version 15+): field 7 run time in ns, field 8 run_delay in ns, field 9 timeslices.
struct SchedstatCpu {
  std::uint64_t run_ns = 0;
  std::uint64_t run_delay_ns = 0;
  std::uint64_t slices = 0;
};
std::unordered_map<int, SchedstatCpu> parse_schedstat(std::string_view text);

// "0-3,32-35" -> 0 1 2 3 32 33 34 35; empty on a malformed list.
std::vector<int> parse_cpu_ranges(std::string_view text);
