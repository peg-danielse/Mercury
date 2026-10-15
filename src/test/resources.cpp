#include "resources.hpp"

#include <unistd.h>

#include <algorithm>
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

// One lap, or until the walk wraps onto a CPU already handed. The wrap call has
// already advanced the cursor past that CPU.
std::vector<int> take(std::size_t count) {
  std::vector<int> handed;
  handed.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    const auto cpu = request_cpu({});
    if (!cpu) {
      break;
    }
    if (std::find(handed.begin(), handed.end(), *cpu) != handed.end()) {
      break;
    }
    handed.push_back(*cpu);
  }
  return handed;
}

bool contains(const std::vector<int>& cpus, int cpu) { return std::binary_search(cpus.begin(), cpus.end(), cpu); }

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

bool on_full_core(const std::vector<ResourceGroupState>& states, int cpu) {
  const auto* core = group_of(states, ResourceLevel::Core, cpu);
  require(core != nullptr, "cpu " + std::to_string(cpu) + " has no core group");
  return core->load >= kCoreFull;
}

std::string level_name(ResourceLevel level) {
  switch (level) {
    case ResourceLevel::Socket:
      return "socket";
    case ResourceLevel::Node:
      return "node";
    case ResourceLevel::L3:
      return "l3";
    case ResourceLevel::Core:
      return "core";
  }
  return "resource";
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

    const auto first = take(pool.size());
    auto sorted = first;
    std::sort(sorted.begin(), sorted.end());
    if (online.size() > 1) {
      require(std::find(first.begin(), first.end(), reserved) == first.end(), "reserved cpu was handed out");
    }
    const auto after = resource_states();
    for (const int cpu : pool) {
      if (std::binary_search(sorted.begin(), sorted.end(), cpu)) {
        continue;
      }
      require(on_full_core(after, cpu), "cpu " + std::to_string(cpu) + " was withheld from a core under the load line");
    }
    for (const int cpu : sorted) {
      require(std::binary_search(pool.begin(), pool.end(), cpu),
              "handed cpu " + std::to_string(cpu) + " is outside the pool");
    }

    // A withheld CPU consumes a cursor step inside request_cpu, and the repeated CPU
    // that revealed the wrap was consumed too. The second lap starts just after it.
    const auto second = take(first.size());
    if (sorted.size() == pool.size()) {
      require(second == first, "second pass changed the handout order");
    }

    int socket_changes = 0;
    int l3_changes = 0;
    for (std::size_t i = 1; i < first.size(); ++i) {
      const auto* previous_socket = group_of(states, ResourceLevel::Socket, first[i - 1]);
      const auto* current_socket = group_of(states, ResourceLevel::Socket, first[i]);
      const auto* previous_l3 = group_of(states, ResourceLevel::L3, first[i - 1]);
      const auto* current_l3 = group_of(states, ResourceLevel::L3, first[i]);
      if (previous_socket->name != current_socket->name) {
        ++socket_changes;
      }
      if (previous_l3->name != current_l3->name) {
        ++l3_changes;
      }
    }
    std::vector<std::string> sockets_used;
    std::vector<std::string> l3_used;
    for (const int cpu : pool) {
      const auto socket = group_of(states, ResourceLevel::Socket, cpu)->name;
      const auto l3 = group_of(states, ResourceLevel::L3, cpu)->name;
      if (std::find(sockets_used.begin(), sockets_used.end(), socket) == sockets_used.end()) {
        sockets_used.push_back(socket);
      }
      if (std::find(l3_used.begin(), l3_used.end(), l3) == l3_used.end()) {
        l3_used.push_back(l3);
      }
    }
    if (sockets_used.size() > 1) {
      require(socket_changes > 0, "round-robin stayed on one socket");
      require(sorted != first, "round-robin handed cpus in numeric order");
    }
    if (l3_used.size() > 1) {
      require(l3_changes > 0, "round-robin stayed on one L3 domain");
    }

    require(!request_cpu(pool), "request_cpu returned a cpu that was excluded");
    if (sorted.size() == pool.size() && !first.empty()) {
      const auto again = request_cpu({});
      require(again && *again == first.front(), "cursor did not return to the first cpu");
      if (first.size() > 1) {
        const auto skipped = request_cpu({first.front()});
        require(skipped && *skipped == first[1], "excluding the first cpu did not yield the second");
      }
    }

    std::cout << "resources_test handed " << first.size() << " cpus";
    if (online.size() > 1) {
      std::cout << " reserved " << reserved;
    }
    std::cout << " sockets " << sockets_used.size() << " l3 " << l3_used.size() << std::endl;
    // sample_loop is still running. Skip static teardown so it cannot race the rings.
    _exit(0);
  } catch (const std::exception& error) {
    std::cerr << error.what() << std::endl;
    _exit(1);
  }
}
