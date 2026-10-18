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
// Cgroup ring layout: task-clock, user, system, then pressure (or runnable in the fallback).
inline constexpr int kCgroupTaskClock = 0;
inline constexpr int kCgroupPressure = 3;

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

// ---- Policies beyond saturate (policies.md). Pure decisions, no I/O. ----

#include <cstdint>
#include <optional>

// Every policy rule counts in five 1 s intervals taken from the resource-group rings.
inline constexpr int kPolicyIntervals = 5;
inline constexpr std::int64_t kPolicyIntervalNs = 1000000000LL;
inline constexpr double kHotLoad = 0.75;
inline constexpr int kHotIntervals = 3;
inline constexpr double kBrownoutLoad = 0.8;

// Interval load of a resource-group ring: busy / (busy + idle + iowait) over the samples whose
// timestamp falls in each of the last count 1 s stretches ending at end_ns, oldest first.
// A stretch with no samples (or no time) is nullopt, a missing interval.
std::vector<std::optional<double>> interval_loads(const std::vector<Sample>& samples, std::int64_t end_ns,
                                                  int count = kPolicyIntervals);

// True when at least three of the last five interval loads are above 0.75. Fewer than five
// present intervals means the node is cold.
bool node_hot(const std::vector<std::optional<double>>& loads);

// One legal group for widen. allocatable are its allocatable Linux CPUs.
struct GroupCandidate {
  std::string name;
  std::vector<int> allocatable;
  double load = 0;
  int lowest_cpu = 0;
};

// Index of the group with the highest share of allocatable CPUs that appear in no running
// daemon cpuset (used_cpus). No allocatable CPU scores 0. Equal score keeps the lower load,
// then the lower Linux CPU. Empty when there is no candidate.
std::optional<std::size_t> least_allocated(const std::vector<GroupCandidate>& legal,
                                           const std::vector<int>& used_cpus);

struct DegradeComponent {
  std::string name;
  double cost = 0;
};

// Brownout step. loads are the last interval loads U of the daemon's group, oldest first
// (up to five). disabled is the current disabled set. Returns the new disabled set, which differs
// from disabled by at most one component:
//  - when latest U minus the cost of the disabled set is above 0.8, the enabled component with
//    the highest cost is disabled (equal cost keeps list order);
//  - otherwise the disabled component with the smallest cost is enabled again when all five
//    intervals satisfy U - (disabled cost - its cost) <= 0.8.
std::vector<std::string> degrade_set(const std::vector<DegradeComponent>& optional,
                                     const std::vector<std::string>& disabled,
                                     const std::vector<std::optional<double>>& loads);
