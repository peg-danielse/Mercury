#include "resources.hpp"

#include "policy.hpp"
#include "ring.hpp"

#include <dirent.h>
#include <sched.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

// /proc/stat deltas for one group, in nanoseconds. busy is user + nice + system + irq + softirq.
constexpr int kUser = 0;
constexpr int kNice = 1;
constexpr int kSystem = 2;
constexpr int kIdle = 3;
constexpr int kIowait = 4;
constexpr int kIrq = 5;
constexpr int kSoftirq = 6;
constexpr int kBusy = 7;

const std::vector<std::string> kMetricNames{"user", "nice", "system", "idle", "iowait", "irq", "softirq", "busy"};

struct Ticks {
  std::uint64_t user = 0;
  std::uint64_t nice = 0;
  std::uint64_t system = 0;
  std::uint64_t idle = 0;
  std::uint64_t iowait = 0;
  std::uint64_t irq = 0;
  std::uint64_t softirq = 0;
};

struct Group {
  ResourceLevel level = ResourceLevel::Core;
  std::string name;
  std::vector<int> linux_cpus;
  int parent = -1;
  std::vector<int> children;
  std::shared_ptr<Ring> ring;
};

struct MetricTarget {
  std::vector<int> linux_cpus;
  std::shared_ptr<Ring> ring;
};

// load_of averages the last 100 ms, independent of the saturate window.
constexpr std::size_t kLoadWindow = 10;

bool ready = false;
void (*policy_thread)() = nullptr;
std::vector<Group> groups;
std::vector<char> known;
std::vector<char> allocatable;

std::string trimmed(std::string_view value) {
  std::size_t begin = 0;
  while (begin < value.size() && (value[begin] == ' ' || value[begin] == '\n' || value[begin] == '\t')) {
    ++begin;
  }
  std::size_t end = value.size();
  while (end > begin && (value[end - 1] == ' ' || value[end - 1] == '\n' || value[end - 1] == '\t' ||
                         value[end - 1] == '\r')) {
    --end;
  }
  return std::string(value.substr(begin, end - begin));
}

std::string read_file(const std::string& path) {
  std::ifstream in(path);
  if (!in) {
    return {};
  }
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

std::vector<int> parse_cpu_list(std::string_view text) {
  std::vector<int> cpus;
  if (text.empty()) {
    return cpus;
  }
  std::size_t i = 0;
  auto read_cpu = [&](int& cpu) {
    if (i >= text.size() || text[i] < '0' || text[i] > '9') {
      return false;
    }
    if (text[i] == '0' && i + 1 < text.size() && text[i + 1] >= '0' && text[i + 1] <= '9') {
      return false;
    }
    int value = 0;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
      value = value * 10 + (text[i] - '0');
      ++i;
      if (value >= CPU_SETSIZE) {
        return false;
      }
    }
    cpu = value;
    return true;
  };
  while (i < text.size()) {
    if (!cpus.empty()) {
      if (text[i] != ',') {
        return {};
      }
      ++i;
    }
    int first = 0;
    if (!read_cpu(first)) {
      return {};
    }
    int last = first;
    if (i < text.size() && text[i] == '-') {
      ++i;
      if (!read_cpu(last) || last < first) {
        return {};
      }
    }
    for (int cpu = first; cpu <= last; ++cpu) {
      cpus.push_back(cpu);
    }
  }
  std::sort(cpus.begin(), cpus.end());
  cpus.erase(std::unique(cpus.begin(), cpus.end()), cpus.end());
  return cpus;
}

std::string cpu_key(const std::vector<int>& cpus) {
  std::string key;
  for (const int cpu : cpus) {
    if (!key.empty()) {
      key.push_back(',');
    }
    key += std::to_string(cpu);
  }
  return key;
}

std::vector<int> keep_known(const std::vector<int>& cpus) {
  std::vector<int> kept;
  for (const int cpu : cpus) {
    if (cpu >= 0 && cpu < static_cast<int>(known.size()) && known[cpu] != 0) {
      kept.push_back(cpu);
    }
  }
  std::sort(kept.begin(), kept.end());
  kept.erase(std::unique(kept.begin(), kept.end()), kept.end());
  return kept;
}

int lowest_cpu(const Group& group) {
  if (group.linux_cpus.empty()) {
    return CPU_SETSIZE;
  }
  return group.linux_cpus.front();
}

std::int64_t monotonic_ns() {
  timespec now{};
  if (::clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    return 0;
  }
  return static_cast<std::int64_t>(now.tv_sec) * 1000000000LL + now.tv_nsec;
}

std::uint64_t tick_delta(std::uint64_t now, std::uint64_t before) {
  if (now < before) {
    return 0;
  }
  return now - before;
}

std::string l3_key_for(int cpu) {
  const auto dir_path = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cache";
  DIR* dir = ::opendir(dir_path.c_str());
  if (dir == nullptr) {
    return std::to_string(cpu);
  }
  std::string found;
  while (const auto* entry = ::readdir(dir)) {
    const std::string name = entry->d_name;
    if (name == "." || name == "..") {
      continue;
    }
    const auto index = dir_path + "/" + name;
    if (trimmed(read_file(index + "/level")) != "3") {
      continue;
    }
    if (trimmed(read_file(index + "/type")) == "Instruction") {
      continue;
    }
    const auto shared = keep_known(parse_cpu_list(trimmed(read_file(index + "/shared_cpu_list"))));
    if (!shared.empty()) {
      found = cpu_key(shared);
      break;
    }
  }
  ::closedir(dir);
  if (found.empty()) {
    return std::to_string(cpu);
  }
  return found;
}

std::string core_key_for(int cpu) {
  const auto path = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/topology/thread_siblings_list";
  auto shared = keep_known(parse_cpu_list(trimmed(read_file(path))));
  if (shared.empty()) {
    const auto fallback =
        "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/topology/core_cpus_list";
    shared = keep_known(parse_cpu_list(trimmed(read_file(fallback))));
  }
  if (shared.empty()) {
    return std::to_string(cpu);
  }
  return cpu_key(shared);
}

int socket_id_for(int cpu) {
  const auto text =
      trimmed(read_file("/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/topology/physical_package_id"));
  if (text.empty()) {
    return 0;
  }
  try {
    return std::stoi(text);
  } catch (const std::exception&) {
    return 0;
  }
}

int ensure_group(std::unordered_map<std::string, int>& index, ResourceLevel level, const std::string& key,
                 const std::string& name, int parent) {
  const auto it = index.find(key);
  if (it != index.end()) {
    return it->second;
  }
  Group group;
  group.level = level;
  group.name = name;
  group.parent = parent;
  const int id = static_cast<int>(groups.size());
  groups.push_back(std::move(group));
  index.emplace(key, id);
  return id;
}

void add_member(int index, int cpu) { groups[static_cast<std::size_t>(index)].linux_cpus.push_back(cpu); }

void finish_groups() {
  for (auto& group : groups) {
    std::sort(group.linux_cpus.begin(), group.linux_cpus.end());
    group.linux_cpus.erase(std::unique(group.linux_cpus.begin(), group.linux_cpus.end()), group.linux_cpus.end());
  }
  for (int i = 0; i < static_cast<int>(groups.size()); ++i) {
    const int parent = groups[static_cast<std::size_t>(i)].parent;
    if (parent >= 0) {
      groups[static_cast<std::size_t>(parent)].children.push_back(i);
    }
  }
  const auto by_cpu = [](int left, int right) { return lowest_cpu(groups[static_cast<std::size_t>(left)]) <
                                                       lowest_cpu(groups[static_cast<std::size_t>(right)]); };
  for (auto& group : groups) {
    std::sort(group.children.begin(), group.children.end(), by_cpu);
  }

  for (const auto level : {ResourceLevel::L3, ResourceLevel::Core}) {
    std::vector<int> ids;
    for (int i = 0; i < static_cast<int>(groups.size()); ++i) {
      if (groups[static_cast<std::size_t>(i)].level == level) {
        ids.push_back(i);
      }
    }
    std::sort(ids.begin(), ids.end(), by_cpu);
    const char* prefix = level == ResourceLevel::L3 ? "l3" : "core";
    for (int n = 0; n < static_cast<int>(ids.size()); ++n) {
      groups[static_cast<std::size_t>(ids[static_cast<std::size_t>(n)])].name =
          std::string(prefix) + "." + std::to_string(n);
    }
  }
}

double load_of(const Ring& ring) {
  const auto samples = ring.recent(kLoadWindow);
  std::uint64_t busy = 0;
  std::uint64_t idle = 0;
  std::uint64_t iowait = 0;
  for (const auto& sample : samples) {
    if (sample.nvalues <= kBusy) {
      continue;
    }
    busy += sample.value[kBusy];
    idle += sample.value[kIdle];
    iowait += sample.value[kIowait];
  }
  const auto span = busy + idle + iowait;
  if (span == 0) {
    return 0;
  }
  return static_cast<double>(busy) / static_cast<double>(span);
}

ResourceGroupState state_of(const Group& group) {
  ResourceGroupState state;
  state.level = group.level;
  state.name = group.name;
  state.linux_cpus = group.linux_cpus;
  if (group.ring) {
    state.samples = group.ring->size();
    state.load = load_of(*group.ring);
  }
  return state;
}

bool less_loaded(const ResourceGroupState& candidate, const ResourceGroupState& best) {
  if (candidate.load != best.load) {
    return candidate.load < best.load;
  }
  const int candidate_cpu = candidate.linux_cpus.empty() ? CPU_SETSIZE : candidate.linux_cpus.front();
  const int best_cpu = best.linux_cpus.empty() ? CPU_SETSIZE : best.linux_cpus.front();
  if (candidate_cpu != best_cpu) {
    return candidate_cpu < best_cpu;
  }
  return candidate.name < best.name;
}

bool intersects(const std::vector<int>& group_cpus, const std::vector<int>& want) {
  for (const int cpu : want) {
    if (std::binary_search(group_cpus.begin(), group_cpus.end(), cpu)) {
      return true;
    }
  }
  return false;
}

bool is_allocatable(int cpu) {
  return cpu >= 0 && cpu < static_cast<int>(allocatable.size()) && allocatable[static_cast<std::size_t>(cpu)] != 0;
}

// Lowest allocatable Linux CPU on this core that the caller does not already hold.
std::optional<int> lowest_free(const Group& group, const std::vector<int>& skip) {
  for (const int cpu : group.linux_cpus) {
    if (!is_allocatable(cpu) || std::binary_search(skip.begin(), skip.end(), cpu)) {
      continue;
    }
    return cpu;
  }
  return std::nullopt;
}

int held_in(const Group& group, const std::vector<int>& skip) {
  int count = 0;
  for (const int cpu : group.linux_cpus) {
    if (std::binary_search(skip.begin(), skip.end(), cpu)) {
      ++count;
    }
  }
  return count;
}

// Fewer CPUs already held keeps sibling groups even. Equal counts take the lower load,
// then the lower Linux CPU, the same tie break as less_loaded.
bool more_balanced(const Group& candidate, const Group& best, const std::vector<int>& skip) {
  const int candidate_held = held_in(candidate, skip);
  const int best_held = held_in(best, skip);
  if (candidate_held != best_held) {
    return candidate_held < best_held;
  }
  const auto candidate_state = state_of(candidate);
  const auto best_state = state_of(best);
  if (candidate_state.load != best_state.load) {
    return candidate_state.load < best_state.load;
  }
  const int candidate_cpu = candidate.linux_cpus.empty() ? CPU_SETSIZE : candidate.linux_cpus.front();
  const int best_cpu = best.linux_cpus.empty() ? CPU_SETSIZE : best.linux_cpus.front();
  if (candidate_cpu != best_cpu) {
    return candidate_cpu < best_cpu;
  }
  return candidate.name < best.name;
}

// Socket, then node, then L3, then core. Each step stays inside the group chosen above it.
std::optional<int> balanced_cpu(const std::vector<int>& skip) {
  if (!ready) {
    return std::nullopt;
  }
  std::vector<int> level;
  for (int id = 0; id < static_cast<int>(groups.size()); ++id) {
    if (groups[static_cast<std::size_t>(id)].parent < 0) {
      level.push_back(id);
    }
  }
  int chosen = -1;
  while (!level.empty()) {
    int best = -1;
    for (const int id : level) {
      if (!lowest_free(groups[static_cast<std::size_t>(id)], skip)) {
        continue;
      }
      if (best < 0 || more_balanced(groups[static_cast<std::size_t>(id)], groups[static_cast<std::size_t>(best)], skip)) {
        best = id;
      }
    }
    if (best < 0) {
      return std::nullopt;
    }
    chosen = best;
    level = groups[static_cast<std::size_t>(best)].children;
  }
  if (chosen < 0) {
    return std::nullopt;
  }
  return lowest_free(groups[static_cast<std::size_t>(chosen)], skip);
}

std::optional<ResourceGroupState> least_among(ResourceLevel level, const std::vector<int>* touching) {
  std::optional<ResourceGroupState> best;
  for (const auto& group : groups) {
    if (group.level != level) {
      continue;
    }
    if (touching != nullptr && !intersects(group.linux_cpus, *touching)) {
      continue;
    }
    auto state = state_of(group);
    if (!best || less_loaded(state, *best)) {
      best = std::move(state);
    }
  }
  return best;
}

std::unordered_map<int, Ticks> read_proc_stat() {
  std::unordered_map<int, Ticks> ticks;
  std::ifstream in("/proc/stat");
  std::string line;
  while (std::getline(in, line)) {
    if (line.size() < 4 || line.compare(0, 3, "cpu") != 0 || line[3] < '0' || line[3] > '9') {
      continue;
    }
    std::istringstream row(line);
    std::string label;
    Ticks sample;
    row >> label >> sample.user >> sample.nice >> sample.system >> sample.idle >> sample.iowait >> sample.irq >>
        sample.softirq;
    try {
      ticks.emplace(std::stoi(label.substr(3)), sample);
    } catch (const std::exception&) {
    }
  }
  return ticks;
}

void sample_once(const std::vector<MetricTarget>& targets, const std::unordered_map<int, Ticks>& previous,
                 const std::unordered_map<int, Ticks>& now, std::uint64_t tick_ns) {
  const auto stamp = monotonic_ns();
  for (const auto& target : targets) {
    Ticks sum;
    bool any = false;
    for (const int cpu : target.linux_cpus) {
      const auto before = previous.find(cpu);
      const auto after = now.find(cpu);
      if (before == previous.end() || after == now.end()) {
        continue;
      }
      any = true;
      sum.user += tick_delta(after->second.user, before->second.user);
      sum.nice += tick_delta(after->second.nice, before->second.nice);
      sum.system += tick_delta(after->second.system, before->second.system);
      sum.idle += tick_delta(after->second.idle, before->second.idle);
      sum.iowait += tick_delta(after->second.iowait, before->second.iowait);
      sum.irq += tick_delta(after->second.irq, before->second.irq);
      sum.softirq += tick_delta(after->second.softirq, before->second.softirq);
    }
    if (!any || !target.ring) {
      continue;
    }
    Sample sample;
    sample.timestamp_ns = stamp;
    sample.nvalues = 8;
    sample.value[kUser] = sum.user * tick_ns;
    sample.value[kNice] = sum.nice * tick_ns;
    sample.value[kSystem] = sum.system * tick_ns;
    sample.value[kIdle] = sum.idle * tick_ns;
    sample.value[kIowait] = sum.iowait * tick_ns;
    sample.value[kIrq] = sum.irq * tick_ns;
    sample.value[kSoftirq] = sum.softirq * tick_ns;
    sample.value[kBusy] = (sum.user + sum.nice + sum.system + sum.irq + sum.softirq) * tick_ns;
    sample.time_running = sample.value[kBusy];
    sample.time_enabled = sample.value[kBusy] + sample.value[kIdle] + sample.value[kIowait];
    target.ring->push(sample);
  }
}

void sample_loop(std::vector<MetricTarget> targets) {
  const long tick = ::sysconf(_SC_CLK_TCK);
  const auto tick_ns = tick > 0 ? 1000000000ULL / static_cast<std::uint64_t>(tick) : 10000000ULL;
  auto previous = read_proc_stat();
  while (true) {
    std::this_thread::sleep_for(std::chrono::milliseconds(kSampleMs));
    try {
      auto now = read_proc_stat();
      sample_once(targets, previous, now, tick_ns);
      previous = std::move(now);
    } catch (const std::exception& error) {
      std::cerr << "resources " << error.what() << std::endl;
    }
  }
}

int count_level(ResourceLevel level) {
  int count = 0;
  for (const auto& group : groups) {
    if (group.level == level) {
      ++count;
    }
  }
  return count;
}

}  // namespace

void init_resources(std::vector<int> allocatable_linux_cpus) {
  if (ready) {
    return;
  }
  auto online = parse_cpu_list(trimmed(read_file("/sys/devices/system/cpu/online")));
  if (online.empty()) {
    online = allocatable_linux_cpus;
  }
  int max_cpu = 0;
  for (const int cpu : online) {
    max_cpu = std::max(max_cpu, cpu);
  }
  for (const int cpu : allocatable_linux_cpus) {
    max_cpu = std::max(max_cpu, cpu);
  }
  known.assign(static_cast<std::size_t>(max_cpu) + 1, 0);
  allocatable.assign(known.size(), 0);
  for (const int cpu : online) {
    if (cpu >= 0 && cpu < static_cast<int>(known.size())) {
      known[static_cast<std::size_t>(cpu)] = 1;
    }
  }

  std::vector<int> node_of(known.size(), 0);
  DIR* nodes = ::opendir("/sys/devices/system/node");
  if (nodes != nullptr) {
    while (const auto* entry = ::readdir(nodes)) {
      const std::string name = entry->d_name;
      if (name.size() < 5 || name.compare(0, 4, "node") != 0) {
        continue;
      }
      int node = 0;
      try {
        node = std::stoi(name.substr(4));
      } catch (const std::exception&) {
        continue;
      }
      for (const int cpu : parse_cpu_list(trimmed(read_file("/sys/devices/system/node/" + name + "/cpulist")))) {
        if (cpu >= 0 && cpu < static_cast<int>(node_of.size())) {
          node_of[static_cast<std::size_t>(cpu)] = node;
        }
      }
    }
    ::closedir(nodes);
  }

  std::unordered_map<std::string, int> index;
  for (const int cpu : online) {
    if (cpu < 0 || cpu >= static_cast<int>(known.size()) || known[static_cast<std::size_t>(cpu)] == 0) {
      continue;
    }
    const int socket = socket_id_for(cpu);
    const int node = node_of[static_cast<std::size_t>(cpu)];
    const auto l3 = l3_key_for(cpu);
    const auto core = core_key_for(cpu);
    const int socket_index = ensure_group(index, ResourceLevel::Socket, "socket:" + std::to_string(socket),
                                           "socket." + std::to_string(socket), -1);
    const int node_index = ensure_group(index, ResourceLevel::Node, "node:" + std::to_string(node),
                                         "node." + std::to_string(node), socket_index);
    const int l3_index = ensure_group(index, ResourceLevel::L3, "l3:" + l3, "l3", node_index);
    const int core_index = ensure_group(index, ResourceLevel::Core, "core:" + core, "core", l3_index);
    add_member(socket_index, cpu);
    add_member(node_index, cpu);
    add_member(l3_index, cpu);
    add_member(core_index, cpu);
  }
  finish_groups();

  for (const int cpu : allocatable_linux_cpus) {
    if (cpu >= 0 && cpu < static_cast<int>(allocatable.size()) && known[static_cast<std::size_t>(cpu)] != 0) {
      allocatable[static_cast<std::size_t>(cpu)] = 1;
    }
  }
  std::vector<MetricTarget> targets;
  targets.reserve(groups.size());
  for (auto& group : groups) {
    group.ring = std::make_shared<Ring>(kRingSlots, kMetricNames);
    targets.push_back(MetricTarget{group.linux_cpus, group.ring});
  }

  int allocatable_count = 0;
  for (const char bit : allocatable) {
    allocatable_count += bit != 0 ? 1 : 0;
  }
  std::cout << "resources sockets=" << count_level(ResourceLevel::Socket) << " nodes=" << count_level(ResourceLevel::Node)
            << " l3=" << count_level(ResourceLevel::L3) << " cores=" << count_level(ResourceLevel::Core)
            << " cpus=" << online.size() << " allocatable=" << allocatable_count << std::endl;
  ready = !groups.empty();
  if (ready) {
    std::thread(sample_loop, std::move(targets)).detach();
    if (policy_thread != nullptr) {
      std::thread(policy_thread).detach();
    }
  }
}

void set_policy_loop(void (*loop)()) { policy_thread = loop; }

bool resources_ready() { return ready; }

bool resource_groups_see(const std::vector<int>& linux_cpus) {
  if (!ready || linux_cpus.empty()) {
    return false;
  }
  for (const int cpu : linux_cpus) {
    if (cpu < 0 || cpu >= static_cast<int>(known.size()) || known[static_cast<std::size_t>(cpu)] == 0) {
      return false;
    }
  }
  return true;
}

std::vector<ResourceGroupState> resource_states() {
  std::vector<ResourceGroupState> states;
  if (!ready) {
    return states;
  }
  states.reserve(groups.size());
  for (const auto& group : groups) {
    states.push_back(state_of(group));
  }
  return states;
}

std::optional<ResourceGroupState> least_loaded(ResourceLevel level) {
  if (!ready) {
    return std::nullopt;
  }
  return least_among(level, nullptr);
}

std::optional<ResourceGroupState> least_loaded_touching(ResourceLevel level, const std::vector<int>& linux_cpus) {
  if (!ready) {
    return std::nullopt;
  }
  return least_among(level, &linux_cpus);
}

std::optional<int> request_cpu(const std::vector<int>& exclude) {
  std::vector<int> skip = exclude;
  std::sort(skip.begin(), skip.end());
  skip.erase(std::unique(skip.begin(), skip.end()), skip.end());
  return balanced_cpu(skip);
}

namespace {

// Group id holding this CPU at a level, or -1.
int group_at(ResourceLevel level, int cpu) {
  for (int id = 0; id < static_cast<int>(groups.size()); ++id) {
    const auto& group = groups[static_cast<std::size_t>(id)];
    if (group.level == level && std::binary_search(group.linux_cpus.begin(), group.linux_cpus.end(), cpu)) {
      return id;
    }
  }
  return -1;
}

}  // namespace

std::optional<int> request_cpu_nearest(const std::vector<int>& held, const std::vector<int>& exclude) {
  if (!ready) {
    return std::nullopt;
  }
  if (held.empty()) {
    return request_cpu(exclude);
  }
  std::vector<int> skip = exclude;
  skip.insert(skip.end(), held.begin(), held.end());
  std::sort(skip.begin(), skip.end());
  skip.erase(std::unique(skip.begin(), skip.end()), skip.end());
  // Tier 0 core, 1 L3, 2 node, 3 socket, 4 anywhere.
  const ResourceLevel levels[] = {ResourceLevel::Core, ResourceLevel::L3, ResourceLevel::Node, ResourceLevel::Socket};
  std::vector<std::vector<int>> held_groups(4);
  for (int t = 0; t < 4; ++t) {
    for (const int cpu : held) {
      held_groups[static_cast<std::size_t>(t)].push_back(group_at(levels[t], cpu));
    }
  }
  int best_cpu = -1;
  int best_tier = 5;
  double best_load = 0;
  for (int cpu = 0; cpu < static_cast<int>(allocatable.size()); ++cpu) {
    if (!is_allocatable(cpu) || std::binary_search(skip.begin(), skip.end(), cpu)) {
      continue;
    }
    int tier = 4;
    for (int t = 0; t < 4; ++t) {
      const int id = group_at(levels[t], cpu);
      const auto& mine = held_groups[static_cast<std::size_t>(t)];
      if (id >= 0 && std::find(mine.begin(), mine.end(), id) != mine.end()) {
        tier = t;
        break;
      }
    }
    const int core = group_at(ResourceLevel::Core, cpu);
    const double load = core >= 0 ? state_of(groups[static_cast<std::size_t>(core)]).load : 0;
    // Ascending CPU scan: a later CPU only wins with a better tier or a lower load.
    if (best_cpu < 0 || tier < best_tier || (tier == best_tier && load < best_load)) {
      best_cpu = cpu;
      best_tier = tier;
      best_load = load;
    }
  }
  if (best_cpu < 0) {
    return std::nullopt;
  }
  return best_cpu;
}

std::optional<int> request_cpu_for(CpuPlacement placement, const std::vector<int>& held, const std::vector<int>& exclude) {
  if (placement == CpuPlacement::NearestFirst) {
    return request_cpu_nearest(held, exclude);
  }
  std::vector<int> all = exclude;
  all.insert(all.end(), held.begin(), held.end());
  return request_cpu(all);
}

std::vector<ResourceGroupInfo> resource_groups() {
  std::vector<ResourceGroupInfo> infos;
  if (!ready) {
    return infos;
  }
  infos.reserve(groups.size());
  for (const auto& group : groups) {
    ResourceGroupInfo info;
    info.level = group.level;
    info.name = group.name;
    if (group.parent >= 0 && group.parent < static_cast<int>(groups.size())) {
      info.parent = groups[static_cast<std::size_t>(group.parent)].name;
    }
    info.children.reserve(group.children.size());
    for (const int child : group.children) {
      if (child < 0 || child >= static_cast<int>(groups.size())) {
        continue;
      }
      info.children.push_back(groups[static_cast<std::size_t>(child)].name);
    }
    info.linux_cpus = group.linux_cpus;
    if (group.ring) {
      info.samples = group.ring->size();
      info.load = load_of(*group.ring);
    }
    infos.push_back(std::move(info));
  }
  return infos;
}

std::vector<int> allocatable_cpus() {
  std::vector<int> cpus;
  for (int cpu = 0; cpu < static_cast<int>(allocatable.size()); ++cpu) {
    if (allocatable[static_cast<std::size_t>(cpu)] != 0) {
      cpus.push_back(cpu);
    }
  }
  return cpus;
}

std::vector<ResourceRingView> resource_rings(std::size_t count) {
  std::vector<ResourceRingView> views;
  if (!ready) {
    return views;
  }
  views.reserve(groups.size());
  for (const auto& group : groups) {
    ResourceRingView view;
    view.level = group.level;
    view.name = group.name;
    if (group.ring) {
      view.counters = group.ring->names();
      view.stored = group.ring->size();
      const auto n = count == 0 ? view.stored : std::min(count, view.stored);
      view.samples = group.ring->recent(n);
    }
    views.push_back(std::move(view));
  }
  return views;
}
