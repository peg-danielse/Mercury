// Unit tests for SaturatePolicy::decide in src/policy.cpp, on synthetic and recorded cgroup windows.
#include "policy.hpp"

#include <algorithm>
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

// ---- l3 policy: block choice on a dtn-01 shaped host ----

// 2 sockets, 8 NUMA nodes, 16 L3 domains of 2 cores with SMT: L3 i holds 2i, 2i+1, 2i+32, 2i+33.
// CPU 0 is Mercury's and not allocatable. taken are CPUs other daemons hold; mine this daemon's.
std::vector<L3Block> dtn01_blocks(const std::vector<int>& mine, const std::vector<int>& taken) {
  const auto in = [](const std::vector<int>& cpus, int cpu) { return std::find(cpus.begin(), cpus.end(), cpu) != cpus.end(); };
  std::vector<L3Block> blocks;
  for (int i = 0; i < 16; ++i) {
    L3Block block;
    block.name = "l3." + std::to_string(i);
    block.node = "node." + std::to_string(i / 2);
    block.socket = "socket." + std::to_string(i / 8);
    for (const int cpu : {2 * i, 2 * i + 1, 2 * i + 32, 2 * i + 33}) {
      if (cpu == 0) {
        block.partial = true;
        continue;
      }
      block.cpus.push_back(cpu);
      if (in(mine, cpu)) {
        ++block.held;
      } else if (!in(taken, cpu)) {
        block.free.push_back(cpu);
      }
    }
    blocks.push_back(block);
  }
  return blocks;
}

std::vector<int> l3_start(std::vector<int> have, std::size_t start, const std::vector<int>& taken = {}) {
  auto blocks = dtn01_blocks(have, taken);
  while (have.size() < start && l3_take_block(blocks, have, start - have.size())) {
  }
  return have;
}

void test_l3() {
  // From CPU 1: its own L3 (1, 32, 33), the other L3 of node 0 (2, 3, 34, 35), then one CPU of node 1.
  const auto start = l3_start({1}, 8);
  check(start == std::vector<int>({1, 2, 3, 4, 32, 33, 34, 35}), "l3 start from CPU 1 is node 0 plus CPU 4");

  // The next step tops up the partly held L3 2, then takes the other L3 on node 1.
  auto have = start;
  auto blocks = dtn01_blocks(have, {});
  check(l3_take_block(blocks, have, 64) && have.size() == 11, "l3 add tops up the partly held domain");
  check(l3_take_block(blocks, have, 64) && have.size() == 15 &&
            std::find(have.begin(), have.end(), 6) != have.end(),
        "l3 add then takes the free domain on the same node");

  // A domain another daemon touches is skipped: node 1's L3 3 is in use, so L3 2's socket neighbour wins.
  have = {2, 3, 34, 35};
  blocks = dtn01_blocks(have, {7});
  check(l3_take_block(blocks, have, 64) && have.size() == 7 && have.front() == 1, "l3 tops up L3 0 around CPU 0");
  check(l3_take_block(blocks, have, 64) && have.size() == 11 && std::find(have.begin(), have.end(), 4) != have.end(),
        "l3 then L3 2 on the same socket");
  check(std::find(have.begin(), have.end(), 6) == have.end(), "l3 never takes a domain another daemon uses");

  // room caps the step, and an empty room or a full machine adds nothing.
  have = {1};
  blocks = dtn01_blocks(have, {});
  check(l3_take_block(blocks, have, 1) && have == std::vector<int>({1, 32}), "l3 room caps the CPUs taken");
  check(!l3_take_block(blocks, have, 0), "l3 room 0 takes nothing");
  std::vector<int> all;
  for (int cpu = 1; cpu < 64; ++cpu) {
    all.push_back(cpu);
  }
  have = {1};
  blocks = dtn01_blocks(have, all);
  check(!l3_take_block(blocks, have, 64) && have.size() == 1, "l3 holds when no domain is free");

  // Load breaks ties inside a tier.
  have = {2, 3, 34, 35};
  blocks = dtn01_blocks(have, {1, 32, 33});
  for (auto& block : blocks) {
    block.load = block.name == "l3.2" ? 0.9 : 0.1;
  }
  check(l3_take_block(blocks, have, 64) && std::find(have.begin(), have.end(), 6) != have.end(),
        "l3 prefers the less loaded domain within a tier");
}

// ---- l3 pool: start set and growth ----

void test_pool() {
  // 8 CPUs from the services' start CPU 2: its domain (L3 1), then the near whole domain L3 2 on the same
  // socket; L3 0 is skipped although it shares the node, because it holds Mercury's CPU 0.
  const std::vector<int> eight{2, 3, 4, 5, 34, 35, 36, 37};
  check(pool_start(dtn01_blocks({}, {}), 8, 2) == eight, "pool start: 8 CPUs of whole domains from CPU 2");
  check(pool_start(dtn01_blocks({}, {}), 8, -1) == eight, "pool start without a seed: the first whole domain");
  check(pool_start(dtn01_blocks({}, {}), 1, 2) == std::vector<int>({2, 3, 34, 35}), "pool start rounds up to a whole domain");
  check(pool_start(dtn01_blocks({}, {}), 1000, 2).size() == 63, "pool start never exceeds the allocatable CPUs");

  // Growth: the pool's blocks hold the pool's CPUs; the next domain is the nearest free one, whole.
  auto pool = eight;
  auto blocks = dtn01_blocks(pool, {});
  check(l3_take_block(blocks, pool, 64) && pool.size() == 11 && std::find(pool.begin(), pool.end(), 1) != pool.end(),
        "pool grows by the free domain on a pooled node (L3 0)");
  check(l3_take_block(blocks, pool, 64) && pool.size() == 15 && std::find(pool.begin(), pool.end(), 6) != pool.end(),
        "then by the next near domain (L3 3)");
}

// ---- throttle: quota and release ----

void test_throttle() {
  check(throttle_quota_us(4, 0.75) == 100000, "throttle 4 CPUs cut 75% is one CPU of quota");
  check(throttle_quota_us(1, 0.75) == 25000, "throttle 1 CPU cut 75% is 25 ms per 100 ms");
  check(throttle_quota_us(1, 0.999) == 1000, "throttle quota floor is 1 ms");
  // 16 CPUs, cut 75%: a daemon using 1 CPU throttled comes back as 4, adding 3/16 = 0.1875.
  check(throttle_release_test(0.60, 1.0, 16, 0.75, 0.80), "release when 0.60 + 0.1875 <= 0.80");
  check(!throttle_release_test(0.65, 1.0, 16, 0.75, 0.80), "hold when 0.65 + 0.1875 > 0.80");
  check(!throttle_release_test(0.85, 0.0, 16, 0.75, 0.80), "hold while the host alone is above the line");
  check(throttle_release_test(0.10, 0.0, 16, 0.75, 0.80), "an idle daemon on a quiet host is released");
  check(!throttle_release_test(0.10, 1.0, 0, 0.75, 0.80), "no host CPUs never releases");
}

// ---- shift: plan on a dtn-01 shaped host ----

void set_loads(std::vector<L3Block>& blocks, double base, const std::vector<std::pair<std::string, double>>& loads) {
  for (auto& block : blocks) {
    block.load = base;
    for (const auto& [name, load] : loads) {
      if (block.name == name) {
        block.load = load;
      }
    }
  }
}

void test_shift() {
  // Two daemons share L3 1 at full load; this one (2 CPUs busy) moves to the near, cool L3 0.
  std::vector<int> have{2, 3};
  auto blocks = dtn01_blocks(have, {34, 35});
  set_loads(blocks, 0.1, {{"l3.1", 1.0}});
  auto plan = plan_shift(blocks, have, 2.0, 0.8);
  check(plan && plan->dest && blocks[*plan->dest].name == "l3.0", "shift from a shared hot L3 to the cool one on its node");
  check(plan && plan->leave == std::vector<int>({2, 3}) && plan->take == std::vector<int>({1, 32}),
        "shift leaves the hot domain's CPUs and takes as many low free CPUs");

  // A daemon filling its own domain would make any 4-CPU domain hot too: no destination, so it stays.
  have = {2, 3, 34, 35};
  blocks = dtn01_blocks(have, {});
  set_loads(blocks, 0.1, {{"l3.1", 1.0}});
  plan = plan_shift(blocks, have, 4.0, 0.8);
  check(plan && !plan->dest, "shift holds when the move would only make another domain hot");

  // A destination already loaded is passed over for one that stays under the line.
  have = {2, 3};
  blocks = dtn01_blocks(have, {34, 35});
  set_loads(blocks, 0.1, {{"l3.1", 1.0}, {"l3.0", 0.5}});
  plan = plan_shift(blocks, have, 2.0, 0.8);
  check(plan && plan->dest && blocks[*plan->dest].name == "l3.2", "shift skips a destination it would push over the line");

  // Only the hot part of a spanning cpuset moves.
  have = {2, 3, 4, 5};
  blocks = dtn01_blocks(have, {34, 35});
  set_loads(blocks, 0.1, {{"l3.1", 0.95}, {"l3.2", 0.6}});
  plan = plan_shift(blocks, have, 3.0, 0.8);
  check(plan && blocks[plan->source].name == "l3.1" && plan->leave == std::vector<int>({2, 3}),
        "shift moves only the CPUs in the hot domain");

  // An idle daemon on a hot domain has nothing to carry away: it holds.
  have = {2, 3};
  blocks = dtn01_blocks(have, {34, 35});
  set_loads(blocks, 0.1, {{"l3.1", 1.0}});
  plan = plan_shift(blocks, have, 0.01, 0.8);
  check(plan && !plan->dest, "shift holds an idle daemon (moving it relieves nothing)");

  // Nothing hot, or the hot domain cooling down after a move: no plan.
  set_loads(blocks, 0.1, {});
  check(!plan_shift(blocks, have, 3.0, 0.8), "shift does nothing below the line");
  set_loads(blocks, 0.1, {{"l3.1", 1.0}});
  check(!plan_shift(blocks, have, 3.0, 0.8, {"l3.1"}), "shift skips a domain that just lost a daemon");
}

void test_pool_swap() {
  // Pool L3 1 + L3 2; an antagonist puts 2 CPUs of foreign load on L3 2.
  auto blocks = dtn01_blocks({2, 3, 4, 5, 34, 35, 36, 37}, {});
  std::vector<double> foreign(blocks.size(), 0.0);
  foreign[2] = 2.0;
  set_loads(blocks, 0.0, {{"l3.1", 0.6}, {"l3.2", 0.9}});
  auto swap = plan_pool_swap(blocks, foreign);
  check(swap && blocks[swap->from].name == "l3.2" && blocks[swap->to].name == "l3.3",
        "pool swap: the domain with foreign load goes, the near clean free domain comes");
  // L3 0 is closer (same node as L3 1) but holds Mercury's CPU: never a destination.
  check(swap && blocks[swap->to].name != "l3.0", "pool swap never takes a domain with Mercury's CPU");
  // Busy only from the daemons themselves (saturation): no swap.
  foreign.assign(blocks.size(), 0.1);
  check(!plan_pool_swap(blocks, foreign), "pool swap ignores saturation by Mercury's own daemons");
  // A destination already loaded is not clean.
  foreign[2] = 2.0;
  set_loads(blocks, 0.5, {{"l3.1", 0.6}, {"l3.2", 0.9}});
  check(!plan_pool_swap(blocks, foreign), "pool swap needs a clean destination");
}

int main() {
  test_decide_synthetic();
  test_decide_traces();
  test_l3();
  test_pool();
  test_pool_swap();
  test_throttle();
  test_shift();
  if (failures != 0) {
    std::cerr << "policy_test " << failures << " failure(s)" << std::endl;
    return 1;
  }
  std::cout << "policy_test ok" << std::endl;
  return 0;
}
