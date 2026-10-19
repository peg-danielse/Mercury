// Unit tests for SaturatePolicy::decide in src/policy.cpp, on synthetic and recorded cgroup windows.
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
  // window_means over a 100-gap window.
  const auto second = synthetic(2, 0.9, 0.2, 101);
  const auto means = window_means(second.cgroup, 2, 100, false);
  check(means && std::abs(means->occupancy - 0.9) < 1e-6 && std::abs(means->pressure - 0.2) < 1e-6,
        "window_means over 100 gaps");
  check(means && saturate_add_test(*means, 2, false), "add test passes at 0.9 / 0.2");
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
  test_decide_synthetic();
  test_decide_traces();
  if (failures != 0) {
    std::cerr << "policy_test " << failures << " failure(s)" << std::endl;
    return 1;
  }
  std::cout << "policy_test ok" << std::endl;
  return 0;
}
