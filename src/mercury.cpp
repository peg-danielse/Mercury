#include "http.hpp"
#include "daemons.hpp"
#include "resources.hpp"

#include <sched.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void pin_to(int cpu) {
  if (cpu < 0 || cpu >= CPU_SETSIZE) {
    throw std::runtime_error("invalid cpu");
  }
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  if (::sched_setaffinity(0, sizeof(set), &set) != 0) {
    throw std::runtime_error("sched_setaffinity failed");
  }
}

int parse_cpu(const char* text) {
  try {
    std::size_t used = 0;
    const int cpu = std::stoi(text, &used);
    if (text[used] != '\0' || cpu < 0 || cpu >= CPU_SETSIZE) {
      throw std::runtime_error("invalid cpu");
    }
    return cpu;
  } catch (const std::runtime_error&) {
    throw;
  } catch (const std::exception&) {
    throw std::runtime_error("invalid cpu");
  }
}

std::vector<int> online_cpus() {
  std::ifstream in("/sys/devices/system/cpu/online");
  if (!in) {
    throw std::runtime_error("cannot read online cpus");
  }
  std::string text;
  std::getline(in, text);
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '\t')) {
    text.pop_back();
  }
  std::size_t i = 0;
  while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) {
    ++i;
  }
  if (i >= text.size()) {
    throw std::runtime_error("no online cpus");
  }
  std::vector<int> cpus;
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
        throw std::runtime_error("invalid online cpus");
      }
      ++i;
    }
    int first = 0;
    if (!read_cpu(first)) {
      throw std::runtime_error("invalid online cpus");
    }
    int last = first;
    if (i < text.size() && text[i] == '-') {
      ++i;
      if (!read_cpu(last) || last < first) {
        throw std::runtime_error("invalid online cpus");
      }
    }
    for (int cpu = first; cpu <= last; ++cpu) {
      if (std::find(cpus.begin(), cpus.end(), cpu) != cpus.end()) {
        throw std::runtime_error("duplicate cpu");
      }
      cpus.push_back(cpu);
    }
  }
  if (cpus.empty() || std::find(cpus.begin(), cpus.end(), 0) == cpus.end()) {
    throw std::runtime_error("cpu 0 is not online");
  }
  return cpus;
}

std::string route(const std::string& request) {
  if (auto response = ping_route(request)) {
    return *response;
  }
  if (auto response = stat_route(request)) {
    return *response;
  }
  if (auto response = start_route(request)) {
    return *response;
  }
  if (auto response = migrate_route(request)) {
    return *response;
  }
  if (auto response = cpus_route(request)) {
    return *response;
  }
  if (auto response = scale_route(request)) {
    return *response;
  }
  if (auto response = policies_route(request)) {
    return *response;
  }
  if (auto response = map_route(request)) {
    return *response;
  }
  if (auto response = rings_route(request)) {
    return *response;
  }
  if (auto response = kill_route(request)) {
    return *response;
  }
  return http_response(404, "Not Found", "not found");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: mercury <port> [cpu...]\n";
    return 1;
  }
  try {
    const int port_number = std::stoi(argv[1]);
    if (port_number < 1 || port_number > 65535) {
      throw std::runtime_error("invalid port");
    }
    std::vector<int> cpus;
    if (argc == 2) {
      cpus = online_cpus();
    } else {
      for (int i = 2; i < argc; ++i) {
        const int cpu = parse_cpu(argv[i]);
        if (std::find(cpus.begin(), cpus.end(), cpu) != cpus.end()) {
          throw std::runtime_error("duplicate cpu");
        }
        cpus.push_back(cpu);
      }
    }
    const int self = *std::min_element(cpus.begin(), cpus.end());
    pin_to(self);
    std::vector<int> daemon_cpus;
    for (const int cpu : cpus) {
      if (cpu != self) {
        daemon_cpus.push_back(cpu);
      }
    }
    std::sort(daemon_cpus.begin(), daemon_cpus.end());
    raise_nofile();
    set_mercury_cpu(self);
    std::cout << "pinned to cpu " << self << ", daemons on";
    for (const int cpu : daemon_cpus) {
      std::cout << ' ' << cpu;
    }
    std::cout << std::endl;
    set_daemon_cpus(daemon_cpus);
    init_resources(std::move(daemon_cpus));
    start_policy_loop();
    serve(static_cast<std::uint16_t>(port_number), "mercury", route);
  } catch (const std::exception& error) {
    std::cerr << error.what() << std::endl;
    return 1;
  }
}
