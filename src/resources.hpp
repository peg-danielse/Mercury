#pragma once

#include "ring.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

// Socket, NUMA node, L3 domain, and physical core. A logical CPU sits in one group at each level.
enum class ResourceLevel { Socket, Node, L3, Core };

struct ResourceGroupState {
  ResourceLevel level = ResourceLevel::Core;
  std::string name;
  std::vector<int> linux_cpus;
  // Busy time over busy + idle + iowait. 0 until the ring has a sample.
  double load = 0;
  std::size_t samples = 0;
};

// Read the host topology, build one ring of /proc/stat metrics per group, and start sampling.
// allocatable_linux_cpus is the pool request_cpu may return. Groups still cover every online CPU.
void init_resources(std::vector<int> allocatable_linux_cpus);

bool resources_ready();

// True when every Linux CPU is a member of a discovered group.
bool resource_groups_see(const std::vector<int>& linux_cpus);

std::vector<ResourceGroupState> resource_states();

// Least loaded group at this level. Equal load keeps the group with the lowest Linux CPU.
std::optional<ResourceGroupState> least_loaded(ResourceLevel level);

// Same, among groups that contain one of these Linux CPUs.
std::optional<ResourceGroupState> least_loaded_touching(ResourceLevel level,
                                                        const std::vector<int>& linux_cpus);

// Allocatable Linux CPU on the least-loaded core that still has one outside exclude.
// exclude are Linux CPUs the caller already holds. Another daemon may already run on
// the CPU returned. Empty when every allocatable CPU is excluded.
std::optional<int> request_cpu(const std::vector<int>& exclude);

inline const char* resource_level_name(ResourceLevel level) {
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
  return "core";
}

// One group in the socket → node → L3 → core tree. parent is empty for a socket.
struct ResourceGroupInfo {
  ResourceLevel level = ResourceLevel::Core;
  std::string name;
  std::string parent;
  std::vector<std::string> children;
  std::vector<int> linux_cpus;
  double load = 0;
  std::size_t samples = 0;
};

std::vector<ResourceGroupInfo> resource_groups();

// Linux CPUs init_resources was allowed to hand out.
std::vector<int> allocatable_cpus();

// Newest samples from one resource-group ring, oldest first.
struct ResourceRingView {
  ResourceLevel level = ResourceLevel::Core;
  std::string name;
  std::size_t stored = 0;
  std::vector<std::string> counters;
  std::vector<Sample> samples;
};

// count is how many of the newest samples to copy. Zero copies every sample stored.
std::vector<ResourceRingView> resource_rings(std::size_t count);
