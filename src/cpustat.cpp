#include "cpustat.hpp"

#include "counters.hpp"
#include "ring.hpp"
#include "ring_archive.hpp"

#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace {

constexpr int kBusy = 0;
constexpr int kIrq = 1;
constexpr int kRunDelay = 2;
constexpr int kSlices = 3;
constexpr int kDaemons = 7;
// Archive every second from the sampling thread: SIGTERM ends Mercury with _exit, so at most this much is lost.
constexpr int kFlushMs = 1000;

struct CpuTarget {
  int cpu = 0;
  std::shared_ptr<Ring> ring;
  CounterGroup perf;
  int perf_column[kMaxCounters]{};  // ring column of each opened perf event, in opened order; -1 skips it
};

bool enabled = false;
std::mutex placement_mu;
std::map<std::string, std::vector<int>> placement;  // daemon -> cpuset, daemons with a non-empty set only
std::unordered_map<int, int> daemons_on;             // cpu -> daemons whose cpuset contains it
std::ofstream placement_log;

std::string read_file(const std::string& path) {
  std::ifstream in(path);
  if (!in) {
    return {};
  }
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

std::int64_t monotonic_ns() {
  timespec now{};
  if (::clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    return 0;
  }
  return static_cast<std::int64_t>(now.tv_sec) * 1000000000LL + now.tv_nsec;
}

std::uint64_t delta(std::uint64_t after, std::uint64_t before) { return after >= before ? after - before : 0; }

// An archive with other columns (an older layout) is moved aside, as for the daemon rings.
std::shared_ptr<RingArchive> open_archive(const std::string& path) {
  try {
    return std::make_shared<RingArchive>(path, kCpustatNames);
  } catch (const std::exception& first) {
    const auto aside = path + "." + std::to_string(std::time(nullptr));
    try {
      if (std::rename(path.c_str(), aside.c_str()) != 0) {
        throw std::runtime_error("rename failed");
      }
      std::cout << "cpustat archive " << path << " " << first.what() << ", moved to " << aside << std::endl;
      return std::make_shared<RingArchive>(path, kCpustatNames);
    } catch (const std::exception& error) {
      std::cerr << "cpustat archive " << path << " " << error.what() << std::endl;
      return nullptr;
    }
  }
}

void sample_loop(std::vector<CpuTarget> targets, std::uint64_t tick_ns, int period_ms, int cpu) {
  if (cpu >= 0) {
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(cpu, &mask);
    if (::pthread_setaffinity_np(::pthread_self(), sizeof(mask), &mask) != 0) {
      std::cerr << "cpustat: cannot pin the sampler to cpu " << cpu << std::endl;
    }
  }
  const long flush_every = std::max(1, kFlushMs / period_ms);
  auto previous_stat = parse_proc_stat(read_file("/proc/stat"));
  auto previous_sched = parse_schedstat(read_file("/proc/schedstat"));
  auto next = std::chrono::steady_clock::now();
  for (long tick = 1;; ++tick) {
    // Fixed cadence: reading every CPU takes a few ms, which a plain sleep would add to each period.
    next += std::chrono::milliseconds(period_ms);
    std::this_thread::sleep_until(next);
    const auto stat = parse_proc_stat(read_file("/proc/stat"));
    const auto sched = parse_schedstat(read_file("/proc/schedstat"));
    std::unordered_map<int, int> on;
    {
      std::lock_guard lock(placement_mu);
      on = daemons_on;
    }
    const auto stamp = monotonic_ns();
    for (auto& target : targets) {
      const auto s1 = stat.find(target.cpu);
      const auto s0 = previous_stat.find(target.cpu);
      const auto r1 = sched.find(target.cpu);
      const auto r0 = previous_sched.find(target.cpu);
      if (s1 == stat.end() || s0 == previous_stat.end()) {
        continue;
      }
      Sample sample;
      sample.timestamp_ns = stamp;
      sample.nvalues = static_cast<int>(kCpustatNames.size());
      sample.value[kIrq] = delta(s1->second.irq, s0->second.irq) * tick_ns;
      if (r1 != sched.end() && r0 != previous_sched.end()) {
        sample.value[kBusy] = delta(r1->second.run_ns, r0->second.run_ns);
        sample.value[kRunDelay] = delta(r1->second.run_delay_ns, r0->second.run_delay_ns);
        sample.value[kSlices] = delta(r1->second.slices, r0->second.slices);
      }
      Sample perf;
      if (!target.perf.empty() && target.perf.pull(perf)) {
        for (int i = 0; i < perf.nvalues; ++i) {
          if (target.perf_column[i] >= 0) {
            sample.value[target.perf_column[i]] = perf.value[i];
          }
        }
        sample.time_enabled = perf.time_enabled;
        sample.time_running = perf.time_running;
      }
      const auto here = on.find(target.cpu);
      sample.value[kDaemons] = here == on.end() ? 0 : static_cast<std::uint64_t>(here->second);
      target.ring->push(sample);
    }
    if (tick % flush_every == 0) {
      for (auto& target : targets) {
        target.ring->flush();
      }
    }
    previous_stat = stat;
    previous_sched = sched;
  }
}

}  // namespace

std::vector<int> parse_cpu_ranges(std::string_view text) {
  std::vector<int> cpus;
  std::string item;
  std::istringstream in{std::string(text)};
  while (std::getline(in, item, ',')) {
    while (!item.empty() && (item.back() == '\n' || item.back() == ' ')) {
      item.pop_back();
    }
    if (item.empty()) {
      continue;
    }
    try {
      const auto dash = item.find('-');
      const int first = std::stoi(item.substr(0, dash));
      const int last = dash == std::string::npos ? first : std::stoi(item.substr(dash + 1));
      if (first < 0 || last < first) {
        return {};
      }
      for (int cpu = first; cpu <= last; ++cpu) {
        cpus.push_back(cpu);
      }
    } catch (const std::exception&) {
      return {};
    }
  }
  return cpus;
}

std::unordered_map<int, ProcStatCpu> parse_proc_stat(std::string_view text) {
  std::unordered_map<int, ProcStatCpu> cpus;
  std::istringstream in{std::string(text)};
  std::string line;
  while (std::getline(in, line)) {
    if (line.size() < 4 || line.compare(0, 3, "cpu") != 0 || line[3] < '0' || line[3] > '9') {
      continue;
    }
    std::istringstream row(line);
    std::string label;
    std::uint64_t user = 0, nice = 0, system = 0, idle = 0, iowait = 0, irq = 0, softirq = 0;
    if (!(row >> label >> user >> nice >> system >> idle >> iowait >> irq >> softirq)) {
      continue;
    }
    try {
      cpus[std::stoi(label.substr(3))] = ProcStatCpu{user + nice + system + irq + softirq, irq + softirq};
    } catch (const std::exception&) {
    }
  }
  return cpus;
}

std::unordered_map<int, SchedstatCpu> parse_schedstat(std::string_view text) {
  std::unordered_map<int, SchedstatCpu> cpus;
  std::istringstream in{std::string(text)};
  std::string line;
  while (std::getline(in, line)) {
    if (line.size() < 4 || line.compare(0, 3, "cpu") != 0 || line[3] < '0' || line[3] > '9') {
      continue;
    }
    std::istringstream row(line);
    std::string label;
    std::uint64_t field[9]{};
    row >> label;
    int read = 0;
    while (read < 9 && row >> field[read]) {
      ++read;
    }
    if (read < 9) {
      continue;
    }
    try {
      cpus[std::stoi(label.substr(3))] = SchedstatCpu{field[6], field[7], field[8]};
    } catch (const std::exception&) {
    }
  }
  return cpus;
}

void init_cpustat(const std::string& dir, int period_ms, int cpu) {
  if (enabled || period_ms <= 0) {
    return;
  }
  auto online = parse_cpu_ranges(read_file("/sys/devices/system/cpu/online"));
  if (online.empty()) {
    std::cerr << "cpustat: no online CPUs" << std::endl;
    return;
  }
  const long tick = ::sysconf(_SC_CLK_TCK);
  const auto tick_ns = tick > 0 ? 1000000000ULL / static_cast<std::uint64_t>(tick) : 10000000ULL;
  std::vector<CpuTarget> targets;
  int perf_cpus = 0;
  std::string perf_error;
  for (const int cpu : online) {
    CpuTarget target;
    target.cpu = cpu;
    target.ring = std::make_shared<Ring>(kRingSlots, kCpustatNames,
                                         open_archive(dir + "/cpu-" + std::to_string(cpu) + ".mring"));
    try {
      target.perf = open_cpu_hardware(cpu);
      const auto names = target.perf.names();
      for (std::size_t i = 0; i < names.size() && i < kMaxCounters; ++i) {
        const auto at = std::find(kCpustatNames.begin(), kCpustatNames.end(), names[i]);
        target.perf_column[i] = at == kCpustatNames.end() ? -1 : static_cast<int>(at - kCpustatNames.begin());
      }
      target.perf.arm();
      ++perf_cpus;
    } catch (const std::exception& error) {
      target.perf = CounterGroup();
      perf_error = error.what();
    }
    targets.push_back(std::move(target));
  }
  placement_log.open(dir + "/placement.jsonl", std::ios::app);
  enabled = true;
  std::cout << "cpustat cpus=" << targets.size() << " perf=" << perf_cpus << " period_ms=" << period_ms
            << " sampler_cpu=" << cpu << " dir=" << dir
            << (perf_error.empty() ? "" : " perf error: " + perf_error) << std::endl;
  std::thread(sample_loop, std::move(targets), tick_ns, period_ms, cpu).detach();
}

bool cpustat_enabled() { return enabled; }

void note_cpuset(const std::string& daemon_id, const std::vector<int>& cpus) {
  if (!enabled) {
    return;
  }
  std::vector<int> sorted = cpus;
  std::sort(sorted.begin(), sorted.end());
  std::lock_guard lock(placement_mu);
  const auto known = placement.find(daemon_id);
  if (known == placement.end() ? sorted.empty() : known->second == sorted) {
    return;
  }
  if (known != placement.end()) {
    for (const int cpu : known->second) {
      if (--daemons_on[cpu] <= 0) {
        daemons_on.erase(cpu);
      }
    }
  }
  if (sorted.empty()) {
    placement.erase(daemon_id);
  } else {
    placement[daemon_id] = sorted;
    for (const int cpu : sorted) {
      ++daemons_on[cpu];
    }
  }
  if (placement_log) {
    placement_log << "{\"t_ns\":" << monotonic_ns() << ",\"daemon\":\"" << daemon_id << "\",\"cpus\":[";
    for (std::size_t i = 0; i < sorted.size(); ++i) {
      placement_log << (i ? "," : "") << sorted[i];
    }
    placement_log << "]}\n" << std::flush;
  }
}
