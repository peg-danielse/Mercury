#include "http.hpp"
#include "daemons.hpp"
#include "gnn.hpp"
#include "resources.hpp"

#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// kernel.numa_balancing is off while mercury runs, so the scheduler does not
// keep threads on a preferred node the cpuset no longer favours. The original
// value is kept in kNumaState (tmpfs: it is gone after a reboot, as is the
// sysctl) and restored on exit, SIGINT, SIGTERM, or by the next start.
constexpr const char* kNumaSysctl = "/proc/sys/kernel/numa_balancing";
constexpr const char* kNumaStateDir = "/run/mercury";
constexpr const char* kNumaState = "/run/mercury/numa_balancing.orig";
char g_numa_orig[32] = {0};
bool g_numa_owner = false;

std::string read_trimmed(const char* path) {
  std::ifstream in(path);
  std::string text;
  std::getline(in, text);
  return text;
}

bool write_text(const char* path, const std::string& text) {
  std::ofstream out(path, std::ios::trunc);
  out << text << "\n";
  out.flush();
  return static_cast<bool>(out);
}

// Async-signal-safe: open, write, close only.
void restore_numa_raw() {
  if (!g_numa_owner) {
    return;
  }
  const int fd = ::open(kNumaSysctl, O_WRONLY | O_CLOEXEC);
  if (fd >= 0) {
    const auto n = ::write(fd, g_numa_orig, std::strlen(g_numa_orig));
    (void)n;
    ::close(fd);
  }
  ::unlink(kNumaState);
  g_numa_owner = false;
}

void restore_numa_at_exit() {
  if (!g_numa_owner) {
    return;
  }
  const std::string orig = g_numa_orig;
  restore_numa_raw();
  std::cout << "numa_balancing restored to " << orig << " (now " << read_trimmed(kNumaSysctl) << ")" << std::endl;
}

void on_terminate(int sig) {
  kill_daemon_cgroups_raw();
  gnn_restore_all_affinity_raw();
  restore_numa_raw();
  ::_exit(128 + sig);
}

bool pid_alive(long pid) { return pid > 0 && (::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM); }

void setup_numa_balancing(bool keep) {
  const auto now = read_trimmed(kNumaSysctl);
  if (now.empty()) {
    std::cout << "numa_balancing unavailable" << std::endl;
    return;
  }
  // State file: "<original value> <owner pid>".
  std::string orig = now;
  {
    std::ifstream state(kNumaState);
    std::string value;
    long owner = 0;
    if (state >> value >> owner) {
      if (pid_alive(owner) && owner != static_cast<long>(::getpid())) {
        std::cout << "numa_balancing=" << now << " owned by running mercury pid " << owner << " (original " << value
                  << "); not changed" << std::endl;
        return;
      }
      std::cout << "numa_balancing stale state from pid " << owner << ": original " << value << ", restoring"
                << std::endl;
      write_text(kNumaSysctl, value);
      ::unlink(kNumaState);
      orig = value;
    }
  }
  if (keep) {
    std::cout << "numa_balancing kept at " << read_trimmed(kNumaSysctl) << " (MERCURY_NUMA_BALANCING=keep)" << std::endl;
    return;
  }
  ::mkdir(kNumaStateDir, 0755);
  if (!write_text(kNumaState, orig + " " + std::to_string(::getpid()))) {
    std::cout << "numa_balancing state file " << kNumaState << " not writable; left at " << orig << std::endl;
    return;
  }
  std::snprintf(g_numa_orig, sizeof(g_numa_orig), "%s\n", orig.c_str());
  g_numa_owner = true;
  std::atexit(restore_numa_at_exit);
  struct sigaction action {};
  action.sa_handler = on_terminate;
  ::sigemptyset(&action.sa_mask);
  ::sigaction(SIGINT, &action, nullptr);
  ::sigaction(SIGTERM, &action, nullptr);
  if (!write_text(kNumaSysctl, "0")) {
    std::cout << "numa_balancing write failed; left at " << orig << std::endl;
    restore_numa_raw();
    return;
  }
  std::cout << "numa_balancing " << orig << " -> " << read_trimmed(kNumaSysctl) << " (original saved in " << kNumaState
            << ")" << std::endl;
}


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
  // --numa-balancing=keep (or MERCURY_NUMA_BALANCING=keep) leaves the sysctl alone.
  bool keep_numa = false;
  if (const char* env = std::getenv("MERCURY_NUMA_BALANCING"); env != nullptr && std::string(env) == "keep") {
    keep_numa = true;
  }
  {
    int out = 1;
    for (int i = 1; i < argc; ++i) {
      if (std::string(argv[i]) == "--numa-balancing=keep") {
        keep_numa = true;
      } else {
        argv[out++] = argv[i];
      }
    }
    argc = out;
  }
  if (argc < 2) {
    std::cerr << "usage: mercury [--numa-balancing=keep] <port> [cpu...]\n";
    return 1;
  }
  try {
    setup_numa_balancing(keep_numa);
    // gnn threads pinned by Mercury get the cpuset-wide mask back on exit, SIGINT and SIGTERM
    // (whether or not this instance owns numa_balancing). SIGKILL cannot be caught.
    std::atexit(gnn_restore_all_affinity_raw);
    {
      struct sigaction action {};
      action.sa_handler = on_terminate;
      ::sigemptyset(&action.sa_mask);
      ::sigaction(SIGINT, &action, nullptr);
      ::sigaction(SIGTERM, &action, nullptr);
    }
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
    serve(static_cast<std::uint16_t>(port_number), "mercury", route);
  } catch (const std::exception& error) {
    std::cerr << error.what() << std::endl;
    return 1;  // atexit restores numa_balancing
  }
}
