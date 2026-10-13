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
std::optional<std::string> kill_route(const std::string& request);
