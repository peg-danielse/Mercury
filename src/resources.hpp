#pragma once

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

// Next allocatable Linux CPU. The walk is round-robin across socket, node, L3, then core.
// exclude are Linux CPUs the caller already holds. Empty when none remain.
std::optional<int> request_cpu(const std::vector<int>& exclude);
