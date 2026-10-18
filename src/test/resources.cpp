#include "resources.hpp"
#include "ring.hpp"

#include <unistd.h>

#include <algorithm>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void require(bool ok, const std::string& message) {
  if (!ok) {
    throw std::runtime_error(message);
  }
}

std::string trimmed(std::string_view value) {
  std::size_t begin = 0;
  while (begin < value.size() && (value[begin] == ' ' || value[begin] == '\n' || value[begin] == '\t')) {
    ++begin;
  }
  std::size_t end = value.size();
  while (end > begin &&
         (value[end - 1] == ' ' || value[end - 1] == '\n' || value[end - 1] == '\t' || value[end - 1] == '\r')) {
    --end;
  }
  return std::string(value.substr(begin, end - begin));
}

std::vector<int> parse_cpu_list(std::string_view text) {
  std::vector<int> cpus;
  std::size_t i = 0;
  auto read_cpu = [&](int& cpu) {
    if (i >= text.size() || text[i] < '0' || text[i] > '9') {
      return false;
    }
    int value = 0;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
      value = value * 10 + (text[i] - '0');
      ++i;
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

std::vector<int> online_cpus() {
  std::ifstream in("/sys/devices/system/cpu/online");
  require(static_cast<bool>(in), "cannot read /sys/devices/system/cpu/online");
  std::ostringstream text;
  text << in.rdbuf();
  const auto cpus = parse_cpu_list(trimmed(text.str()));
  require(!cpus.empty(), "no online cpus");
  return cpus;
}

bool contains(const std::vector<int>& cpus, int cpu) { return std::binary_search(cpus.begin(), cpus.end(), cpu); }

int held_in(const ResourceGroupInfo& group, const std::vector<int>& exclude) {
  int count = 0;
  for (const int cpu : group.linux_cpus) {
    if (contains(exclude, cpu)) {
      ++count;
    }
  }
  return count;
}

bool more_balanced(const ResourceGroupInfo& candidate, const ResourceGroupInfo& best,
                   const std::vector<int>& exclude) {
  const int candidate_held = held_in(candidate, exclude);
  const int best_held = held_in(best, exclude);
  if (candidate_held != best_held) {
    return candidate_held < best_held;
  }
  if (candidate.load != best.load) {
    return candidate.load < best.load;
  }
  const int candidate_cpu = candidate.linux_cpus.empty() ? 1 << 30 : candidate.linux_cpus.front();
  const int best_cpu = best.linux_cpus.empty() ? 1 << 30 : best.linux_cpus.front();
  if (candidate_cpu != best_cpu) {
    return candidate_cpu < best_cpu;
  }
  return candidate.name < best.name;
}

bool has_free(const ResourceGroupInfo& group, const std::vector<int>& pool, const std::vector<int>& exclude) {
  for (const int cpu : group.linux_cpus) {
    if (contains(pool, cpu) && !contains(exclude, cpu)) {
      return true;
    }
  }
  return false;
}

const ResourceGroupInfo* find_named(const std::vector<ResourceGroupInfo>& groups, const std::string& name) {
  for (const auto& group : groups) {
    if (group.name == name) {
      return &group;
    }
  }
  return nullptr;
}

// Same walk as balanced_cpu: even sibling counts, then lower load, then lower Linux CPU.
std::optional<int> expected_cpu(const std::vector<ResourceGroupInfo>& groups, const std::vector<int>& pool,
                                const std::vector<int>& exclude) {
  std::vector<const ResourceGroupInfo*> level;
  for (const auto& group : groups) {
    if (group.parent.empty()) {
      level.push_back(&group);
    }
  }
  const ResourceGroupInfo* chosen = nullptr;
  while (!level.empty()) {
    const ResourceGroupInfo* best = nullptr;
    for (const auto* group : level) {
      if (!has_free(*group, pool, exclude)) {
        continue;
      }
      if (best == nullptr || more_balanced(*group, *best, exclude)) {
        best = group;
      }
    }
    if (best == nullptr) {
      return std::nullopt;
    }
    chosen = best;
    level.clear();
    for (const auto& child : best->children) {
      if (const auto* next = find_named(groups, child)) {
        level.push_back(next);
      }
    }
  }
  if (chosen == nullptr) {
    return std::nullopt;
  }
  for (const int cpu : chosen->linux_cpus) {
    if (contains(pool, cpu) && !contains(exclude, cpu)) {
      return cpu;
    }
  }
  return std::nullopt;
}

const ResourceGroupState* group_of(const std::vector<ResourceGroupState>& states, ResourceLevel level, int cpu) {
  const ResourceGroupState* found = nullptr;
  for (const auto& state : states) {
    if (state.level != level || !contains(state.linux_cpus, cpu)) {
      continue;
    }
    require(found == nullptr, "cpu " + std::to_string(cpu) + " is in two groups at one level");
    found = &state;
  }
  return found;
}

std::string level_name(ResourceLevel level) { return resource_level_name(level); }

const ResourceGroupInfo* named(const std::vector<ResourceGroupInfo>& groups, const std::string& name) {
  const ResourceGroupInfo* found = nullptr;
  for (const auto& group : groups) {
    if (group.name != name) {
      continue;
    }
    require(found == nullptr, "duplicate group name " + name);
    found = &group;
  }
  return found;
}

ResourceLevel parent_level(ResourceLevel level) {
  switch (level) {
    case ResourceLevel::Node:
      return ResourceLevel::Socket;
    case ResourceLevel::L3:
      return ResourceLevel::Node;
    case ResourceLevel::Core:
      return ResourceLevel::L3;
    case ResourceLevel::Socket:
      return ResourceLevel::Socket;
  }
  return ResourceLevel::Socket;
}

void sleep_ms(int ms) {
  timespec delay{};
  delay.tv_sec = ms / 1000;
  delay.tv_nsec = static_cast<long>(ms % 1000) * 1000000L;
  ::nanosleep(&delay, nullptr);
}

}  // namespace


std::vector<ResourceGroupInfo> infos_now() { return resource_groups(); }

int count_level_groups(const std::vector<ResourceGroupInfo>& infos, ResourceLevel level) {
  int n = 0;
  for (const auto& g : infos) {
    n += g.level == level ? 1 : 0;
  }
  return n;
}

// nearest_first tier of cpu relative to held: 0 core, 1 l3, 2 node, 3 socket, 4 none.
int nearest_tier(const std::vector<ResourceGroupInfo>& infos, const std::vector<int>& held, int cpu) {
  const ResourceLevel levels[] = {ResourceLevel::Core, ResourceLevel::L3, ResourceLevel::Node, ResourceLevel::Socket};
  for (int t = 0; t < 4; ++t) {
    for (const auto& g : infos) {
      if (g.level != levels[t] || !contains(g.linux_cpus, cpu)) {
        continue;
      }
      for (const int h : held) {
        if (contains(g.linux_cpus, h)) {
          return t;
        }
      }
    }
  }
  return 4;
}

// The returned cpu is allocatable, outside held and exclude, and no candidate has a better tier.
void check_nearest(const std::vector<int>& pool, std::vector<int> held, std::vector<int> exclude,
                   const std::string& label) {
  std::sort(held.begin(), held.end());
  std::sort(exclude.begin(), exclude.end());
  const auto infos = resource_groups();
  const auto got = request_cpu_nearest(held, exclude);
  int best = 5;
  for (const int cpu : pool) {
    if (!contains(held, cpu) && !contains(exclude, cpu)) {
      best = std::min(best, nearest_tier(infos, held, cpu));
    }
  }
  if (best == 5) {
    require(!got, label + ": nothing free but a cpu was returned");
    return;
  }
  require(got.has_value(), label + ": no cpu returned");
  require(contains(pool, *got) && !contains(held, *got) && !contains(exclude, *got), label + ": cpu not allowed");
  require(nearest_tier(infos, held, *got) == best, label + ": cpu " + std::to_string(*got) + " is tier " +
                                                    std::to_string(nearest_tier(infos, held, *got)) + ", best " +
                                                    std::to_string(best));
}

int main() {
  try {
    const auto online = online_cpus();
    const int reserved = online.front();
    std::vector<int> pool;
    for (const int cpu : online) {
      if (online.size() > 1 && cpu == reserved) {
        continue;
      }
      pool.push_back(cpu);
    }

    init_resources(pool);
    require(resources_ready(), "resources were not built");
    require(resource_groups_see(online), "an online cpu is missing from the groups");
    require(!resource_groups_see({}), "an empty cpu list was accepted");
    require(!resource_groups_see({online.back() + 1000}), "an offline cpu was accepted");

    const auto states = resource_states();
    require(!states.empty(), "no resource group state");
    for (const auto level : {ResourceLevel::Socket, ResourceLevel::Node, ResourceLevel::L3, ResourceLevel::Core}) {
      const auto least = least_loaded(level);
      require(least.has_value(), "no " + level_name(level) + " group");
      require(!least->linux_cpus.empty(), level_name(level) + " group has no cpus");
      for (const int cpu : online) {
        require(group_of(states, level, cpu) != nullptr,
                "cpu " + std::to_string(cpu) + " has no " + level_name(level) + " group");
      }
    }

    const auto offered = resource_groups();
    const auto want = expected_cpu(offered, pool, {});
    const auto first = request_cpu({});
    const auto shared = request_cpu({});
    require(want.has_value(), "no allocatable cpu on any core");
    require(first && *first == *want, "request_cpu did not return the balanced cpu");
    require(shared && *shared == *first, "a second request did not share the least-loaded cpu");
    require(contains(pool, *first), "handed cpu is outside the pool");
    if (online.size() > 1) {
      require(*first != reserved, "reserved cpu was handed out");
    }

    std::vector<int> skip{*first};
    const auto next_offered = resource_groups();
    const auto next_want = expected_cpu(next_offered, pool, skip);
    const auto next = request_cpu(skip);
    if (pool.size() > 1) {
      require(next_want && next && *next == *next_want, "excluding the granted cpu did not move to the next balanced cpu");
      require(*next != *first, "excluding a cpu returned that same cpu");
    }
    require(!request_cpu(pool), "request_cpu returned a cpu that was excluded");

    // nearest_first.
    require(request_cpu_nearest({}, {}) == request_cpu({}), "nearest_first with an empty cpuset is least_loaded");
    for (const int start : pool) {
      check_nearest(pool, {start}, {}, "nearest from " + std::to_string(start));
    }
    {
      std::vector<int> held;
      for (std::size_t i = 0; i < 20 && i < pool.size(); ++i) {
        const auto next = request_cpu_nearest(held.empty() ? std::vector<int>{pool.front()} : held, {});
        check_nearest(pool, held.empty() ? std::vector<int>{pool.front()} : held, {}, "nearest grow step " + std::to_string(i));
        if (held.empty()) {
          held.push_back(pool.front());
        }
        if (next) {
          held.push_back(*next);
        }
      }
    }
    std::cout << "nearest_first generic ok" << std::endl;
    // dtn-01: EPYC 7301, 2 sockets, 8 nodes, 16 L3 of 2 cores, SMT sibling = cpu + 32.
    const bool dtn01 = online.size() == 64 && count_level_groups(infos_now(), ResourceLevel::L3) == 16 &&
                       count_level_groups(infos_now(), ResourceLevel::Node) == 8;
    if (dtn01) {
      auto in = [](int cpu, std::initializer_list<int> set) { return std::find(set.begin(), set.end(), cpu) != set.end(); };
      require(request_cpu_nearest({1}, {}) == 33, "dtn-01: sibling of 1 is 33");
      require(request_cpu_nearest({1, 33}, {}) == 32, "dtn-01: l3 of 1 (0 reserved) gives 32");
      const auto node0 = request_cpu_nearest({1, 32, 33}, {});
      require(node0 && in(*node0, {2, 3, 34, 35}), "dtn-01: full l3 moves within node 0");
      const auto l3 = request_cpu_nearest({8}, {40});
      require(l3 && in(*l3, {9, 41}), "dtn-01: excluded sibling falls to the l3");
      std::vector<int> socket0;
      for (int c = 0; c < 16; ++c) {
        socket0.push_back(c);
        socket0.push_back(c + 32);
      }
      const auto sock = request_cpu_nearest({1}, socket0);
      require(sock && *sock >= 16 && (*sock < 32 || *sock >= 48), "dtn-01: socket 0 excluded goes to socket 1");
      const auto same_socket = request_cpu_nearest({1, 2, 3, 32, 33, 34, 35}, {});
      require(same_socket && in(*same_socket, {4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47}) ,
              "dtn-01: full node 0 stays on socket 0");
      std::cout << "nearest_first dtn-01 ok" << std::endl;
    }

    const auto infos = resource_groups();
    require(infos.size() == states.size(), "resource_groups size");
    for (std::size_t i = 0; i < infos.size(); ++i) {
      require(infos[i].name == states[i].name, "group name order");
      require(infos[i].level == states[i].level, "group level");
      require(infos[i].linux_cpus == states[i].linux_cpus, "group cpus");
    }
    for (const auto& group : infos) {
      if (group.level == ResourceLevel::Core) {
        require(group.children.empty(), group.name + " core has children");
      } else {
        require(!group.children.empty(), group.name + " has no children");
      }
      if (group.parent.empty()) {
        require(group.level == ResourceLevel::Socket, group.name + " has no parent");
        continue;
      }
      const auto* parent = named(infos, group.parent);
      require(parent != nullptr, "missing parent " + group.parent);
      require(parent->level == parent_level(group.level), group.name + " parent level");
      bool linked = false;
      for (const auto& child : parent->children) {
        if (child == group.name) {
          linked = true;
        }
      }
      require(linked, "parent children omit " + group.name);
      for (const int cpu : group.linux_cpus) {
        require(contains(parent->linux_cpus, cpu), "cpu missing from parent of " + group.name);
      }
    }
    require(allocatable_cpus() == pool, "allocatable pool");

    sleep_ms(80);
    const auto rings = resource_rings(3);
    require(rings.size() == infos.size(), "ring view size");
    bool sampled = false;
    for (std::size_t i = 0; i < rings.size(); ++i) {
      require(rings[i].name == infos[i].name, "ring name order");
      require(rings[i].counters.size() == 8, rings[i].name + " counter count");
      require(rings[i].counters.back() == "busy", rings[i].name + " missing busy");
      require(rings[i].stored <= kRingSlots, rings[i].name + " stored past capacity");
      require(rings[i].samples.size() <= 3, rings[i].name + " returned more than asked");
      if (rings[i].samples.empty()) {
        continue;
      }
      sampled = true;
      require(rings[i].samples.back().nvalues == 8, rings[i].name + " sample width");
      require(rings[i].samples.back().timestamp_ns > 0, rings[i].name + " sample time");
    }
    require(sampled, "no resource ring samples");
    const auto full = resource_rings(0);
    require(full.size() == rings.size(), "full ring view size");
    for (std::size_t i = 0; i < full.size(); ++i) {
      require(full[i].samples.size() == full[i].stored, full[i].name + " did not return the stored ring");
      require(full[i].stored >= rings[i].samples.size(), full[i].name + " stored fewer than the short view");
    }

    const ResourceGroupInfo* core = nullptr;
    for (const auto& group : offered) {
      if (group.level == ResourceLevel::Core && contains(group.linux_cpus, *first)) {
        core = &group;
        break;
      }
    }
    require(core != nullptr, "granted cpu has no core");
    std::cout << "resources_test balanced " << core->name << " cpu " << *first;
    if (online.size() > 1) {
      std::cout << " reserved " << reserved;
    }
    std::cout << std::endl;
    // sample_loop is still running. Skip static teardown so it cannot race the rings.
    _exit(0);
  } catch (const std::exception& error) {
    std::cerr << error.what() << std::endl;
    _exit(1);
  }
}
