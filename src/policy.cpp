#include "policy.hpp"

#include <algorithm>

std::optional<WindowMeans> window_means(const std::vector<Sample>& cgroup, int cores, std::size_t count,
                                        bool runnable) {
  if (cores < 1 || count == 0 || cgroup.size() < count + 1) {
    return std::nullopt;
  }
  const std::size_t first = cgroup.size() - count;
  double used = 0;
  double pressure = 0;
  for (std::size_t i = first; i < cgroup.size(); ++i) {
    const auto& sample = cgroup[i];
    if (sample.timestamp_ns <= cgroup[i - 1].timestamp_ns || sample.nvalues <= kCgroupPressure) {
      return std::nullopt;
    }
    used += static_cast<double>(sample.value[kCgroupTaskClock]);
    pressure += static_cast<double>(sample.value[kCgroupPressure]);
  }
  const auto span = static_cast<double>(cgroup.back().timestamp_ns - cgroup[first - 1].timestamp_ns);
  WindowMeans means;
  means.occupancy = used / span / static_cast<double>(cores);
  means.pressure = runnable ? pressure / static_cast<double>(count) : pressure / span;
  return means;
}

bool saturate_add_test(const WindowMeans& means, int cores, bool runnable) {
  if (means.occupancy < kAddOccupancy) {
    return false;
  }
  return runnable ? means.pressure >= static_cast<double>(cores) + kRunnableAddMargin : means.pressure >= kAddPressure;
}

bool saturate_remove_test(const WindowMeans& means, int cores, bool runnable) {
  if (cores < 2) {
    return false;
  }
  const double fits = static_cast<double>(cores - 1) / static_cast<double>(cores);
  const double line = fits - kRemoveMargin;
  const bool idle_queue =
      runnable ? means.pressure <= static_cast<double>(cores - 1) : means.pressure <= kRemovePressure;
  // Margin rule: clearly under one core less, and nothing waits.
  if (means.occupancy <= line && idle_queue) {
    return true;
  }
  // Relaxed rule: with no run-queue pressure the cgroup fits on one core less
  // whenever its mean occupancy does, without the margin. Without it, N busy
  // threads that picked up an (N+1)th CPU (N/(N+1) > N/(N+1) - margin) would
  // never give it back.
  return means.occupancy <= fits && idle_queue;
}

Decision SaturatePolicy::decide(const OccupancyWindow& window) {
  if (window.cores < 1) {
    return Decision::Hold;
  }
  if (window.cores < window.min_cores) {
    return Decision::AddCore;
  }
  if (window.cores > window.max_cores) {
    return Decision::RemoveCore;
  }
  const auto means =
      window_means(window.cgroup, window.cores, static_cast<std::size_t>(kScaleWindow), window.runnable);
  if (!means) {
    return Decision::Hold;
  }
  if (window.cores < window.max_cores && saturate_add_test(*means, window.cores, window.runnable)) {
    return Decision::AddCore;
  }
  if (window.cores > window.min_cores && saturate_remove_test(*means, window.cores, window.runnable)) {
    return Decision::RemoveCore;
  }
  return Decision::Hold;
}

std::shared_ptr<Policy> make_policy(const std::string& name) {
  if (name == "saturate") {
    return std::make_shared<SaturatePolicy>();
  }
  if (name == "gnn") {
    return std::make_shared<GnnPolicy>();
  }
  return nullptr;
}

// ---- Policies beyond saturate (policies.md) ----

namespace {

// Same counter layout as the resource-group rings in resources.cpp.
constexpr int kRingIdle = 3;
constexpr int kRingIowait = 4;
constexpr int kRingBusy = 7;

double disabled_cost(const std::vector<DegradeComponent>& optional, const std::vector<std::string>& disabled) {
  double cost = 0;
  for (const auto& component : optional) {
    if (std::find(disabled.begin(), disabled.end(), component.name) != disabled.end()) {
      cost += component.cost;
    }
  }
  return cost;
}

}  // namespace

std::vector<std::optional<double>> interval_loads(const std::vector<Sample>& samples, std::int64_t end_ns, int count) {
  std::vector<std::optional<double>> loads;
  if (count <= 0) {
    return loads;
  }
  std::vector<std::uint64_t> busy(static_cast<std::size_t>(count), 0);
  std::vector<std::uint64_t> span(static_cast<std::size_t>(count), 0);
  std::vector<bool> seen(static_cast<std::size_t>(count), false);
  const std::int64_t start_ns = end_ns - kPolicyIntervalNs * count;
  for (const auto& sample : samples) {
    if (sample.timestamp_ns <= start_ns || sample.timestamp_ns > end_ns || sample.nvalues <= kRingBusy) {
      continue;
    }
    // Stretch k covers (start + k s, start + (k + 1) s].
    auto k = static_cast<int>((sample.timestamp_ns - start_ns - 1) / kPolicyIntervalNs);
    k = std::clamp(k, 0, count - 1);
    const auto index = static_cast<std::size_t>(k);
    seen[index] = true;
    busy[index] += sample.value[kRingBusy];
    span[index] += sample.value[kRingBusy] + sample.value[kRingIdle] + sample.value[kRingIowait];
  }
  for (int k = 0; k < count; ++k) {
    const auto index = static_cast<std::size_t>(k);
    if (!seen[index] || span[index] == 0) {
      loads.push_back(std::nullopt);
    } else {
      loads.push_back(static_cast<double>(busy[index]) / static_cast<double>(span[index]));
    }
  }
  return loads;
}

bool node_hot(const std::vector<std::optional<double>>& loads) {
  if (loads.size() < static_cast<std::size_t>(kPolicyIntervals)) {
    return false;
  }
  int present = 0;
  int hot = 0;
  for (std::size_t i = loads.size() - static_cast<std::size_t>(kPolicyIntervals); i < loads.size(); ++i) {
    if (!loads[i]) {
      continue;
    }
    ++present;
    if (*loads[i] > kHotLoad) {
      ++hot;
    }
  }
  return present >= kPolicyIntervals && hot >= kHotIntervals;
}

std::optional<std::size_t> least_allocated(const std::vector<GroupCandidate>& legal, const std::vector<int>& used_cpus) {
  std::vector<int> used = used_cpus;
  std::sort(used.begin(), used.end());
  std::optional<std::size_t> best;
  double best_score = 0;
  for (std::size_t i = 0; i < legal.size(); ++i) {
    const auto& group = legal[i];
    double score = 0;
    if (!group.allocatable.empty()) {
      int free = 0;
      for (const int cpu : group.allocatable) {
        if (!std::binary_search(used.begin(), used.end(), cpu)) {
          ++free;
        }
      }
      score = static_cast<double>(free) / static_cast<double>(group.allocatable.size());
    }
    if (!best) {
      best = i;
      best_score = score;
      continue;
    }
    const auto& current = legal[*best];
    bool better = false;
    if (score != best_score) {
      better = score > best_score;
    } else if (group.load != current.load) {
      better = group.load < current.load;
    } else {
      better = group.lowest_cpu < current.lowest_cpu;
    }
    if (better) {
      best = i;
      best_score = score;
    }
  }
  return best;
}

std::vector<std::string> degrade_set(const std::vector<DegradeComponent>& optional,
                                     const std::vector<std::string>& disabled,
                                     const std::vector<std::optional<double>>& loads) {
  // Keep only names that are still optional components.
  std::vector<std::string> current;
  for (const auto& component : optional) {
    if (std::find(disabled.begin(), disabled.end(), component.name) != disabled.end()) {
      current.push_back(component.name);
    }
  }
  if (loads.empty() || !loads.back()) {
    return current;
  }
  const double latest = *loads.back();
  const double off = disabled_cost(optional, current);
  if (latest - off > kBrownoutLoad) {
    const DegradeComponent* pick = nullptr;
    for (const auto& component : optional) {
      if (std::find(current.begin(), current.end(), component.name) != current.end()) {
        continue;
      }
      if (pick == nullptr || component.cost > pick->cost) {
        pick = &component;
      }
    }
    if (pick != nullptr) {
      current.push_back(pick->name);
    }
    return current;
  }
  if (current.empty() || loads.size() < static_cast<std::size_t>(kPolicyIntervals)) {
    return current;
  }
  const DegradeComponent* back = nullptr;
  for (const auto& component : optional) {
    if (std::find(current.begin(), current.end(), component.name) == current.end()) {
      continue;
    }
    if (back == nullptr || component.cost < back->cost) {
      back = &component;
    }
  }
  if (back == nullptr) {
    return current;
  }
  const double off_after = off - back->cost;
  for (std::size_t i = loads.size() - static_cast<std::size_t>(kPolicyIntervals); i < loads.size(); ++i) {
    if (!loads[i] || *loads[i] - off_after > kBrownoutLoad) {
      return current;
    }
  }
  current.erase(std::find(current.begin(), current.end(), back->name));
  return current;
}
