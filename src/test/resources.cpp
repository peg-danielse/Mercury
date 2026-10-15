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

bool quieter(const ResourceGroupState& candidate, const ResourceGroupState& best) {
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

// Lowest pool CPU on the least-loaded core that still has one outside exclude.
std::optional<int> expected_cpu(const std::vector<ResourceGroupState>& states, const std::vector<int>& pool,
                                const std::vector<int>& exclude) {
  const ResourceGroupState* best = nullptr;
  int chosen = -1;
  for (const auto& state : states) {
    if (state.level != ResourceLevel::Core) {
      continue;
    }
    int cpu = -1;
    for (const int member : state.linux_cpus) {
      if (!contains(pool, member) || contains(exclude, member)) {
        continue;
      }
      cpu = member;
      break;
    }
    if (cpu < 0) {
      continue;
    }
    if (best == nullptr || quieter(state, *best)) {
      best = &state;
      chosen = cpu;
    }
  }
  if (best == nullptr) {
    return std::nullopt;
  }
  return chosen;
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

    const auto offered = resource_states();
    const auto want = expected_cpu(offered, pool, {});
    const auto first = request_cpu({});
    const auto shared = request_cpu({});
    require(want.has_value(), "no allocatable cpu on any core");
    require(first && *first == *want, "request_cpu did not return the least-loaded core");
    require(shared && *shared == *first, "a second request did not share the least-loaded cpu");
    require(contains(pool, *first), "handed cpu is outside the pool");
    if (online.size() > 1) {
      require(*first != reserved, "reserved cpu was handed out");
    }

    std::vector<int> skip{*first};
    const auto next_want = expected_cpu(offered, pool, skip);
    const auto next = request_cpu(skip);
    if (pool.size() > 1) {
      require(next_want && next && *next == *next_want, "excluding the granted cpu did not move to the next least-loaded cpu");
      require(*next != *first, "excluding a cpu returned that same cpu");
    }
    require(!request_cpu(pool), "request_cpu returned a cpu that was excluded");

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

    const auto* core = group_of(offered, ResourceLevel::Core, *first);
    std::cout << "resources_test least " << core->name << " cpu " << *first;
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
