#include "http.hpp"
#include "daemons.hpp"

#include <sched.h>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void pin_to(int cpu) {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu - 1, &set);
  if (::sched_setaffinity(0, sizeof(set), &set) != 0) {
    throw std::runtime_error("sched_setaffinity failed");
  }
}

int parse_cpu(const char* text) {
  try {
    std::size_t used = 0;
    const int cpu = std::stoi(text, &used);
    if (text[used] != '\0' || cpu < 1 || cpu > CPU_SETSIZE) {
      throw std::runtime_error("invalid cpu");
    }
    return cpu;
  } catch (const std::runtime_error&) {
    throw;
  } catch (const std::exception&) {
    throw std::runtime_error("invalid cpu");
  }
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
  if (auto response = kill_route(request)) {
    return *response;
  }
  return http_response(404, "Not Found", "not found");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: mercury <port> <cpu> [cpu...]\n";
    return 1;
  }
  try {
    const int port_number = std::stoi(argv[1]);
    if (port_number < 1 || port_number > 65535) {
      throw std::runtime_error("invalid port");
    }
    std::vector<int> cpus;
    for (int i = 2; i < argc; ++i) {
      const int cpu = parse_cpu(argv[i]);
      if (std::find(cpus.begin(), cpus.end(), cpu) != cpus.end()) {
        throw std::runtime_error("duplicate cpu");
      }
      cpus.push_back(cpu);
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
    std::cout << "pinned to cpu " << self << ", daemons on";
    for (const int cpu : daemon_cpus) {
      std::cout << ' ' << cpu;
    }
    std::cout << std::endl;
    set_daemon_cpus(std::move(daemon_cpus));
    serve(static_cast<std::uint16_t>(port_number), "mercury", route);
  } catch (const std::exception& error) {
    std::cerr << error.what() << std::endl;
    return 1;
  }
}
