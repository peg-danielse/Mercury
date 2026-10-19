#include "policy.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
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

namespace {

std::optional<std::size_t> counter_index(const std::vector<std::string>& names, const char* name) {
  for (std::size_t i = 0; i < names.size(); ++i) {
    if (names[i] == name) {
      return i;
    }
  }
  return std::nullopt;
}

bool all_present(const IntervalLoads& loads) {
  if (loads.size() < static_cast<std::size_t>(kPolicyIntervals)) {
    return false;
  }
  for (const auto& load : loads) {
    if (!load) {
      return false;
    }
  }
  return true;
}

}  // namespace

IntervalLoads interval_loads(const std::vector<std::string>& names, const std::vector<Sample>& samples,
                             std::int64_t end_ns, int count) {
  IntervalLoads loads(static_cast<std::size_t>(std::max(count, 0)));
  const auto busy_at = counter_index(names, "busy");
  const auto idle_at = counter_index(names, "idle");
  const auto iowait_at = counter_index(names, "iowait");
  if (!busy_at || !idle_at || count <= 0) {
    return loads;
  }
  std::vector<double> busy(loads.size(), 0);
  std::vector<double> total(loads.size(), 0);
  std::vector<bool> seen(loads.size(), false);
  const std::int64_t start_ns = end_ns - static_cast<std::int64_t>(count) * kIntervalNs;
  for (const auto& sample : samples) {
    if (sample.timestamp_ns <= start_ns || sample.timestamp_ns > end_ns) {
      continue;
    }
    // (start, start + 1 s] is interval 0, the oldest.
    auto slot = static_cast<std::size_t>((sample.timestamp_ns - start_ns - 1) / kIntervalNs);
    if (slot >= loads.size()) {
      slot = loads.size() - 1;
    }
    auto value = [&](std::size_t index) -> double {
      if (static_cast<int>(index) >= sample.nvalues) {
        return 0;
      }
      return static_cast<double>(sample.value[index]);
    };
    const double b = value(*busy_at);
    const double t = b + value(*idle_at) + (iowait_at ? value(*iowait_at) : 0);
    busy[slot] += b;
    total[slot] += t;
    seen[slot] = true;
  }
  for (std::size_t i = 0; i < loads.size(); ++i) {
    if (!seen[i]) {
      continue;
    }
    loads[i] = total[i] > 0 ? busy[i] / total[i] : 0.0;
  }
  return loads;
}

bool node_hot(const IntervalLoads& loads, double util, int hits) {
  if (!all_present(loads)) {
    return false;
  }
  int hot = 0;
  for (std::size_t i = loads.size() - static_cast<std::size_t>(kPolicyIntervals); i < loads.size(); ++i) {
    if (*loads[i] > util) {
      ++hot;
    }
  }
  return hot >= hits;
}

bool node_overloaded(const IntervalLoads& loads, double theta) {
  if (!all_present(loads)) {
    return false;
  }
  double sum = 0;
  for (std::size_t i = loads.size() - static_cast<std::size_t>(kPolicyIntervals); i < loads.size(); ++i) {
    sum += *loads[i];
  }
  return sum / kPolicyIntervals > theta;
}

bool shift_triggered(const IntervalLoads& loads, const ShiftRule& rule) {
  switch (rule.trigger) {
    case ShiftTrigger::Hotspot:
      return node_hot(loads, rule.hot_util);
    case ShiftTrigger::Mean:
      return node_overloaded(loads, rule.mean_theta);
    case ShiftTrigger::Either:
      return node_hot(loads, rule.hot_util) || node_overloaded(loads, rule.mean_theta);
  }
  return false;
}

std::optional<std::string> shift_destination(const std::string& source, const std::vector<NodeCandidate>& nodes,
                                             int need_cpus) {
  const NodeCandidate* best = nullptr;
  for (const auto& node : nodes) {
    if (node.name == source || node.hot || !node.inside_ceiling || node.allocatable < need_cpus) {
      continue;
    }
    if (best == nullptr || node.load < best->load ||
        (node.load == best->load && node.lowest_cpu < best->lowest_cpu)) {
      best = &node;
    }
  }
  if (best == nullptr) {
    return std::nullopt;
  }
  return best->name;
}

std::optional<std::string> least_allocated(const std::vector<GroupCandidate>& groups) {
  const GroupCandidate* best = nullptr;
  double best_score = -1;
  for (const auto& group : groups) {
    const double score =
        group.allocatable > 0 ? static_cast<double>(group.unallocated) / static_cast<double>(group.allocatable) : 0.0;
    if (best == nullptr || score > best_score ||
        (score == best_score &&
         (group.load < best->load || (group.load == best->load && group.lowest_cpu < best->lowest_cpu)))) {
      best = &group;
      best_score = score;
    }
  }
  if (best == nullptr) {
    return std::nullopt;
  }
  return best->name;
}

std::optional<std::size_t> degrade_step(std::vector<DegradeComponent>& components, const IntervalLoads& loads,
                                        double limit) {
  if (loads.empty() || !loads.back()) {
    return std::nullopt;
  }
  double disabled_cost = 0;
  for (const auto& component : components) {
    if (!component.enabled) {
      disabled_cost += component.cost;
    }
  }
  const double latest = *loads.back();
  if (latest - disabled_cost > limit) {
    std::optional<std::size_t> pick;
    for (std::size_t i = 0; i < components.size(); ++i) {
      if (components[i].enabled && (!pick || components[i].cost > components[*pick].cost)) {
        pick = i;
      }
    }
    if (pick) {
      components[*pick].enabled = false;
    }
    return pick;
  }
  std::optional<std::size_t> pick;
  for (std::size_t i = 0; i < components.size(); ++i) {
    if (!components[i].enabled && (!pick || components[i].cost < components[*pick].cost)) {
      pick = i;
    }
  }
  if (!pick || !all_present(loads)) {
    return std::nullopt;
  }
  const double after = disabled_cost - components[*pick].cost;
  for (const auto& load : loads) {
    if (*load - after > limit) {
      return std::nullopt;
    }
  }
  components[*pick].enabled = true;
  return pick;
}

std::optional<std::size_t> tune_step(std::vector<TuneParameter>& parameters, const IntervalLoads& loads,
                                     bool degraded, double limit) {
  if (loads.empty() || !loads.back()) {
    return std::nullopt;
  }
  if (*loads.back() > limit || degraded) {
    for (std::size_t i = 0; i < parameters.size(); ++i) {
      if (parameters[i].index > 0) {
        --parameters[i].index;
        return i;
      }
    }
    return std::nullopt;
  }
  if (!all_present(loads)) {
    return std::nullopt;
  }
  for (const auto& load : loads) {
    if (*load > limit) {
      return std::nullopt;
    }
  }
  for (std::size_t i = 0; i < parameters.size(); ++i) {
    if (parameters[i].index + 1 < parameters[i].values.size()) {
      ++parameters[i].index;
      return i;
    }
  }
  return std::nullopt;
}
