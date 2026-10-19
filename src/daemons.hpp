#pragma once

#include <optional>
#include <string>
#include <vector>

void set_daemon_cpus(std::vector<int> cpus);
void set_mercury_cpu(int cpu);
void raise_nofile();

std::optional<std::string> stat_route(const std::string& request);
std::optional<std::string> start_route(const std::string& request);
std::optional<std::string> migrate_route(const std::string& request);
std::optional<std::string> cpus_route(const std::string& request);
std::optional<std::string> scale_route(const std::string& request);
std::optional<std::string> policies_route(const std::string& request);
std::optional<std::string> map_route(const std::string& request);
std::optional<std::string> rings_route(const std::string& request);
std::optional<std::string> kill_route(const std::string& request);

// Async-signal-safe: writes cgroup.kill in every daemon cgroup this mercury created, then removes
// them (polling up to 2 s for the tasks to go). Called from Mercury's SIGINT/SIGTERM handler.
void kill_daemon_cgroups_raw();

// Policies from policies.md: shift, the grant (restrict/widen), and degrade. Wakes once a second.
// init_resources detaches it beside sample_loop (see set_policy_loop).
void policy_loop();
