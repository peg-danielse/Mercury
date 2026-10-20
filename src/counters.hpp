#pragma once

#include "ring.hpp"

#include <memory>
#include <string>
#include <sys/types.h>

class CounterGroup {
 public:
  CounterGroup();
  ~CounterGroup();
  CounterGroup(CounterGroup&&) noexcept;
  CounterGroup& operator=(CounterGroup&&) noexcept;
  CounterGroup(const CounterGroup&) = delete;
  CounterGroup& operator=(const CounterGroup&) = delete;

  bool empty() const;
  std::vector<std::string> names() const;
  // Reset and enable. The next pull is the delta since this call.
  void arm();
  // Disable and re-enable the group without resetting it (forces the kernel to reprogram the PMU).
  void kick();
  // One delta since arm or the previous pull. False when the thread is gone.
  bool pull(Sample& sample);

 private:
  struct Impl;
  explicit CounterGroup(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;

  friend CounterGroup open_software(pid_t pid);
  friend CounterGroup open_cpu_hardware(int cpu);
};

// task-clock, context-switches, cpu-migrations as one software group read with PERF_FORMAT_GROUP
// (threadstat). names() lists the events that opened, in that order.
CounterGroup open_software(pid_t pid);

// cycles, instructions, llc_misses for everything that runs on one CPU (pid -1), as one hardware group.
// Needs root or perf_event_paranoid <= 0. names() lists the events that opened; throws when none did.
CounterGroup open_cpu_hardware(int cpu);
