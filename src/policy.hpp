#pragma once

#include "ring.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// saturate decides on the mean of the last kScaleWindow cgroup samples (640 ms at 10 ms).
inline constexpr int kScaleWindow = 64;
inline constexpr int kDefaultMaxCores = 16;
inline constexpr double kAddOccupancy = 0.85;
inline constexpr double kAddPressure = 0.10;
inline constexpr double kRemoveMargin = 0.05;
inline constexpr double kRemovePressure = 0.02;
inline constexpr double kRunnableAddMargin = 0.5;
// Cgroup ring layout: task-clock, user, system, then pressure (or runnable in the fallback), then
// cores (the cpuset size at that sample).
inline constexpr int kCgroupTaskClock = 0;
inline constexpr int kCgroupPressure = 3;
inline constexpr int kCgroupCores = 4;

enum class Decision { Hold, AddCore, RemoveCore };

// cores is the cpuset the daemon has now. min_cores and max_cores bound the policy.
// cgroup is the daemon cgroup ring, oldest first, only samples newer than the last cpuset change.
// value[0] is task-clock in nanoseconds for that gap. value[3] is the cpu.pressure "some" gap in
// nanoseconds, or, when runnable is true, the count of threads in state R at that tick.
struct OccupancyWindow {
  int cores = 0;
  int min_cores = 1;
  int max_cores = kDefaultMaxCores;
  bool runnable = false;
  std::vector<Sample> cgroup;
};

// Means over the newest count gaps of a cgroup series (count + 1 samples). occupancy is
// task-clock over the summed gaps and cores; pressure is the pressure gap over the summed gaps,
// or the mean runnable count in the fallback. Empty when there are not enough samples, a gap is
// not positive, or a sample lacks the pressure value.
struct WindowMeans {
  double occupancy = 0;
  double pressure = 0;
};
std::optional<WindowMeans> window_means(const std::vector<Sample>& cgroup, int cores, std::size_t count,
                                        bool runnable);

// The add and remove tests of saturate, without the min/max limits.
bool saturate_add_test(const WindowMeans& means, int cores, bool runnable);
bool saturate_remove_test(const WindowMeans& means, int cores, bool runnable);

class Policy {
 public:
  virtual ~Policy() = default;
  virtual Decision decide(const OccupancyWindow& window) = 0;
};

// Add a core when the window mean shows the cpuset full (occupancy >= 0.85) and tasks waiting for a
// CPU (pressure >= 0.10). Remove one when a CPU is clearly spare and nothing waits.
class SaturatePolicy : public Policy {
 public:
  Decision decide(const OccupancyWindow& window) override;
};

// gnn (SPEC.md "gnn policy"): the per-thread controller in src/gnn.cpp runs in monitor_daemon instead of
// decide(); decide() only holds. make_policy returns it for "gnn".
class GnnPolicy : public Policy {
 public:
  Decision decide(const OccupancyWindow&) override { return Decision::Hold; }
};

std::shared_ptr<Policy> make_policy(const std::string& name);
