#include "policy.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>

namespace {

constexpr double kOccupancyHigh = 0.90;
constexpr std::uint64_t kNominalSpanNs = static_cast<std::uint64_t>(kSampleMs) * 1000000ULL;

std::uint64_t span_before(const ThreadSamples& thread, std::size_t index) {
  if (index == 0) {
    return kNominalSpanNs;
  }
  const auto delta = thread.samples[index].timestamp_ns - thread.samples[index - 1].timestamp_ns;
  if (delta <= 0) {
    return kNominalSpanNs;
  }
  return static_cast<std::uint64_t>(delta);
}

std::optional<std::uint64_t> named_counter(const ThreadSamples& thread, const Sample& sample, const char* name) {
  const int limit = std::min(sample.nvalues, static_cast<int>(thread.names.size()));
  for (int i = 0; i < limit; ++i) {
    if (thread.names[static_cast<std::size_t>(i)] == name) {
      return sample.value[i];
    }
  }
  return std::nullopt;
}

struct Tick {
  int busy = 0;
  double occupancy = 0;
  bool counted = false;
};

Tick measure_age(const OccupancyWindow& window, int age) {
  Tick tick;
  double util_sum = 0;
  double busiest = 0;
  std::vector<double> used;
  used.reserve(window.threads.size());
  for (const auto& thread : window.threads) {
    if (thread.samples.size() <= static_cast<std::size_t>(age) + 1) {
      continue;
    }
    const auto index = thread.samples.size() - 1 - static_cast<std::size_t>(age);
    const auto clock = named_counter(thread, thread.samples[index], "task-clock");
    if (!clock) {
      return tick;
    }
    const auto span = span_before(thread, index);
    // Each thread's own gap. A stalled sample must not raise the cut for the others.
    double util = static_cast<double>(*clock) / static_cast<double>(span);
    if (util > 1) {
      util = 1;
    }
    util_sum += util;
    busiest = std::max(busiest, util);
    used.push_back(util);
  }
  if (used.empty()) {
    return tick;
  }
  // 2% of the interval, same floor as span/50, compared per thread.
  const double cut = std::max(busiest / 8, 0.02);
  tick.occupancy = util_sum / static_cast<double>(window.cores);
  for (const double util : used) {
    if (util >= cut) {
      ++tick.busy;
    }
  }
  tick.counted = true;
  return tick;
}

}  // namespace

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

  std::size_t depth = 0;
  for (const auto& thread : window.threads) {
    depth = std::max(depth, thread.samples.size());
  }
  // One extra sample so every monitored slot has a real gap before it.
  if (depth < static_cast<std::size_t>(kScaleWindow) + 1) {
    return Decision::Hold;
  }

  bool scale_up = true;
  bool scale_down = true;
  const double idle_line = static_cast<double>(window.cores - 1) / static_cast<double>(window.cores) + 0.02;
  for (int age = 0; age < kScaleWindow; ++age) {
    const auto tick = measure_age(window, age);
    if (!tick.counted) {
      return Decision::Hold;
    }
    // One busy thread on one core is full and still does not need another core.
    // Several busy threads squeezed onto fewer cores do.
    if (tick.occupancy < kOccupancyHigh || tick.busy <= window.cores) {
      scale_up = false;
    }
    // Fewer busy threads than cores, and that spare core shows up in the occupancy.
    if (tick.busy >= window.cores || tick.occupancy > idle_line) {
      scale_down = false;
    }
  }
  if (scale_up && window.cores < window.max_cores) {
    return Decision::AddCore;
  }
  if (scale_down && window.cores > window.min_cores) {
    return Decision::RemoveCore;
  }
  return Decision::Hold;
}

std::shared_ptr<Policy> make_policy(const std::string& name) {
  if (name == "saturate") {
    return std::make_shared<SaturatePolicy>();
  }
  return nullptr;
}
