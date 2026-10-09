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
  void sample(const std::string& daemon_id, pid_t pid, const std::shared_ptr<Ring>& ring,
              bool (*running)(const std::string&, pid_t));

 private:
  struct Impl;
  explicit CounterGroup(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;

  friend CounterGroup open_counters(pid_t pid);
};

CounterGroup open_counters(pid_t pid);
