#pragma once

#include <optional>
#include <string>
#include <vector>

void set_daemon_cpus(std::vector<int> cpus);
void set_mercury_cpu(int cpu);
void raise_nofile();
// Directory of the ring archives: MERCURY_RINGS, else ./rings.
std::string ring_archive_dir();

std::optional<std::string> stat_route(const std::string& request);
std::optional<std::string> start_route(const std::string& request);
std::optional<std::string> migrate_route(const std::string& request);
std::optional<std::string> cpus_route(const std::string& request);
std::optional<std::string> scale_route(const std::string& request);
// POST /throttle: cut a daemon's CPU time while the whole host is busy (SPEC.md "throttle").
std::optional<std::string> throttle_route(const std::string& request);
// GET/POST /degrade: shed optional routes at the application's entry while the system is busy (SPEC.md "degrade").
std::optional<std::string> degrade_route(const std::string& request);
// GET/POST /pool: the l3 pool autoscaling hands CPUs out from (SPEC.md "l3 pool").
std::optional<std::string> pool_route(const std::string& request);
// MERCURY_POOL turns the l3 pool on at startup; call after init_resources.
void init_pool_from_env();
// POST /shift: move a daemon off an L3 domain above the line (SPEC.md "shift").
std::optional<std::string> shift_route(const std::string& request);
// GET /llm/state and POST /llm/act: the placement view and moves for an LLM controller (SPEC.md "LLM interface").
std::optional<std::string> llm_state_route(const std::string& request);
std::optional<std::string> llm_act_route(const std::string& request);
std::optional<std::string> map_route(const std::string& request);
std::optional<std::string> rings_route(const std::string& request);
std::optional<std::string> kill_route(const std::string& request);

// Async-signal-safe: writes cgroup.kill in every daemon cgroup this mercury created, then removes
// them (polling up to 2 s for the tasks to go). Called from Mercury's SIGINT/SIGTERM handler.
void kill_daemon_cgroups_raw();
