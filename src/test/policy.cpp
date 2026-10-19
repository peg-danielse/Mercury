// Unit tests for the pure policy decisions in src/policy.cpp (policies.md):
// interval_loads, node_hot, least_allocated, and degrade_set.
#include "policy.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const std::string& message) {
  if (!ok) {
    ++failures;
    std::cerr << "FAIL " << message << std::endl;
  }
}

using Loads = std::vector<std::optional<double>>;

Sample ring_sample(std::int64_t ts, std::uint64_t busy, std::uint64_t idle, std::uint64_t iowait = 0) {
  Sample s;
  s.timestamp_ns = ts;
  s.nvalues = 8;
  s.value[3] = idle;
  s.value[4] = iowait;
  s.value[7] = busy;
  return s;
}

void test_interval_loads() {
  const std::int64_t end = 100 * kPolicyIntervalNs;
  std::vector<Sample> samples;
  // Interval 0 (oldest) has no samples. Interval k = 1..4 has load k/10 using 100 samples each.
  for (int k = 1; k < kPolicyIntervals; ++k) {
    const std::int64_t base = end - kPolicyIntervalNs * (kPolicyIntervals - k);
    for (int i = 1; i <= 100; ++i) {
      samples.push_back(ring_sample(base + i * 10000000LL, static_cast<std::uint64_t>(k), 10 - static_cast<std::uint64_t>(k)));
    }
  }
  // Outside the five intervals: ignored.
  samples.push_back(ring_sample(end - kPolicyIntervalNs * 6, 10, 0));
  samples.push_back(ring_sample(end + 1, 10, 0));
  const auto loads = interval_loads(samples, end);
  check(loads.size() == 5, "interval_loads returns five intervals");
  check(!loads[0], "empty stretch is a missing interval");
  for (int k = 1; k < 5; ++k) {
    check(loads[static_cast<std::size_t>(k)] && std::abs(*loads[static_cast<std::size_t>(k)] - k / 10.0) < 1e-9,
          "interval " + std::to_string(k) + " load");
  }
  // iowait counts in the denominator, the same ratio as load_of.
  const auto io = interval_loads({ring_sample(end - 1, 1, 1, 2)}, end, 1);
  check(io.size() == 1 && io[0] && std::abs(*io[0] - 0.25) < 1e-9, "iowait is in the denominator");
}

void test_node_hot() {
  check(node_hot(Loads{0.8, 0.9, 0.76, 0.1, 0.2}), "three of five above 0.75 is hot");
  check(!node_hot(Loads{0.8, 0.9, 0.75, 0.1, 0.2}), "exactly 0.75 is not above 0.75");
  check(!node_hot(Loads{0.8, 0.9, 0.1, 0.1, 0.2}), "two of five is cold");
  check(!node_hot(Loads{0.9, 0.9, 0.9, 0.9}), "fewer than five intervals is cold");
  check(!node_hot(Loads{0.9, std::nullopt, 0.9, 0.9, 0.9}), "a missing interval makes fewer than five: cold");
  check(!node_hot(Loads{0.9, 0.9, 0.9, 0.1, 0.1, 0.1}), "only the last five count");
  check(node_hot(Loads{0.1, 0.1, 0.9, 0.9, 0.9}), "last five hot");
  check(node_hot(Loads{1.0, 1.0, 1.0, 1.0, 1.0}), "full load is hot");
}

void test_least_allocated() {
  std::vector<GroupCandidate> legal{
      {"core.1", {2, 34}, 0.1, 2},
      {"core.2", {3, 35}, 0.0, 3},
      {"core.3", {4, 36}, 0.5, 4},
  };
  // core.2 is half used, core.3 is free, core.1 is fully used.
  auto pick = least_allocated(legal, {2, 34, 3});
  check(pick && legal[*pick].name == "core.3", "highest unallocated share wins over lower load");
  // Equal score: lower load wins.
  pick = least_allocated(legal, {});
  check(pick && legal[*pick].name == "core.2", "equal score keeps the lower load");
  // Equal score and load: lower Linux CPU wins.
  std::vector<GroupCandidate> tied{{"core.5", {10, 42}, 0.2, 10}, {"core.4", {8, 40}, 0.2, 8}};
  pick = least_allocated(tied, {});
  check(pick && tied[*pick].name == "core.4", "equal score and load keeps the lower cpu");
  // A group with no allocatable CPU scores 0.
  std::vector<GroupCandidate> zero{{"core.6", {}, 0.0, 12}, {"core.7", {14, 46}, 0.9, 14}};
  pick = least_allocated(zero, {14});
  check(pick && zero[*pick].name == "core.7", "half free beats no allocatable cpu");
  pick = least_allocated(zero, {14, 46});
  check(pick && zero[*pick].name == "core.6", "score 0 ties fall to lower load");
  check(!least_allocated({}, {}), "no legal group is empty");
}

void test_degrade_set() {
  const std::vector<DegradeComponent> parts{{"trace", 0.3}, {"metrics", 0.1}, {"thumbnails", 0.2}};
  // U = 1.0: disable the highest cost first, one per call, until U - disabled <= 0.8.
  auto off = degrade_set(parts, {}, Loads{1.0, 1.0, 1.0, 1.0, 1.0});
  check(off == std::vector<std::string>{"trace"}, "highest cost is disabled first");
  auto same = degrade_set(parts, off, Loads{1.0, 1.0, 1.0, 1.0, 1.0});
  check(same == std::vector<std::string>{"trace"}, "1.0 - 0.3 <= 0.8: nothing more is disabled");
  const std::vector<DegradeComponent> small{{"a", 0.15}, {"b", 0.1}, {"c", 0.05}};
  off = degrade_set(small, {}, Loads{1.0});
  off = degrade_set(small, off, Loads{1.0});
  check(off == std::vector<std::string>({"a", "b"}), "descending cost: a then b");
  off = degrade_set(small, off, Loads{1.0});
  check(off == std::vector<std::string>({"a", "b"}), "stops once U minus the disabled cost is at or below 0.8");
  // U = 0.85: one 0.1 component is not enough? 0.85 - 0 > 0.8 -> disable trace (0.3) -> 0.55.
  off = degrade_set(parts, {}, Loads{0.85});
  check(off == std::vector<std::string>{"trace"}, "U just above 0.8 disables the highest cost");
  off = degrade_set(parts, off, Loads{0.85});
  check(off == std::vector<std::string>{"trace"}, "U - cost below line holds with only one interval");
  // Re-enable: smallest cost first, only when all five intervals fit with its cost added back.
  std::vector<std::string> both{"trace", "metrics"};
  auto back = degrade_set(parts, both, Loads{0.85, 0.85, 0.85, 0.85, 0.85});
  // disabled cost 0.4: 0.85-0.4=0.45. Enable metrics (0.1): 0.85-0.3=0.55 <=0.8 for all five.
  check(back == std::vector<std::string>{"trace"}, "smallest cost is enabled first");
  back = degrade_set(parts, back, Loads{0.85, 0.85, 0.85, 0.85, 0.85});
  // trace back: 0.85 - 0 > 0.8 -> stays disabled.
  check(back == std::vector<std::string>{"trace"}, "a component stays off when its cost would push U over");
  back = degrade_set(parts, back, Loads{0.7, 0.7, 0.9, 0.7, 0.7});
  check(back == std::vector<std::string>{"trace"}, "every one of the five intervals must fit");
  back = degrade_set(parts, back, Loads{0.7, 0.7, 0.7, 0.7, 0.7});
  check(back.empty(), "all five fit: re-enabled");
  back = degrade_set(parts, {"trace"}, Loads{0.1, 0.1, 0.1, 0.1});
  check(back == std::vector<std::string>{"trace"}, "fewer than five intervals never re-enables");
  back = degrade_set(parts, {"trace"}, Loads{0.1, std::nullopt, 0.1, 0.1, 0.1});
  check(back == std::vector<std::string>{"trace"}, "a missing interval never re-enables");
  // Unknown names in the disabled set are dropped; no load means hold.
  back = degrade_set(parts, {"gone"}, Loads{});
  check(back.empty(), "names that are not optional components are dropped");
  // Every optional part off and still overloaded: nothing left to disable, nothing re-enabled.
  const std::vector<DegradeComponent> tiny{{"x", 0.05}};
  back = degrade_set(tiny, {"x"}, Loads{1.0, 1.0, 1.0, 1.0, 1.0});
  check(back == std::vector<std::string>{"x"}, "every optional part off stays off under overload");
  // Equal cost keeps list order.
  const std::vector<DegradeComponent> equal{{"a", 0.2}, {"b", 0.2}};
  check(degrade_set(equal, {}, Loads{1.0}) == std::vector<std::string>{"a"}, "equal cost keeps list order");
}


// ---- SaturatePolicy::decide on synthetic and recorded cgroup windows ----

Sample cgroup_sample(std::int64_t ts, double busy_ns, double pressure_ns, int nvalues = 4) {
  Sample s;
  s.timestamp_ns = ts;
  s.nvalues = nvalues;
  s.value[kCgroupTaskClock] = static_cast<std::uint64_t>(busy_ns);
  s.value[kCgroupPressure] = static_cast<std::uint64_t>(pressure_ns);
  return s;
}

// n samples 10 ms apart with the given per-sample occupancy (of cores) and pressure ratio.
// In runnable mode pressure is the runnable count of each sample.
OccupancyWindow synthetic(int cores, double occupancy, double pressure, int n = kScaleWindow + 1,
                          bool runnable = false) {
  OccupancyWindow window;
  window.cores = cores;
  window.runnable = runnable;
  for (int i = 0; i < n; ++i) {
    const double gap = 10e6;
    window.cgroup.push_back(
        cgroup_sample(1000000000LL + i * 10000000LL, occupancy * gap * cores, runnable ? pressure : pressure * gap));
  }
  return window;
}

const char* name_of(Decision d) {
  return d == Decision::AddCore ? "add" : d == Decision::RemoveCore ? "remove" : "hold";
}

void expect_decision(const OccupancyWindow& window, Decision want, const std::string& label) {
  SaturatePolicy policy;
  const auto got = policy.decide(window);
  check(got == want, label + ": want " + name_of(want) + " got " + name_of(got));
}

void test_decide_synthetic() {
  expect_decision(synthetic(1, 1.0, 1.0, kScaleWindow), Decision::Hold, "64 samples is not a full window");
  expect_decision(synthetic(1, 1.0, 1.0), Decision::AddCore, "6 threads on 1 cpu adds");
  expect_decision(synthetic(4, 1.0, 0.5), Decision::AddCore, "6 threads on 4 cpus adds");
  expect_decision(synthetic(4, 1.0, 0.0), Decision::Hold, "4 threads on 4 cpus holds");
  expect_decision(synthetic(1, 1.0, 0.004), Decision::Hold, "1 thread on 1 cpu holds");
  expect_decision(synthetic(4, 0.25, 0.0), Decision::RemoveCore, "1 thread on 4 cpus removes");
  expect_decision(synthetic(4, 0.84, 0.5), Decision::Hold, "occupancy below 0.85 does not add");
  expect_decision(synthetic(4, 0.90, 0.09), Decision::Hold, "pressure below 0.10 does not add");
  expect_decision(synthetic(4, 0.85, 0.10), Decision::AddCore, "add lines are inclusive");
  expect_decision(synthetic(4, 0.80, 0.0), Decision::Hold, "dead band between 0.70 and 0.85 holds");
  expect_decision(synthetic(4, 0.70, 0.0), Decision::RemoveCore, "remove line (cores-1)/cores-0.05 is inclusive");
  expect_decision(synthetic(4, 0.50, 0.03), Decision::Hold, "pressure above 0.02 keeps the cpu");
  expect_decision(synthetic(2, 0.40, 0.0), Decision::RemoveCore, "2 cores remove below 0.45");
  expect_decision(synthetic(2, 0.46, 0.0), Decision::RemoveCore, "2 cores at 0.46 remove under the relaxed rule");
  expect_decision(synthetic(2, 0.51, 0.0), Decision::Hold, "2 cores keep above 0.5");
  expect_decision(synthetic(1, 0.0, 0.0), Decision::Hold, "one cpu is never removed");
  auto limited = synthetic(4, 1.0, 1.0);
  limited.max_cores = 4;
  expect_decision(limited, Decision::Hold, "at max no add");
  auto low = synthetic(4, 0.1, 0.0);
  low.min_cores = 4;
  expect_decision(low, Decision::Hold, "at min no remove");
  auto under = synthetic(2, 0.0, 0.0, 1);
  under.min_cores = 3;
  expect_decision(under, Decision::AddCore, "under min adds immediately, without a window");
  auto over = synthetic(5, 1.0, 1.0, 1);
  over.max_cores = 4;
  expect_decision(over, Decision::RemoveCore, "over max removes immediately, without a window");
  // The mean decides: a noisy window whose 10 ms samples swing 0.6..1.4 but average 1.0 still adds.
  auto noisy = synthetic(1, 1.0, 1.0);
  for (std::size_t i = 1; i < noisy.cgroup.size(); ++i) {
    noisy.cgroup[i].value[kCgroupTaskClock] = (i % 2 == 0) ? 6000000 : 14000000;
  }
  expect_decision(noisy, Decision::AddCore, "decides on the window mean, not every sample");
  // Fresh window after a change: the monitor keeps only samples newer than changed_at_ns.
  auto fresh = synthetic(2, 1.0, 1.0);
  const auto changed_at = fresh.cgroup[10].timestamp_ns;
  std::vector<Sample> kept;
  for (const auto& s : fresh.cgroup) {
    if (s.timestamp_ns > changed_at) {
      kept.push_back(s);
    }
  }
  fresh.cgroup = kept;
  expect_decision(fresh, Decision::Hold, "after a change the next decision waits a full window");
  // Malformed samples hold.
  auto old_ring = synthetic(1, 1.0, 1.0);
  old_ring.cgroup.back().nvalues = 3;
  expect_decision(old_ring, Decision::Hold, "a sample without pressure holds");
  auto zero_gap = synthetic(1, 1.0, 1.0);
  zero_gap.cgroup.back().timestamp_ns = zero_gap.cgroup[zero_gap.cgroup.size() - 2].timestamp_ns;
  expect_decision(zero_gap, Decision::Hold, "a zero gap holds");
  // Relaxed remove: 6 busy threads on 7 CPUs, no pressure, must give one back.
  expect_decision(synthetic(7, 6.0 / 7.0, 0.0), Decision::RemoveCore, "6 threads on 7 cpus without pressure removes");
  expect_decision(synthetic(7, 6.0 / 7.0, 0.01), Decision::RemoveCore, "6 on 7 with pressure 0.01 removes");
  expect_decision(synthetic(7, 6.0 / 7.0, 0.05), Decision::Hold, "6 on 7 with pressure 0.05 holds");
  expect_decision(synthetic(7, 0.87, 0.0), Decision::Hold, "occupancy above 6/7 holds");
  expect_decision(synthetic(6, 1.0, 0.0), Decision::Hold, "6 threads on 6 cpus holds");
  // Runnable fallback (integer counts per sample): add at mean >= cores + 0.5, remove at mean <= cores - 1.
  expect_decision(synthetic(2, 1.0, 3, kScaleWindow + 1, true), Decision::AddCore, "runnable 3 on 2 adds");
  expect_decision(synthetic(2, 1.0, 2, kScaleWindow + 1, true), Decision::Hold, "runnable 2 on 2 holds");
  expect_decision(synthetic(4, 0.25, 1.0, kScaleWindow + 1, true), Decision::RemoveCore, "runnable 1 on 4 removes");
  expect_decision(synthetic(4, 0.25, 4, kScaleWindow + 1, true), Decision::Hold, "runnable 4 on 4 keeps");
  // window_means over one second, as widen uses it.
  const auto second = synthetic(2, 0.9, 0.2, 101);
  const auto means = window_means(second.cgroup, 2, 100, false);
  check(means && std::abs(means->occupancy - 0.9) < 1e-6 && std::abs(means->pressure - 0.2) < 1e-6,
        "window_means over 100 gaps");
  check(means && saturate_add_test(*means, 2, false), "widen add test passes at 0.9 / 0.2");
}

// Recorded traces: src/test/traces/*.csv, cgroup cpu.stat and cpu.pressure of spinner runs on dtn-01
// sampled every 10 ms. The header names the cpuset size and the decision every window must give.
void test_decide_traces() {
  int files = 0;
  for (const auto& entry : std::filesystem::directory_iterator(TRACES_DIR)) {
    if (entry.path().extension() != ".csv") {
      continue;
    }
    std::ifstream in(entry.path());
    std::string line;
    int cores = 0;
    std::string expect;
    OccupancyWindow window;
    while (std::getline(in, line)) {
      if (line.rfind("# cores=", 0) == 0) {
        cores = std::stoi(line.substr(8));
        expect = line.substr(line.find("expect=") + 7);
        expect = expect.substr(0, expect.find(' '));
        continue;
      }
      if (line.empty() || line[0] == '#') {
        continue;
      }
      std::istringstream row(line);
      std::string field;
      std::vector<std::int64_t> v;
      while (std::getline(row, field, ',')) {
        v.push_back(std::stoll(field));
      }
      if (v.size() == 5) {
        Sample s = cgroup_sample(v[0], static_cast<double>(v[1]), static_cast<double>(v[4]));
        s.value[1] = static_cast<std::uint64_t>(v[2]);
        s.value[2] = static_cast<std::uint64_t>(v[3]);
        window.cgroup.push_back(s);
      }
    }
    ++files;
    const Decision want = expect == "add" ? Decision::AddCore : expect == "remove" ? Decision::RemoveCore : Decision::Hold;
    const auto all = window.cgroup;
    int windows = 0;
    int wrong = 0;
    for (std::size_t end = kScaleWindow + 1; end <= all.size(); ++end) {
      OccupancyWindow w;
      w.cores = cores;
      w.cgroup.assign(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(end));
      SaturatePolicy policy;
      ++windows;
      if (policy.decide(w) != want) {
        ++wrong;
      }
    }
    check(windows > 0 && wrong == 0, entry.path().filename().string() + ": " + std::to_string(wrong) + "/" +
                                         std::to_string(windows) + " windows did not " + expect);
    std::cout << "trace " << entry.path().filename().string() << " " << windows << " windows " << expect << std::endl;
  }
  check(files >= 5, "recorded traces found in " + std::string(TRACES_DIR));
}

}  // namespace

int main() {
  test_interval_loads();
  test_node_hot();
  test_least_allocated();
  test_degrade_set();
  test_decide_synthetic();
  test_decide_traces();
  if (failures != 0) {
    std::cerr << "policy_test " << failures << " failure(s)" << std::endl;
    return 1;
  }
  std::cout << "policy_test ok" << std::endl;
  return 0;
}
