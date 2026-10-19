#pragma once
// Per-thread signal reader for one cgroup (SPEC.md "gnn policy", phase 1).
// Each tick rediscovers the threads in <cgroup>/cgroup.threads, opens a software perf group
// (task-clock, context-switches, cpu-migrations) on every new tid, closes the group of every tid
// that left, and returns deltas since the previous tick. Only the previous values are kept.
// The cgroup files and each thread's /proc/<tid>/{stat,schedstat} stay open and are re-read with
// pread, so a thread costs 5 fds (3 perf + 2 proc).

#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct ThreadDelta {
  pid_t tid = 0;
  std::string comm;
  bool fresh = false;  // first seen this tick: deltas are 0, this tick is its baseline
  bool perf = false;   // perf group open (false: over the fd cap, or perf_event_open failed)
  std::uint64_t task_clock_ns = 0;
  std::uint64_t context_switches = 0;
  std::uint64_t cpu_migrations = 0;
  bool schedstat = false;
  std::uint64_t run_ns = 0;   // /proc/<tid>/schedstat field 1 delta
  std::uint64_t wait_ns = 0;  // field 2 delta (time runnable on a run queue)
  std::uint64_t timeslices = 0;
  int cpu = -1;   // /proc/<tid>/stat field 39 (last CPU)
  int node = -1;  // NUMA node of that CPU
};

struct CgroupDelta {
  bool cpu_ok = false;
  std::uint64_t usage_ns = 0;
  std::uint64_t user_ns = 0;
  std::uint64_t system_ns = 0;
  bool psi = false;
  std::uint64_t some_ns = 0;  // cpu.pressure "some total" delta
  std::uint64_t full_ns = 0;  // cpu.pressure "full total" delta (0 when absent)
  int cpuset_cpus = 0;        // cpuset.cpus.effective size
  std::string cpuset;
};

struct TickResult {
  std::int64_t timestamp_ns = 0;  // CLOCK_MONOTONIC at the read
  std::int64_t interval_ns = 0;   // since the previous tick (0 on the first)
  CgroupDelta cgroup;
  std::vector<ThreadDelta> threads;
  std::size_t exited = 0;      // tids dropped this tick
  std::size_t perf_open = 0;   // tids holding a perf group after this tick
  bool cgroup_gone = false;
  std::int64_t read_ns = 0;    // wall time this tick() spent reading (its own cost)
};

class ThreadStats {
 public:
  // cgroup_dir: e.g. /sys/fs/cgroup/mercury/<daemon_id>. max_perf_threads caps the perf groups
  // (3 fds each); further threads are read from schedstat only.
  explicit ThreadStats(std::string cgroup_dir, std::size_t max_perf_threads = 1024);
  ~ThreadStats();
  ThreadStats(const ThreadStats&) = delete;
  ThreadStats& operator=(const ThreadStats&) = delete;

  TickResult tick();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// CPU -> NUMA node from /sys/devices/system/node/node*/cpulist. -1 for unknown CPUs.
std::vector<int> cpu_node_map();
// Lifts the soft RLIMIT_NOFILE to the hard limit (65536 when unlimited).
void raise_nofile_limit();
