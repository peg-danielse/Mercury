#pragma once

#include <optional>
#include <string>
#include <vector>

void set_daemon_cpus(std::vector<int> cpus);

std::optional<std::string> stat_route(const std::string& request);
std::optional<std::string> start_route(const std::string& request);
std::optional<std::string> kill_route(const std::string& request);
