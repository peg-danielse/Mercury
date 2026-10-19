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
  // One delta since arm or the previous pull. False when the thread is gone.
  bool pull(Sample& sample);
  void sample(const std::string& daemon_id, pid_t pid, const std::shared_ptr<Ring>& ring,
              bool (*running)(const std::string&, pid_t));

 private:
  struct Impl;
  explicit CounterGroup(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;

  friend CounterGroup open_counters(pid_t pid);
  friend CounterGroup open_clock(pid_t pid);
  friend CounterGroup open_software(pid_t pid);
};

CounterGroup open_counters(pid_t pid);
// task-clock alone. One perf fd, for threads past the full-counter budget.
CounterGroup open_clock(pid_t pid);
// task-clock, context-switches, cpu-migrations as one software group read with PERF_FORMAT_GROUP
// (threadstat). names() lists the events that opened, in that order.
CounterGroup open_software(pid_t pid);
