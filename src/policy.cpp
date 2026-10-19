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
