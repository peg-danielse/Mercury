#include "daemons.hpp"

#include "gnn.hpp"

#include "http.hpp"
#include "policy.hpp"
#include "resources.hpp"
#include "ring.hpp"
#include "ring_archive.hpp"
#include "cpustat.hpp"

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <limits>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

void raise_nofile() {
  rlimit limit{};
  if (::getrlimit(RLIMIT_NOFILE, &limit) != 0) {
    return;
  }
  auto target = limit.rlim_max;
  if (target == RLIM_INFINITY) {
    target = 65536;
  }
  if (limit.rlim_cur >= target) {
    return;
  }
  limit.rlim_cur = target;
  ::setrlimit(RLIMIT_NOFILE, &limit);
}

namespace {

constexpr std::string_view kJson = "application/json";

std::vector<int> daemon_cpus;
int mercury_linux_cpu = -1;

// arm_daemon attaches saturate (min 1, max 16) to every daemon it starts. /scale updates it.
struct ScaleState {
  bool enabled = false;
  int min_cores = 1;
  int max_cores = kDefaultMaxCores;
  std::shared_ptr<Policy> policy;
  std::int64_t changed_at_ns = 0;
  // CPU choice for add_core.
  CpuPlacement placement = CpuPlacement::LeastLoaded;
  // gnn policy (SPEC.md): monitor_daemon runs a GnnController every gnn_tick_ms instead of saturate.
  bool gnn = false;
  int gnn_tick_ms = 10;
  bool gnn_pin = true;  // false: gnn only sizes the cpuset; every thread keeps the cpuset-wide mask
  // throttle (SPEC.md): set by /throttle, independent of the policy above. active while cpu.max is cut;
  // throttle_at_ns is the last attach, cut, or release.
  bool throttle = false;
  bool throttle_active = false;
  double throttle_above = kThrottleAbove;
  double throttle_cut = kThrottleCut;
  int throttle_window_ms = kThrottleWindowMs;
  std::int64_t throttle_at_ns = 0;
  // shift (SPEC.md): set by /shift, independent of the policies above. shift_at_ns is the last attach or move.
  bool shift = false;
  double shift_above = kShiftAbove;
  int shift_window_ms = kShiftWindowMs;
  std::int64_t shift_at_ns = 0;
};

}  // namespace
// Defined beside scale_json (outside the anonymous namespace).
std::shared_ptr<const GnnWeights> gnn_weights(std::string& error);
namespace {

struct Daemon {
  std::string daemon_id;
  std::string exec_path;
  pid_t pid = -1;
  bool running = false;
  int exit_code = -1;
  std::string log;
  std::optional<std::int64_t> finished;
  ScaleState scale;
  int cpu = 0;
  int listen_fd = -1;
  bool counters_done = false;
  // Extra environment for the exec'd process. Empty keeps the inherited environment.
  std::vector<std::pair<std::string, std::string>> env;  // The cgroup ring monitor_daemon fills; /llm/state reads the daemon's CPU use from it.
  std::shared_ptr<Ring> ring;
};

std::mutex daemons_mu;
std::condition_variable daemons_cv;
std::vector<Daemon> daemons;

// Placement events for /llm/state's cursor: what changed a cpuset, a cut, or a policy, numbered from 1.
// The newest kMaxEvents are kept; a cursor older than that is told how many it missed.
struct PlacementEvent {
  std::uint64_t seq = 0;
  std::int64_t t_ns = 0;
  std::string daemon_id;
  std::string kind;
  std::string detail;
};
constexpr std::size_t kMaxEvents = 2000;
std::mutex events_mu;
std::deque<PlacementEvent> events;
std::uint64_t next_event = 1;

std::int64_t event_clock_ns() {
  timespec now{};
  ::clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<std::int64_t>(now.tv_sec) * 1000000000LL + now.tv_nsec;
}

void note_event(const std::string& daemon_id, const std::string& kind, const std::string& detail) {
  std::lock_guard lock(events_mu);
  events.push_back({next_event++, event_clock_ns(), daemon_id, kind, detail});
  if (events.size() > kMaxEvents) {
    events.pop_front();
  }
}

// l3 pool (SPEC.md "l3 pool"): when enabled, add_core hands out only CPUs in active, except to daemons whose
// id starts with an exempt prefix (the load generator). Set by POST /pool or MERCURY_POOL; grown by pool_loop.
struct PoolState {
  bool enabled = false;
  std::vector<int> active;
  std::size_t start = kPoolStartCpus;
  double up = kPoolAddLoad;
  int window_ms = kPoolWindowMs;
  std::vector<std::string> exempt;
  double down = kPoolRemoveLoad;
  int down_window_ms = kPoolRemoveWindowMs;
  // l3 grows and shrinks the pool; fixed keeps its start size (the baseline the other systems run on).
  bool grow = true;
  double pressure_up = kPoolAddPressure;
  // The shift system with a pool: swap a pooled domain loaded from outside Mercury for a clean one.
  bool swap = false;
  // Budget: the pool never grows past this many CPUs (0: no limit; the LLM's runs use 42).
  std::size_t max = 0;
  double swap_share = kPoolSwapForeign;
  // The CPUs each growth step added, oldest first: scale-in removes the last one.
  std::vector<std::vector<int>> grown;
  std::int64_t changed_at_ns = 0;
};
std::mutex pool_mu;
PoolState pool;

bool pool_exempt(const PoolState& state, const std::string& daemon_id) {
  return std::any_of(state.exempt.begin(), state.exempt.end(),
                     [&](const std::string& prefix) { return !prefix.empty() && daemon_id.rfind(prefix, 0) == 0; });
}

// Allocatable CPUs this daemon must not be given: those outside the active pool, or for an exempt daemon (the
// load generator) the pool itself, so it never competes with the application for pool CPUs. Empty when the pool
// is off, or when an exempt daemon would have nothing left.
std::vector<int> pool_exclude(const std::string& daemon_id) {
  std::lock_guard lock(pool_mu);
  std::vector<int> out;
  if (!pool.enabled) {
    return out;
  }
  const bool exempt = pool_exempt(pool, daemon_id);
  const auto all = allocatable_cpus();
  for (const int cpu : all) {
    const bool pooled = std::find(pool.active.begin(), pool.active.end(), cpu) != pool.active.end();
    if (pooled == exempt) {
      out.push_back(cpu);
    }
  }
  if (exempt && out.size() >= all.size()) {
    out.clear();
  }
  return out;
}

bool pool_exempt_daemon(const std::string& daemon_id) {
  std::lock_guard lock(pool_mu);
  return pool.enabled && pool_exempt(pool, daemon_id);
}

// A load as a whole percentage, for event details.
std::string pct(double load) { return std::to_string(static_cast<int>(load * 100 + 0.5)) + "%"; }

std::int64_t now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

Daemon* find_daemon(const std::string& daemon_id) {
  for (auto& daemon : daemons) {
    if (daemon.daemon_id == daemon_id) {
      return &daemon;
    }
  }
  return nullptr;
}

bool daemon_running(const std::string& daemon_id, pid_t pid) {
  std::lock_guard lock(daemons_mu);
  const auto* daemon = find_daemon(daemon_id);
  return daemon != nullptr && daemon->pid == pid && daemon->running;
}

std::string ok_json(bool ok) { return ok ? R"({"ok":true})" : R"({"ok":false})"; }

std::string kill_json(std::int64_t finished) {
  return "{\"ok\":true,\"finished\":" + std::to_string(finished) + "}";
}

std::string request_body(const std::string& request) {
  const auto header_end = request.find("\r\n\r\n");
  if (header_end == std::string::npos) {
    return {};
  }
  return request.substr(header_end + 4);
}

bool read_json_string(const std::string& json, std::size_t open, std::string& out, std::size_t& close) {
  if (open >= json.size() || json[open] != '"') {
    return false;
  }
  out.clear();
  for (std::size_t i = open + 1; i < json.size(); ++i) {
    if (json[i] == '\\') {
      if (i + 1 >= json.size()) {
        return false;
      }
      const char next = json[++i];
      switch (next) {
        case '"':
        case '\\':
        case '/':
          out.push_back(next);
          break;
        case 'b':
          out.push_back('\b');
          break;
        case 'f':
          out.push_back('\f');
          break;
        case 'n':
          out.push_back('\n');
          break;
        case 'r':
          out.push_back('\r');
          break;
        case 't':
          out.push_back('\t');
          break;
        default:
          return false;
      }
      continue;
    }
    if (json[i] == '"') {
      close = i;
      return true;
    }
    if (static_cast<unsigned char>(json[i]) < 0x20) {
      return false;
    }
    out.push_back(json[i]);
  }
  return false;
}

// Index of the colon for a key on the outermost object. Keys inside nested objects are ignored.
std::optional<std::size_t> top_level_colon(const std::string& json, std::string_view key) {
  int depth = 0;
  for (std::size_t i = 0; i < json.size();) {
    const char c = json[i];
    if (c == '"') {
      std::string text;
      std::size_t close = 0;
      if (!read_json_string(json, i, text, close)) {
        return std::nullopt;
      }
      std::size_t after = close + 1;
      while (after < json.size() && std::isspace(static_cast<unsigned char>(json[after])) != 0) {
        ++after;
      }
      if (depth == 1 && text == key && after < json.size() && json[after] == ':') {
        return after;
      }
      i = close + 1;
      continue;
    }
    if (c == '{' || c == '[') {
      ++depth;
    } else if (c == '}' || c == ']') {
      if (depth == 0) {
        return std::nullopt;
      }
      --depth;
    }
    ++i;
  }
  return std::nullopt;
}

std::optional<std::string> json_value(const std::string& json, std::string_view key) {
  const auto colon = top_level_colon(json, key);
  if (!colon) {
    return std::nullopt;
  }
  auto at = *colon + 1;
  while (at < json.size() && std::isspace(static_cast<unsigned char>(json[at])) != 0) {
    ++at;
  }
  if (at >= json.size()) {
    return std::nullopt;
  }
  if (json[at] == '"') {
    std::string value;
    std::size_t close = 0;
    if (!read_json_string(json, at, value, close)) {
      return std::nullopt;
    }
    return value;
  }
  const auto end = json.find_first_of(",}", at);
  auto token = json.substr(at, end == std::string::npos ? std::string::npos : end - at);
  while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back())) != 0) {
    token.pop_back();
  }
  return token;
}

bool valid_env_name(const std::string& name) {
  if (name.empty() || (std::isalpha(static_cast<unsigned char>(name.front())) == 0 && name.front() != '_')) {
    return false;
  }
  for (const unsigned char c : name) {
    if (std::isalnum(c) == 0 && c != '_') {
      return false;
    }
  }
  return true;
}

struct EnvRequest {
  bool present = false;
  bool ok = true;
  std::vector<std::pair<std::string, std::string>> vars;
};

// "env" is an object of string values. Absent means the daemon inherits Mercury's environment unchanged.
EnvRequest parse_env(const std::string& json) {
  EnvRequest result;
  const auto colon = top_level_colon(json, "env");
  if (!colon) {
    return result;
  }
  result.present = true;
  std::size_t i = *colon + 1;
  while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i])) != 0) {
    ++i;
  }
  if (i >= json.size() || json[i] != '{') {
    result.ok = false;
    return result;
  }
  ++i;
  while (true) {
    while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i])) != 0) {
      ++i;
    }
    if (i < json.size() && json[i] == '}') {
      return result;
    }
    std::string name;
    std::size_t close = 0;
    if (!read_json_string(json, i, name, close) || !valid_env_name(name)) {
      result.ok = false;
      return result;
    }
    i = close + 1;
    while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i])) != 0) {
      ++i;
    }
    if (i >= json.size() || json[i] != ':') {
      result.ok = false;
      return result;
    }
    ++i;
    while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i])) != 0) {
      ++i;
    }
    std::string value;
    if (!read_json_string(json, i, value, close)) {
      result.ok = false;
      return result;
    }
    i = close + 1;
    bool replaced = false;
    for (auto& entry : result.vars) {
      if (entry.first == name) {
        entry.second = std::move(value);
        replaced = true;
        break;
      }
    }
    if (!replaced) {
      result.vars.emplace_back(std::move(name), std::move(value));
    }
    while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i])) != 0) {
      ++i;
    }
    if (i < json.size() && json[i] == ',') {
      ++i;
      continue;
    }
    if (i < json.size() && json[i] == '}') {
      return result;
    }
    result.ok = false;
    return result;
  }
}

std::string read_pipes(int out_fd, int err_fd) {
  std::string log;
  pollfd fds[2]{};
  fds[0].fd = out_fd;
  fds[0].events = POLLIN;
  fds[1].fd = err_fd;
  fds[1].events = POLLIN;
  int open_count = 2;
  while (open_count > 0) {
    if (poll(fds, 2, -1) < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    for (auto& fd : fds) {
      if (fd.fd < 0 || (fd.revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
        continue;
      }
      char buffer[1024];
      const auto n = ::read(fd.fd, buffer, sizeof(buffer));
      if (n > 0) {
        log.append(buffer, static_cast<std::size_t>(n));
      } else {
        ::close(fd.fd);
        fd.fd = -1;
        --open_count;
      }
    }
  }
  return log;
}

std::string read_file(const std::string& path) {
  std::ifstream in(path);
  if (!in) {
    return {};
  }
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

std::string rings_dir() {
  if (const char* env = std::getenv("MERCURY_RINGS")) {
    if (env[0] != '\0') {
      return env;
    }
  }
  return "rings";
}

std::string archive_name(const std::string& daemon_id);

// <rings>/<daemon>.policy: the file the degrade system writes and the daemon (the hotel frontend) polls.
std::string policy_path(const std::string& daemon_id) { return rings_dir() + "/" + archive_name(daemon_id) + ".policy"; }

std::string archive_name(const std::string& daemon_id) {
  std::string safe;
  for (const unsigned char c : daemon_id) {
    if (std::isalnum(c) != 0 || c == '.' || c == '_' || c == '-') {
      safe.push_back(static_cast<char>(c));
    } else {
      safe.push_back('_');
    }
  }
  if (safe.empty()) {
    safe = "daemon";
  }
  return safe;
}

std::string trimmed(std::string_view value) {
  std::size_t begin = 0;
  while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
    ++begin;
  }
  std::size_t end = value.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
    --end;
  }
  return std::string(value.substr(begin, end - begin));
}

// cpuset list: "4", "0-4", or a comma-separated mix such as "0-2,5".
std::optional<std::vector<int>> parse_cpuset(std::string_view text) {
  std::vector<int> cpus;
  if (text.empty()) {
    return std::nullopt;
  }
  std::size_t i = 0;
  auto read_cpu = [&](int& cpu) {
    if (i >= text.size() || std::isdigit(static_cast<unsigned char>(text[i])) == 0) {
      return false;
    }
    if (text[i] == '0' && i + 1 < text.size() && std::isdigit(static_cast<unsigned char>(text[i + 1])) != 0) {
      return false;
    }
    int value = 0;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])) != 0) {
      const int digit = text[i] - '0';
      if (value > (CPU_SETSIZE - digit) / 10) {
        return false;
      }
      value = value * 10 + digit;
      ++i;
    }
    if (value < 0 || value >= CPU_SETSIZE) {
      return false;
    }
    cpu = value;
    return true;
  };
  while (i < text.size()) {
    if (!cpus.empty()) {
      if (text[i] != ',') {
        return std::nullopt;
      }
      ++i;
    }
    int first = 0;
    if (!read_cpu(first)) {
      return std::nullopt;
    }
    int last = first;
    if (i < text.size() && text[i] == '-') {
      ++i;
      if (!read_cpu(last) || last < first) {
        return std::nullopt;
      }
    }
    for (int cpu = first; cpu <= last; ++cpu) {
      cpus.push_back(cpu);
    }
  }
  if (cpus.empty()) {
    return std::nullopt;
  }
  std::sort(cpus.begin(), cpus.end());
  cpus.erase(std::unique(cpus.begin(), cpus.end()), cpus.end());
  return cpus;
}

bool controller_enabled(const std::string& text, std::string_view name) {
  std::size_t at = 0;
  while (at < text.size()) {
    while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at])) != 0) {
      ++at;
    }
    if (at >= text.size()) {
      break;
    }
    std::size_t end = at;
    while (end < text.size() && std::isspace(static_cast<unsigned char>(text[end])) == 0) {
      ++end;
    }
    if (text.compare(at, end - at, name) == 0) {
      return true;
    }
    at = end;
  }
  return false;
}

bool write_sysfs(const std::string& path, std::string_view value) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    return false;
  }
  std::string data(value);
  if (data.empty() || data.back() != '\n') {
    data.push_back('\n');
  }
  const auto n = ::write(fd, data.data(), data.size());
  const int saved = errno;
  ::close(fd);
  errno = saved;
  return n == static_cast<ssize_t>(data.size());
}

bool ensure_controller(const std::string& subtree_control, const std::string& name) {
  if (controller_enabled(read_file(subtree_control), name)) {
    return true;
  }
  if (write_sysfs(subtree_control, "+" + name)) {
    return true;
  }
  return controller_enabled(read_file(subtree_control), name);
}

bool ensure_cpuset_controller(const std::string& subtree_control) { return ensure_controller(subtree_control, "cpuset"); }

// cpuset.cpus exists on the daemon cgroup only when cpuset is enabled on both ancestors.
std::optional<std::string> ensure_daemon_cpuset(const std::string& daemon_id) {
  const std::string parent = "/sys/fs/cgroup/mercury";
  if (::mkdir(parent.c_str(), 0755) != 0 && errno != EEXIST) {
    return std::nullopt;
  }
  if (!ensure_cpuset_controller("/sys/fs/cgroup/cgroup.subtree_control")) {
    return std::nullopt;
  }
  if (!ensure_cpuset_controller(parent + "/cgroup.subtree_control")) {
    return std::nullopt;
  }
  const std::string dir = parent + "/" + daemon_id;
  if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) {
    return std::nullopt;
  }
  return dir;
}

bool cpuset_applied(const std::string& dir, const std::vector<int>& want) {
  const auto configured = parse_cpuset(trimmed(read_file(dir + "/cpuset.cpus")));
  const auto effective = parse_cpuset(trimmed(read_file(dir + "/cpuset.cpus.effective")));
  return configured && effective && *configured == want && *effective == want;
}

bool pin_pid(pid_t pid, int linux_cpu) {
  cpu_set_t set;
  CPU_ZERO(&set);
  if (linux_cpu < 0 || linux_cpu >= CPU_SETSIZE) {
    return false;
  }
  CPU_SET(linux_cpu, &set);
  return ::sched_setaffinity(pid, sizeof(set), &set) == 0;
}

bool widen_to_online() {
  const auto online = parse_cpuset(trimmed(read_file("/sys/devices/system/cpu/online")));
  if (!online || online->empty()) {
    return false;
  }
  cpu_set_t set;
  CPU_ZERO(&set);
  for (const int cpu : *online) {
    if (cpu >= 0 && cpu < CPU_SETSIZE && cpu != mercury_linux_cpu) {
      CPU_SET(cpu, &set);
    }
  }
  return ::sched_setaffinity(0, sizeof(set), &set) == 0;
}

struct Placement {
  std::string dir;
  int pin_linux = -1;
};

std::string node_list(const std::vector<int>& nodes) {
  std::string out;
  for (const int node : nodes) {
    if (!out.empty()) {
      out += ",";
    }
    out += std::to_string(node);
  }
  return out;
}

// cpuset.mems follows cpuset.cpus: the NUMA nodes whose CPUs the cpuset uses.
// Order keeps every intermediate state valid: mems grows to old|new before
// cpus is written, and shrinks to new only after.
const std::vector<std::pair<int, std::vector<int>>>& numa_node_cpus() {
  static const std::vector<std::pair<int, std::vector<int>>> nodes = [] {
    std::vector<std::pair<int, std::vector<int>>> out;
    for (int node = 0; node < 1024; ++node) {
      const auto text = trimmed(read_file("/sys/devices/system/node/node" + std::to_string(node) + "/cpulist"));
      if (text.empty()) {
        if (node > 64) {
          break;
        }
        continue;
      }
      if (auto cpus = parse_cpuset(text)) {
        out.emplace_back(node, *cpus);
      }
    }
    return out;
  }();
  return nodes;
}

std::vector<int> nodes_for_cpus(const std::vector<int>& cpus) {
  std::vector<int> nodes;
  for (const auto& [node, node_cpus] : numa_node_cpus()) {
    for (const int cpu : cpus) {
      if (std::find(node_cpus.begin(), node_cpus.end(), cpu) != node_cpus.end()) {
        nodes.push_back(node);
        break;
      }
    }
  }
  return nodes;
}

bool write_cpus_and_mems(const std::string& dir, const std::string& cpus_spec) {
  const auto want = parse_cpuset(trimmed(cpus_spec));
  const auto nodes = want ? nodes_for_cpus(*want) : std::vector<int>{};
  if (nodes.empty()) {
    return write_sysfs(dir + "/cpuset.cpus", cpus_spec);
  }
  std::vector<int> grown = nodes;
  if (const auto old = parse_cpuset(trimmed(read_file(dir + "/cpuset.mems")))) {
    grown.insert(grown.end(), old->begin(), old->end());
    std::sort(grown.begin(), grown.end());
    grown.erase(std::unique(grown.begin(), grown.end()), grown.end());
  }
  if (!write_sysfs(dir + "/cpuset.mems", node_list(grown))) {
    std::cerr << "cpuset.mems " << dir << " grow to " << node_list(grown) << " failed" << std::endl;
  }
  if (!write_sysfs(dir + "/cpuset.cpus", cpus_spec)) {
    return false;
  }
  if (grown != nodes && !write_sysfs(dir + "/cpuset.mems", node_list(nodes))) {
    std::cerr << "cpuset.mems " << dir << " shrink to " << node_list(nodes) << " failed" << std::endl;
  }
  return true;
}

// The cgroup is restricted before the new process is allowed to widen its mask.
std::optional<Placement> prepare_placement(const std::string& daemon_id, int linux_cpu) {
  if (linux_cpu < 0 || linux_cpu == mercury_linux_cpu) {
    return std::nullopt;
  }
  const auto dir = ensure_daemon_cpuset(daemon_id);
  if (!dir) {
    return std::nullopt;
  }
  const auto current = trimmed(read_file(*dir + "/cpuset.cpus"));
  if (current.empty()) {
    if (!write_cpus_and_mems(*dir, std::to_string(linux_cpu))) {
      return std::nullopt;
    }
    const std::vector<int> want{linux_cpu};
    bool applied = false;
    for (int attempt = 0; attempt < 50; ++attempt) {
      if (cpuset_applied(*dir, want)) {
        applied = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!applied) {
      return std::nullopt;
    }
    return Placement{*dir, linux_cpu};
  }
  const auto have = parse_cpuset(current);
  if (!have || have->empty()) {
    return std::nullopt;
  }
  return Placement{*dir, have->front()};
}

bool move_into_cgroup(const std::string& dir, pid_t pid) {
  return write_sysfs(dir + "/cgroup.procs", std::to_string(pid));
}

bool safe_daemon_id(const std::string& daemon_id) {
  if (daemon_id.empty() || daemon_id.size() > 255 || daemon_id == "." || daemon_id == "..") {
    return false;
  }
  for (const unsigned char c : daemon_id) {
    if (std::isalnum(c) == 0 && c != '.' && c != '_' && c != '-') {
      return false;
    }
  }
  return true;
}

void mark_counters_done(const std::string& daemon_id, pid_t pid) {
  {
    std::lock_guard lock(daemons_mu);
    if (auto* daemon = find_daemon(daemon_id); daemon != nullptr && daemon->pid == pid) {
      daemon->counters_done = true;
    }
  }
  daemons_cv.notify_all();
}

void watch_daemon(std::string daemon_id, pid_t pid, int out_fd, int err_fd) {
  const auto log = read_pipes(out_fd, err_fd);
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  const int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  const auto finished = now_ms();
  {
    std::lock_guard lock(daemons_mu);
    if (auto* daemon = find_daemon(daemon_id); daemon != nullptr && daemon->pid == pid) {
      daemon->running = false;
      daemon->exit_code = exit_code;
      daemon->log = log;
      daemon->finished = finished;
    }
  }
  daemons_cv.notify_all();
  note_event(daemon_id, "exit", "exit code " + std::to_string(exit_code));
  std::cout << "exited daemon_id=" << daemon_id << " exit_code=" << exit_code << " finished=" << finished
            << " log=" << log << std::endl;
}

}  // namespace

void set_daemon_cpus(std::vector<int> cpus) {
  std::sort(cpus.begin(), cpus.end());
  daemon_cpus = std::move(cpus);
}

void set_mercury_cpu(int cpu) { mercury_linux_cpu = cpu; }

std::optional<std::string> stat_route(const std::string& request) {
  if (request_target(request) != "/stat") {
    return std::nullopt;
  }
  const auto exec_path = json_value(request_body(request), "exec_path");
  if (!exec_path || exec_path->empty()) {
    return http_response(400, "Bad Request", ok_json(false), kJson);
  }
  bool running = false;
  {
    std::lock_guard lock(daemons_mu);
    for (const auto& daemon : daemons) {
      if (daemon.exec_path == *exec_path && daemon.running) {
        running = true;
        break;
      }
    }
  }
  return http_response(200, "OK", ok_json(running), kJson);
}

int bind_loopback(int port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  const int reuse = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<std::uint16_t>(port));
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 || ::listen(fd, 16) < 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

struct Launched {
  pid_t pid = -1;
  int out_fd = -1;
  int err_fd = -1;
};

[[noreturn]] void exec_daemon(const std::string& exec_path, int listen_fd,
                            const std::vector<std::pair<std::string, std::string>>& env) {
  std::vector<char> arg0(exec_path.begin(), exec_path.end());
  arg0.push_back('\0');
  char flag[] = "--listen-fd";
  char fd_text[] = "3";
  char* with_socket[] = {arg0.data(), flag, fd_text, nullptr};
  char* plain[] = {arg0.data(), nullptr};
  char** argv = listen_fd >= 0 ? with_socket : plain;
  if (env.empty()) {
    ::execv(exec_path.c_str(), argv);
    _exit(127);
  }
  extern char** environ;
  std::vector<std::string> owned;
  for (char** it = environ; it != nullptr && *it != nullptr; ++it) {
    owned.emplace_back(*it);
  }
  for (const auto& [name, value] : env) {
    const auto prefix = name + "=";
    bool replaced = false;
    for (auto& entry : owned) {
      if (entry.compare(0, prefix.size(), prefix) == 0) {
        entry = prefix + value;
        replaced = true;
        break;
      }
    }
    if (!replaced) {
      owned.push_back(prefix + value);
    }
  }
  std::vector<char*> envp;
  envp.reserve(owned.size() + 1);
  for (auto& entry : owned) {
    envp.push_back(entry.data());
  }
  envp.push_back(nullptr);
  ::execve(exec_path.c_str(), argv, envp.data());
  _exit(127);
}

namespace {

// Signal-safe registry of the daemon cgroups this mercury created, for kill_daemon_cgroups_raw.
// A slot is filled under g_cgroup_mu and published by its atomic flag; slots are never cleared,
// so a cgroup whose root pid exited is still killed (descendants may remain) and removed.
constexpr int kCgroupSlots = 1024;
constexpr std::size_t kCgroupPathMax = 320;  // "/sys/fs/cgroup/mercury/" + a 255-byte daemon_id
char g_cgroup_dirs[kCgroupSlots][kCgroupPathMax];
std::atomic<bool> g_cgroup_used[kCgroupSlots];
std::mutex g_cgroup_mu;  // only for insert, never in the signal handler

void remember_cgroup(const std::string& dir) {
  if (dir.size() + std::strlen("/cgroup.kill") >= kCgroupPathMax) {
    std::cout << "cgroup path too long to clean up on exit: " << dir << std::endl;
    return;
  }
  std::lock_guard lock(g_cgroup_mu);
  for (int i = 0; i < kCgroupSlots; ++i) {
    if (g_cgroup_used[i].load(std::memory_order_acquire) && dir == g_cgroup_dirs[i]) {
      return;
    }
  }
  for (int i = 0; i < kCgroupSlots; ++i) {
    if (!g_cgroup_used[i].load(std::memory_order_acquire)) {
      std::memcpy(g_cgroup_dirs[i], dir.c_str(), dir.size() + 1);
      g_cgroup_used[i].store(true, std::memory_order_release);
      return;
    }
  }
  std::cout << "cgroup registry full; not cleaned up on exit: " << dir << std::endl;
}

}  // namespace

void kill_daemon_cgroups_raw() {
  char path[kCgroupPathMax];
  bool any = false;
  for (int i = 0; i < kCgroupSlots; ++i) {
    if (!g_cgroup_used[i].load(std::memory_order_acquire)) {
      continue;
    }
    const std::size_t n = std::strlen(g_cgroup_dirs[i]);
    std::memcpy(path, g_cgroup_dirs[i], n);
    std::memcpy(path + n, "/cgroup.kill", sizeof("/cgroup.kill"));
    const int fd = ::open(path, O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
      const auto written = ::write(fd, "1", 1);
      (void)written;
      ::close(fd);
      any = true;
    }
  }
  if (!any) {
    return;
  }
  // rmdir succeeds once the kernel has reaped every task (EBUSY until then). Give up after 2 s.
  const timespec pause{0, 10 * 1000 * 1000};
  for (int attempt = 0; attempt < 200; ++attempt) {
    bool busy = false;
    for (int i = 0; i < kCgroupSlots; ++i) {
      if (g_cgroup_used[i].load(std::memory_order_acquire) && ::rmdir(g_cgroup_dirs[i]) != 0 && errno == EBUSY) {
        busy = true;
      }
    }
    if (!busy) {
      return;
    }
    ::nanosleep(&pause, nullptr);
  }
}

std::optional<Launched> launch(const std::string& daemon_id, const std::string& exec_path, int cpu, int listen_fd,
                               const std::vector<std::pair<std::string, std::string>>& env) {
  const int linux_cpu = cpu;
  const auto placement = prepare_placement(daemon_id, linux_cpu);
  if (placement) {
    remember_cgroup(placement->dir);
  }
  int out_pipe[2] = {-1, -1};
  int err_pipe[2] = {-1, -1};
  int sync_pipe[2] = {-1, -1};
  if (::pipe(out_pipe) != 0) {
    return std::nullopt;
  }
  if (::pipe(err_pipe) != 0) {
    ::close(out_pipe[0]);
    ::close(out_pipe[1]);
    return std::nullopt;
  }
  if (placement && ::pipe(sync_pipe) != 0) {
    ::close(out_pipe[0]);
    ::close(out_pipe[1]);
    ::close(err_pipe[0]);
    ::close(err_pipe[1]);
    return std::nullopt;
  }

  const pid_t pid = ::fork();
  if (pid < 0) {
    ::close(out_pipe[0]);
    ::close(out_pipe[1]);
    ::close(err_pipe[0]);
    ::close(err_pipe[1]);
    if (sync_pipe[0] >= 0) {
      ::close(sync_pipe[0]);
      ::close(sync_pipe[1]);
    }
    return std::nullopt;
  }
  if (pid == 0) {
    if (sync_pipe[1] >= 0) {
      ::close(sync_pipe[1]);
    }
    if (sync_pipe[0] >= 0) {
      char byte = 0;
      ssize_t n = 0;
      do {
        n = ::read(sync_pipe[0], &byte, 1);
      } while (n < 0 && errno == EINTR);
      ::close(sync_pipe[0]);
      if (n != 1 || byte != '1') {
        _exit(127);
      }
    }
    ::dup2(out_pipe[1], STDOUT_FILENO);
    ::dup2(err_pipe[1], STDERR_FILENO);
    ::close(out_pipe[0]);
    ::close(out_pipe[1]);
    ::close(err_pipe[0]);
    ::close(err_pipe[1]);
    if (listen_fd >= 0) {
      if (listen_fd != 3 && ::dup2(listen_fd, 3) < 0) {
        _exit(127);
      }
      ::syscall(__NR_close_range, 4U, ~0U, 0);
    } else {
      ::syscall(__NR_close_range, 3U, ~0U, 0);
    }

    if (placement) {
      if (!widen_to_online()) {
        _exit(127);
      }
    } else {
      cpu_set_t set;
      CPU_ZERO(&set);
      CPU_SET(linux_cpu, &set);
      if (::sched_setaffinity(0, sizeof(set), &set) != 0) {
        _exit(127);
      }
    }
    raise_nofile();
    exec_daemon(exec_path, listen_fd, env);
  }

  ::close(out_pipe[1]);
  ::close(err_pipe[1]);
  if (placement) {
    ::close(sync_pipe[0]);
    bool ready = pin_pid(pid, placement->pin_linux) && move_into_cgroup(placement->dir, pid);
    const char go = '1';
    if (ready) {
      ssize_t n = 0;
      do {
        n = ::write(sync_pipe[1], &go, 1);
      } while (n < 0 && errno == EINTR);
      ready = n == 1;
    }
    ::close(sync_pipe[1]);
    if (!ready) {
      ::kill(pid, SIGKILL);
      ::waitpid(pid, nullptr, 0);
      ::close(out_pipe[0]);
      ::close(err_pipe[0]);
      return std::nullopt;
    }
    note_event(daemon_id, "start", "started on CPU " + std::to_string(placement->pin_linux));
    std::cout << "scheduled daemon_id=" << daemon_id << " cpuset=" << placement->pin_linux
              << " mask=online" << std::endl;
  }
  return Launched{pid, out_pipe[0], err_pipe[0]};
}

bool image_is(pid_t pid, const std::string& exec_path) {
  char buf[4096];
  const auto link = "/proc/" + std::to_string(pid) + "/exe";
  const auto n = ::readlink(link.c_str(), buf, sizeof(buf) - 1);
  if (n < 0) {
    return false;
  }
  return std::string(buf, static_cast<std::size_t>(n)) == exec_path;
}

bool wait_until_image(pid_t pid, const std::string& exec_path) {
  for (int attempt = 0; attempt < 200; ++attempt) {
    if (image_is(pid, exec_path)) {
      return true;
    }
    int status = 0;
    if (::waitpid(pid, &status, WNOHANG) == pid) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return image_is(pid, exec_path);
}

static std::int64_t monotonic_ns() {
  timespec now{};
  if (::clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    return 0;
  }
  return static_cast<std::int64_t>(now.tv_sec) * 1000000000LL + now.tv_nsec;
}

std::string format_cpuset(const std::vector<int>& cpus) {
  std::string out;
  for (std::size_t i = 0; i < cpus.size();) {
    const int first = cpus[i];
    int last = first;
    std::size_t j = i + 1;
    while (j < cpus.size() && cpus[j] == last + 1) {
      last = cpus[j];
      ++j;
    }
    if (!out.empty()) {
      out.push_back(',');
    }
    out += std::to_string(first);
    if (last != first) {
      out.push_back('-');
      out += std::to_string(last);
    }
    i = j;
  }
  return out;
}

std::vector<int> cpuset_of(const std::string& daemon_id) {
  const auto have = parse_cpuset(trimmed(read_file("/sys/fs/cgroup/mercury/" + daemon_id + "/cpuset.cpus")));
  return have ? *have : std::vector<int>{};
}

int cpuset_cores(const std::string& daemon_id) { return static_cast<int>(cpuset_of(daemon_id).size()); }

bool add_core(const std::string& daemon_id, int max_cores, int& cores_after, std::string& spec_after,
              CpuPlacement placement = CpuPlacement::LeastLoaded) {
  const auto dir = ensure_daemon_cpuset(daemon_id);
  if (!dir) {
    return false;
  }
  auto have = parse_cpuset(trimmed(read_file(*dir + "/cpuset.cpus")));
  if (!have || have->empty() || static_cast<int>(have->size()) >= max_cores) {
    return false;
  }
  // Another daemon may already run on the CPU placement returns. With an l3 pool, only pool CPUs are offered.
  auto next = request_cpu_for(placement, *have, pool_exclude(daemon_id));
  if (!next) {
    return false;
  }
  have->push_back(*next);
  std::sort(have->begin(), have->end());
  const auto spec = format_cpuset(*have);
  if (!write_cpus_and_mems(*dir, spec)) {
    return false;
  }
  for (int attempt = 0; attempt < 50; ++attempt) {
    if (cpuset_applied(*dir, *have)) {
      cores_after = static_cast<int>(have->size());
      spec_after = spec;
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

// "full" baseline: the cpuset becomes the whole daemon pool, and nothing scales it afterwards.
bool set_full_pool(const std::string& daemon_id, int& cores_after, std::string& spec_after) {
  const auto dir = ensure_daemon_cpuset(daemon_id);
  if (!dir || daemon_cpus.empty()) {
    return false;
  }
  const auto spec = format_cpuset(daemon_cpus);
  if (!write_cpus_and_mems(*dir, spec)) {
    return false;
  }
  for (int attempt = 0; attempt < 50; ++attempt) {
    if (cpuset_applied(*dir, daemon_cpus)) {
      cores_after = static_cast<int>(daemon_cpus.size());
      spec_after = spec;
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

bool remove_core(const std::string& daemon_id, int min_cores, int& cores_after, std::string& spec_after) {
  const auto dir = ensure_daemon_cpuset(daemon_id);
  if (!dir) {
    return false;
  }
  auto have = parse_cpuset(trimmed(read_file(*dir + "/cpuset.cpus")));
  if (!have || have->size() <= 1 || static_cast<int>(have->size()) <= min_cores) {
    return false;
  }
  have->pop_back();
  const auto spec = format_cpuset(*have);
  if (!write_cpus_and_mems(*dir, spec)) {
    return false;
  }
  for (int attempt = 0; attempt < 50; ++attempt) {
    if (cpuset_applied(*dir, *have)) {
      cores_after = static_cast<int>(have->size());
      spec_after = spec;
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

// Writes the whole cpuset in one update and waits for the kernel to report it.
bool set_cpuset(const std::string& daemon_id, const std::vector<int>& cpus, int& cores_after, std::string& spec_after) {
  const auto dir = ensure_daemon_cpuset(daemon_id);
  if (!dir || cpus.empty()) {
    return false;
  }
  const auto spec = format_cpuset(cpus);
  if (!write_cpus_and_mems(*dir, spec)) {
    return false;
  }
  for (int attempt = 0; attempt < 50; ++attempt) {
    if (cpuset_applied(*dir, cpus)) {
      cores_after = static_cast<int>(cpus.size());
      spec_after = spec;
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

// Serializes l3 and shift steps across daemons, so two daemons never take the same free CPUs.
std::mutex l3_mu;
// When each L3 domain last lost a daemon to shift (CLOCK_MONOTONIC), under l3_mu: one leaves per window.
std::map<std::string, std::int64_t> shift_left_at;

// Every L3 domain with an allocatable CPU, as seen by daemon_id holding the CPUs in mine.
std::vector<L3Block> l3_blocks(const std::string& daemon_id, const std::vector<int>& mine) {
  std::vector<std::string> others;
  {
    std::lock_guard lock(daemons_mu);
    for (const auto& daemon : daemons) {
      if (daemon.running && daemon.daemon_id != daemon_id) {
        others.push_back(daemon.daemon_id);
      }
    }
  }
  std::vector<int> taken = mine;
  for (const auto& other : others) {
    const auto cpus = cpuset_of(other);
    taken.insert(taken.end(), cpus.begin(), cpus.end());
  }
  const auto pool = allocatable_cpus();
  const auto groups = resource_groups();
  const auto parent_of = [&](const std::string& name) {
    for (const auto& group : groups) {
      if (group.name == name) {
        return group.parent;
      }
    }
    return std::string();
  };
  const auto in = [](const std::vector<int>& cpus, int cpu) { return std::find(cpus.begin(), cpus.end(), cpu) != cpus.end(); };
  std::vector<L3Block> blocks;
  for (const auto& group : groups) {
    if (group.level != ResourceLevel::L3) {
      continue;
    }
    L3Block block;
    block.name = group.name;
    block.node = group.parent;
    block.socket = parent_of(group.parent);
    block.load = group.load;
    for (const int cpu : group.linux_cpus) {
      if (!in(pool, cpu)) {
        continue;
      }
      block.cpus.push_back(cpu);
      if (in(mine, cpu)) {
        ++block.held;
      } else if (!in(taken, cpu)) {
        block.free.push_back(cpu);
      }
    }
    block.partial = block.cpus.size() != group.linux_cpus.size();
    if (!block.cpus.empty()) {
      blocks.push_back(std::move(block));
    }
  }
  return blocks;
}

// Every L3 domain with an allocatable CPU, as the pool sees it: held are its CPUs in the pool, free the rest.
std::vector<L3Block> pool_blocks(const std::vector<int>& active) {
  const auto pool_cpus = allocatable_cpus();
  const auto groups = resource_groups();
  const auto parent_of = [&](const std::string& name) {
    for (const auto& group : groups) {
      if (group.name == name) {
        return group.parent;
      }
    }
    return std::string();
  };
  const auto in = [](const std::vector<int>& cpus, int cpu) { return std::find(cpus.begin(), cpus.end(), cpu) != cpus.end(); };
  std::vector<L3Block> blocks;
  for (const auto& group : groups) {
    if (group.level != ResourceLevel::L3) {
      continue;
    }
    L3Block block;
    block.name = group.name;
    block.node = group.parent;
    block.socket = parent_of(group.parent);
    block.load = group.load;
    for (const int cpu : group.linux_cpus) {
      if (!in(pool_cpus, cpu)) {
        continue;
      }
      block.cpus.push_back(cpu);
      if (in(active, cpu)) {
        ++block.held;
      } else {
        block.free.push_back(cpu);
      }
    }
    block.partial = block.cpus.size() != group.linux_cpus.size();
    if (!block.cpus.empty()) {
      blocks.push_back(std::move(block));
    }
  }
  return blocks;
}

// Group names at this level that contain one of these CPUs.
std::vector<std::string> names_touching(ResourceLevel level, const std::vector<int>& cpus) {
  std::vector<std::string> names;
  for (const auto& group : resource_groups()) {
    if (group.level == level &&
        std::any_of(cpus.begin(), cpus.end(), [&](int cpu) {
          return std::find(group.linux_cpus.begin(), group.linux_cpus.end(), cpu) != group.linux_cpus.end();
        })) {
      names.push_back(group.name);
    }
  }
  return names;
}

std::vector<std::string> l3_names_touching(const std::vector<int>& cpus) { return names_touching(ResourceLevel::L3, cpus); }

// Writes cpu.max ("<quota> <period>" or "max <period>"); the cpu controller is enabled on both ancestors first.
bool write_cpu_max(const std::string& daemon_id, const std::string& value) {
  const auto dir = ensure_daemon_cpuset(daemon_id);
  if (!dir || !ensure_controller("/sys/fs/cgroup/cgroup.subtree_control", "cpu") ||
      !ensure_controller("/sys/fs/cgroup/mercury/cgroup.subtree_control", "cpu")) {
    return false;
  }
  return write_sysfs(*dir + "/cpu.max", value);
}

// The socket groups cover every online CPU, so their load is the whole host's.
struct HostGroups {
  std::vector<std::string> names;
  int cpus = 0;
};

HostGroups host_groups() {
  HostGroups host;
  for (const auto& group : resource_groups()) {
    if (group.level == ResourceLevel::Socket) {
      host.names.push_back(group.name);
      host.cpus += static_cast<int>(group.linux_cpus.size());
    }
  }
  return host;
}

// Mean CPUs used (task-clock over wall time) by the samples of a daemon cgroup ring newer than from_ns.
// The first such sample only opens the span: its task-clock covers the gap before it.
double ring_cpus(const Ring& ring, std::int64_t from_ns) {
  const auto age_ms = std::max<std::int64_t>(0, (monotonic_ns() - from_ns) / 1000000);
  std::uint64_t used = 0;
  std::int64_t first = 0, last = 0;
  for (const auto& sample : ring.recent(static_cast<std::size_t>(age_ms / kSampleMs + 2))) {
    if (sample.timestamp_ns <= from_ns) {
      continue;
    }
    if (first == 0) {
      first = sample.timestamp_ns;
    } else {
      used += sample.value[kCgroupTaskClock];
    }
    last = sample.timestamp_ns;
  }
  return last > first ? static_cast<double>(used) / static_cast<double>(last - first) : 0;
}

// The waiting signal of a daemon cgroup ring after from_ns: the share of time some task waited for a
// CPU (cpu.pressure "some") when the ring records pressure, else the mean count of runnable threads.
// psi tells which. Empty without two samples.
struct RingWaiting {
  bool psi = true;
  double value = 0;
};

std::optional<RingWaiting> ring_waiting(const Ring& ring, std::int64_t from_ns) {
  const auto& names = ring.names();
  if (names.size() <= static_cast<std::size_t>(kCgroupPressure)) {
    return std::nullopt;
  }
  RingWaiting waiting;
  waiting.psi = names[kCgroupPressure] == "pressure";
  const auto age_ms = std::max<std::int64_t>(0, (monotonic_ns() - from_ns) / 1000000);
  double sum = 0;
  std::size_t count = 0;
  std::int64_t first = 0, last = 0;
  for (const auto& sample : ring.recent(static_cast<std::size_t>(age_ms / kSampleMs + 2))) {
    if (sample.timestamp_ns <= from_ns || sample.nvalues <= kCgroupPressure) {
      continue;
    }
    if (first == 0) {
      first = sample.timestamp_ns;
      if (waiting.psi) {
        continue;  // its pressure covers the gap before it, as for task-clock
      }
    }
    sum += static_cast<double>(sample.value[kCgroupPressure]);
    ++count;
    last = sample.timestamp_ns;
  }
  if (waiting.psi) {
    if (last <= first) {
      return std::nullopt;
    }
    waiting.value = sum / static_cast<double>(last - first);
  } else {
    if (count == 0) {
      return std::nullopt;
    }
    waiting.value = sum / static_cast<double>(count);
  }
  return waiting;
}

// Busy share of the named groups over capacity CPUs rather than over all their CPUs, at most 1. The
// groups' rings count every CPU in them, including ones no daemon may use (Mercury's own) and ones
// outside the cpuset being judged; those would dilute the load (a 4-CPU L3 domain holding Mercury's
// CPU could never read above 75%). The rest of the group is treated as idle.
std::optional<double> load_over(const std::vector<std::string>& names, std::size_t capacity, std::int64_t from_ns,
                                std::int64_t to_ns = std::numeric_limits<std::int64_t>::max()) {
  const auto load = groups_load(names, from_ns, to_ns);
  if (!load || capacity == 0) {
    return load;
  }
  std::size_t cpus = 0;
  for (const auto& group : resource_groups()) {
    if (std::find(names.begin(), names.end(), group.name) != names.end()) {
      cpus += group.linux_cpus.size();
    }
  }
  return std::min(1.0, *load * static_cast<double>(cpus) / static_cast<double>(capacity));
}

// The allowed CPU (not excluded, not already in have) that the fewest running daemons hold, then the lower
// Linux CPU. Used to place into the pool: least_loaded alone saw the same idle CPU for every service at
// startup and stacked ten hotel services on CPU 2, where each one's occupancy looked low and none grew.
std::optional<int> pool_pick(const std::vector<int>& excluded, const std::vector<int>& have) {
  std::vector<std::string> ids;
  {
    std::lock_guard lock(daemons_mu);
    for (const auto& daemon : daemons) {
      if (daemon.running) {
        ids.push_back(daemon.daemon_id);
      }
    }
  }
  std::map<int, int> on;
  for (const auto& id : ids) {
    for (const int cpu : cpuset_of(id)) {
      ++on[cpu];
    }
  }
  std::optional<int> best;
  for (const int cpu : allocatable_cpus()) {
    if (std::find(excluded.begin(), excluded.end(), cpu) != excluded.end() ||
        std::find(have.begin(), have.end(), cpu) != have.end()) {
      continue;
    }
    if (!best || on[cpu] < on[*best]) {
      best = cpu;
    }
  }
  return best;
}

// Daemons holding CPUs of a domain that left the pool get as many pool CPUs instead, spread by pool_pick (their
// CPU count is kept while the pool has CPUs they do not hold yet). Exempt daemons keep theirs.
void pool_relocate(const std::vector<int>& removed) {
  std::vector<std::string> ids;
  {
    std::lock_guard lock(daemons_mu);
    for (const auto& daemon : daemons) {
      if (daemon.running) {
        ids.push_back(daemon.daemon_id);
      }
    }
  }
  std::lock_guard place(l3_mu);
  for (const auto& id : ids) {
    const auto excluded = pool_exclude(id);
    if (excluded.empty() || pool_exempt_daemon(id)) {
      continue;  // exempt, or the pool was turned off meanwhile
    }
    const auto held = cpuset_of(id);
    std::vector<int> kept;
    std::size_t leaving = 0;
    for (const int cpu : held) {
      if (std::find(removed.begin(), removed.end(), cpu) != removed.end()) {
        ++leaving;
      } else {
        kept.push_back(cpu);
      }
    }
    if (leaving == 0) {
      continue;
    }
    for (; leaving > 0; --leaving) {
      const auto pick = pool_pick(excluded, kept);
      if (!pick) {
        break;
      }
      kept.push_back(*pick);
    }
    std::sort(kept.begin(), kept.end());
    int after = 0;
    std::string spec;
    if (kept.empty() || !set_cpuset(id, kept, after, spec)) {
      std::cerr << "pool daemon_id=" << id << " could not move off removed CPUs " << format_cpuset(removed) << std::endl;
      continue;
    }
    std::cout << "pool daemon_id=" << id << " moved " << format_cpuset(held) << " -> " << spec << std::endl;
    note_event(id, "pool", "moved off a domain that left the pool: " + format_cpuset(held) + " -> " + spec);
    std::lock_guard lock(daemons_mu);
    if (auto* daemon = find_daemon(id)) {
      daemon->scale.changed_at_ns = monotonic_ns();
    }
  }
}

// What the pool loop and the LLM judge the pool by, over one window: busy share of the pool's CPUs, the services'
// mean CPU pressure (non-exempt daemons on the pool that used at least 0.05 CPU and are below their autoscaling
// cap: a daemon at its max_cores waits however large the pool, so its waiting is not unmet pool demand; with it
// counted, one 32-thread daemon capped at 16 cores grew a test pool to all 63 CPUs), and per L3 domain (pool_blocks
// order, with window loads) the CPUs' worth of load that Mercury's daemons do not account for (a daemon's use on a
// domain is its cpu_use times the share of its CPUs there).
struct PoolSignals {
  std::optional<double> busy;
  double pressure = 0;
  // The pool CPUs the autoscaler has handed out, counted once per daemon holding them: above the pool's size the
  // services are doubled up on shared CPUs because the pool is too small.
  std::size_t allocated = 0;
  std::vector<L3Block> blocks;
  std::vector<double> foreign;
};

PoolSignals pool_signals(const std::vector<int>& active, std::int64_t from_ns) {
  PoolSignals out;
  out.busy = load_over(l3_names_touching(active), active.size(), from_ns);
  struct Held {
    std::vector<int> cpus;
    double use = 0;
  };
  struct Row {
    std::string id;
    std::shared_ptr<Ring> ring;
    int max_cores = 0;
  };
  std::vector<Row> rows;
  {
    std::lock_guard lock(daemons_mu);
    for (const auto& daemon : daemons) {
      if (daemon.running && daemon.ring) {
        rows.push_back({daemon.daemon_id, daemon.ring, daemon.scale.max_cores});
      }
    }
  }
  std::vector<Held> held;
  double pressure_sum = 0;
  int pressured = 0;
  for (const auto& [id, ring, max_cores] : rows) {
    Held h{cpuset_of(id), ring_cpus(*ring, from_ns)};
    const bool on_pool = std::any_of(h.cpus.begin(), h.cpus.end(),
                                     [&](int c) { return std::find(active.begin(), active.end(), c) != active.end(); });
    const bool capped = max_cores > 0 && static_cast<int>(h.cpus.size()) >= max_cores;
    if (on_pool && !pool_exempt_daemon(id)) {
      out.allocated += static_cast<std::size_t>(std::count_if(h.cpus.begin(), h.cpus.end(), [&](int c) {
        return std::find(active.begin(), active.end(), c) != active.end();
      }));
    }
    if (on_pool && !capped && !pool_exempt_daemon(id) && h.use >= 0.05) {
      if (const auto waiting = ring_waiting(*ring, from_ns); waiting && waiting->psi) {
        pressure_sum += waiting->value;
        ++pressured;
      }
    }
    held.push_back(std::move(h));
  }
  out.pressure = pressured > 0 ? pressure_sum / pressured : 0;
  out.blocks = pool_blocks(active);
  for (auto& block : out.blocks) {
    block.load = load_over({block.name}, block.cpus.size(), from_ns).value_or(0);
    double ours = 0;
    for (const auto& h : held) {
      if (h.cpus.empty()) {
        continue;
      }
      const auto inside = std::count_if(h.cpus.begin(), h.cpus.end(), [&](int c) {
        return std::find(block.cpus.begin(), block.cpus.end(), c) != block.cpus.end();
      });
      ours += h.use * static_cast<double>(inside) / static_cast<double>(h.cpus.size());
    }
    out.foreign.push_back(std::max(0.0, block.load * static_cast<double>(block.cpus.size()) - ours));
  }
  return out;
}

// Adds one L3 domain to the pool: the named one (it must have free CPUs), else l3_take_block's choice. Shared by
// pool_loop (l3) and the LLM's grow_pool. False with a reason in detail.
bool pool_grow_step(const std::optional<std::string>& name, const std::string& why, std::string& detail) {
  PoolState state;
  {
    std::lock_guard lock(pool_mu);
    state = pool;
  }
  if (!state.enabled) {
    detail = "the pool is off";
    return false;
  }
  auto active = state.active;
  auto blocks = pool_blocks(active);
  if (name) {
    const auto at = std::find_if(blocks.begin(), blocks.end(), [&](const L3Block& b) { return b.name == *name; });
    if (at == blocks.end()) {
      detail = "unknown L3 domain " + *name;
      return false;
    }
    if (at->free.empty()) {
      detail = *name + " is already in the pool";
      return false;
    }
    blocks = {*at};
  }
  if (!l3_take_block(blocks, active, 64 * blocks.size())) {
    detail = "no free L3 domain";
    return false;
  }
  if (state.max > 0 && active.size() > state.max) {
    detail = "the pool budget is " + std::to_string(state.max) + " CPUs; it has " + std::to_string(state.active.size()) +
             " and the next domain would make " + std::to_string(active.size());
    return false;
  }
  std::vector<int> added;
  for (const int cpu : active) {
    if (std::find(state.active.begin(), state.active.end(), cpu) == state.active.end()) {
      added.push_back(cpu);
    }
  }
  {
    std::lock_guard lock(pool_mu);
    if (!pool.enabled || pool.active != state.active) {
      detail = "the pool changed meanwhile";
      return false;
    }
    pool.active = active;
    pool.grown.push_back(added);
    pool.changed_at_ns = monotonic_ns();
  }
  const auto spec = format_cpuset(active);
  detail = "added " + format_cpuset(added) + ", pool " + spec;
  std::cout << "pool l3 " << why << " cores=" << active.size() << " cpus=" << spec << " t_ns=" << monotonic_ns() << std::endl;
  note_event("", "pool", "added an L3 domain (" + why + "), pool " + spec);
  return true;
}

// Swaps a pooled domain for a free one: the pool loses from's pooled CPUs and gains as many of to's, and every
// non-exempt daemon on a leaving CPU moves to the CPU in the same position in to (so SMT pairs stay pairs). Shared
// by pool_loop (shift) and the LLM's swap_domain. False with a reason in detail.
bool pool_swap_step(const std::string& from, const std::string& to, const std::string& why, std::string& detail) {
  PoolState state;
  {
    std::lock_guard lock(pool_mu);
    state = pool;
  }
  if (!state.enabled) {
    detail = "the pool is off";
    return false;
  }
  const auto blocks = pool_blocks(state.active);
  const auto find = [&](const std::string& n) {
    return std::find_if(blocks.begin(), blocks.end(), [&](const L3Block& b) { return b.name == n; });
  };
  const auto src = find(from), dst = find(to);
  if (src == blocks.end() || dst == blocks.end()) {
    detail = "unknown L3 domain";
    return false;
  }
  if (src->held == 0) {
    detail = from + " is not in the pool";
    return false;
  }
  if (dst->held > 0 || dst->free.size() < static_cast<std::size_t>(src->held)) {
    detail = to + " must be outside the pool with at least " + std::to_string(src->held) + " free CPUs";
    return false;
  }
  std::vector<int> leaving, coming(dst->free);
  for (const int cpu : src->cpus) {
    if (std::find(state.active.begin(), state.active.end(), cpu) != state.active.end()) {
      leaving.push_back(cpu);
    }
  }
  std::sort(leaving.begin(), leaving.end());
  std::sort(coming.begin(), coming.end());
  coming.resize(leaving.size());
  std::vector<int> active;
  for (const int cpu : state.active) {
    if (std::find(leaving.begin(), leaving.end(), cpu) == leaving.end()) {
      active.push_back(cpu);
    }
  }
  active.insert(active.end(), coming.begin(), coming.end());
  std::sort(active.begin(), active.end());
  {
    std::lock_guard lock(pool_mu);
    if (!pool.enabled || pool.active != state.active) {
      detail = "the pool changed meanwhile";
      return false;
    }
    pool.active = active;
    // A grown domain that is swapped out is replaced by the new one in the scale-in order.
    for (auto& step : pool.grown) {
      if (std::find(step.begin(), step.end(), leaving.front()) != step.end()) {
        step = coming;
      }
    }
    pool.changed_at_ns = monotonic_ns();
  }
  // Move every non-exempt daemon off the leaving CPUs, position for position.
  std::vector<std::string> ids;
  {
    std::lock_guard lock(daemons_mu);
    for (const auto& daemon : daemons) {
      if (daemon.running) {
        ids.push_back(daemon.daemon_id);
      }
    }
  }
  int moved = 0;
  {
    std::lock_guard place(l3_mu);
    for (const auto& id : ids) {
      if (pool_exempt_daemon(id)) {
        continue;
      }
      auto cpus = cpuset_of(id);
      bool touched = false;
      for (auto& cpu : cpus) {
        const auto at = std::find(leaving.begin(), leaving.end(), cpu);
        if (at != leaving.end()) {
          cpu = coming[static_cast<std::size_t>(at - leaving.begin())];
          touched = true;
        }
      }
      if (!touched) {
        continue;
      }
      std::sort(cpus.begin(), cpus.end());
      cpus.erase(std::unique(cpus.begin(), cpus.end()), cpus.end());
      int after = 0;
      std::string spec;
      if (set_cpuset(id, cpus, after, spec)) {
        ++moved;
        note_event(id, "pool", "moved with a swapped domain to " + spec);
        std::lock_guard lock(daemons_mu);
        if (auto* daemon = find_daemon(id)) {
          daemon->scale.changed_at_ns = monotonic_ns();
        }
      }
    }
  }
  const auto spec = format_cpuset(active);
  detail = "swapped " + from + " (" + format_cpuset(leaving) + ") for " + to + " (" + format_cpuset(coming) + "), moved " +
           std::to_string(moved) + " daemons, pool " + spec;
  std::cout << "pool swap " << why << " " << detail << " t_ns=" << monotonic_ns() << std::endl;
  note_event("", "pool", "swap (" + why + "): " + detail);
  return true;
}

// Gives one L3 domain back: the named pooled domain, else the one added last; never below the start pool. The
// pool shrinks first, then pool_relocate moves the daemons on it to the CPUs left. Shared by pool_loop (l3
// scale-in) and the LLM's shrink_pool. False with a reason in detail.
bool pool_shrink_step(const std::optional<std::string>& name, const std::string& why, std::string& detail) {
  PoolState state;
  {
    std::lock_guard lock(pool_mu);
    state = pool;
  }
  if (!state.enabled) {
    detail = "the pool is off";
    return false;
  }
  std::vector<int> removed;
  if (name) {
    for (const auto& b : pool_blocks(state.active)) {
      if (b.name == *name) {
        for (const int cpu : b.cpus) {
          if (std::find(state.active.begin(), state.active.end(), cpu) != state.active.end()) {
            removed.push_back(cpu);
          }
        }
      }
    }
    if (removed.empty()) {
      detail = *name + " is not in the pool";
      return false;
    }
  } else if (!state.grown.empty()) {
    removed = state.grown.back();
  } else {
    detail = "the pool is at its start size";
    return false;
  }
  if (state.active.size() - removed.size() < state.start) {
    detail = "giving back " + format_cpuset(removed) + " would take the pool below its start size of " +
             std::to_string(state.start) + " CPUs";
    return false;
  }
  std::vector<int> active;
  for (const int cpu : state.active) {
    if (std::find(removed.begin(), removed.end(), cpu) == removed.end()) {
      active.push_back(cpu);
    }
  }
  {
    std::lock_guard lock(pool_mu);
    if (!pool.enabled || pool.active != state.active) {
      detail = "the pool changed meanwhile";
      return false;
    }
    pool.active = active;
    // Drop the removed CPUs from the scale-in order; an emptied step goes.
    for (auto& step : pool.grown) {
      step.erase(std::remove_if(step.begin(), step.end(),
                                [&](int c) { return std::find(removed.begin(), removed.end(), c) != removed.end(); }),
                 step.end());
    }
    pool.grown.erase(std::remove_if(pool.grown.begin(), pool.grown.end(), [](const auto& s) { return s.empty(); }),
                     pool.grown.end());
    pool.changed_at_ns = monotonic_ns();
  }
  const auto spec = format_cpuset(active);
  detail = "removed " + format_cpuset(removed) + ", pool " + spec;
  std::cout << "pool l3 shrink " << why << " cores=" << active.size() << " cpus=" << spec << " removed "
            << format_cpuset(removed) << " t_ns=" << monotonic_ns() << std::endl;
  note_event("", "pool", "removed " + format_cpuset(removed) + " (" + why + "), pool " + spec);
  pool_relocate(removed);
  return true;
}

// The pool's loop. Every 100 ms, once a full window has passed since the last change:
//   swap (the shift system): a pooled domain loaded from outside Mercury is swapped for a clean free one
//     (plan_pool_swap); saturation by the daemons themselves never swaps;
//   grow (l3): busy above up over the pool's CPUs, or the services' mean pressure at or above pressure_up, adds
//     an L3 domain (pool_grow_step); busy at or below down with pressure near 0 over the longer down window gives
//     the domain added last back (never below the start pool) and moves the daemons on it (pool_relocate).
// fixed with neither does nothing: the pool is the capacity the other systems are judged on.
void pool_loop() {
  bool full_logged = false;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    PoolState state;
    {
      std::lock_guard lock(pool_mu);
      state = pool;
    }
    const auto now = monotonic_ns();
    const auto window_ns = static_cast<std::int64_t>(state.window_ms) * 1000000LL;
    if (!state.enabled || state.active.empty() || (!state.grow && !state.swap) || now - state.changed_at_ns < window_ns) {
      continue;
    }
    const auto signals = pool_signals(state.active, now - window_ns);
    if (!signals.busy) {
      continue;
    }
    if (state.swap) {
      if (const auto plan = plan_pool_swap(signals.blocks, signals.foreign, state.swap_share)) {
        std::string detail;
        const auto& src = signals.blocks[plan->from];
        std::ostringstream why;
        why << "foreign=" << signals.foreign[plan->from] << " cpus on " << src.name;
        pool_swap_step(src.name, signals.blocks[plan->to].name, why.str(), detail);
        continue;
      }
    }
    if (!state.grow) {
      continue;
    }
    // Unmet demand: services wait while the autoscaler has handed out more pool CPUs than the pool has (they share
    // CPUs because the pool is too small). Pressure alone also comes from a daemon still climbing toward its cap
    // in a pool that has room: a test pool grew from 8 to 63 CPUs ahead of one daemon's autoscaler.
    const bool short_of_pool = signals.pressure >= state.pressure_up && signals.allocated > state.active.size();
    if (*signals.busy > state.up || short_of_pool) {
      std::ostringstream why;
      why << "load=" << *signals.busy << " pressure=" << signals.pressure << " allocated=" << signals.allocated;
      std::string detail;
      if (!pool_grow_step(std::nullopt, why.str(), detail) && !full_logged) {
        std::cout << "pool l3 " << why.str() << " " << detail << ", holding at " << state.active.size() << " CPUs" << std::endl;
        full_logged = true;
      }
      continue;
    }
    full_logged = false;
    const auto down_ns = static_cast<std::int64_t>(state.down_window_ms) * 1000000LL;
    if (state.grown.empty() || now - state.changed_at_ns < down_ns) {
      continue;
    }
    const auto quiet = pool_signals(state.active, now - down_ns);
    if (!quiet.busy || *quiet.busy > state.down || quiet.pressure > 0.02) {
      continue;
    }
    std::ostringstream why;
    why << "load=" << *quiet.busy;
    std::string detail;
    pool_shrink_step(std::nullopt, why.str(), detail);
  }
}

// Turns the l3 pool on with start CPUs of whole L3 domains, seeded at seed_cpu's domain. False when the
// resource groups are not ready. Starts pool_loop once.
bool enable_pool(const PoolState& config, int seed_cpu, std::string& spec) {
  if (!resources_ready()) {
    return false;
  }
  const auto active = pool_start(pool_blocks({}), config.start, seed_cpu);
  if (active.empty()) {
    return false;
  }
  {
    std::lock_guard lock(pool_mu);
    pool = config;
    pool.enabled = true;
    pool.active = active;
    pool.grown.clear();
    pool.changed_at_ns = monotonic_ns();
  }
  static std::once_flag started;
  std::call_once(started, [] { std::thread(pool_loop).detach(); });
  spec = format_cpuset(active);
  std::cout << "pool " << (config.grow ? "l3" : "fixed") << (config.swap ? " swap" : "") << " start=" << config.start
            << " max=" << config.max
            << " up=" << config.up << " pressure=" << config.pressure_up << " window_ms=" << config.window_ms
            << " down=" << config.down << " down_window_ms=" << config.down_window_ms << " cores=" << active.size()
            << " cpus=" << spec << " t_ns=" << monotonic_ns() << std::endl;
  note_event("", "pool", std::string(config.grow ? "l3" : "fixed") + (config.swap ? " swap" : "") + " pool on, " + spec);
  return true;
}

// The CPUs throttle judges: the active pool when the l3 pool is on, else every CPU held by a running daemon.
// That is the system Mercury runs, not the whole host (on dtn-01's 64 CPUs a hotel run at 1600 req/s keeps
// the host near 10% while its own CPUs are busy), and not only the throttled daemons, which for degrade are
// the optional services and may be idle while the rest of the system is overloaded.
std::vector<int> throttle_scope() {
  {
    std::lock_guard lock(pool_mu);
    if (pool.enabled) {
      return pool.active;
    }
  }
  std::vector<std::string> ids;
  {
    std::lock_guard lock(daemons_mu);
    for (const auto& daemon : daemons) {
      if (daemon.running) {
        ids.push_back(daemon.daemon_id);
      }
    }
  }
  std::vector<int> cpus;
  for (const auto& id : ids) {
    const auto held = cpuset_of(id);
    cpus.insert(cpus.end(), held.begin(), held.end());
  }
  std::sort(cpus.begin(), cpus.end());
  cpus.erase(std::unique(cpus.begin(), cpus.end()), cpus.end());
  return cpus;
}

// cpu.stat is the kernel total for every task in the daemon cgroup.
struct CgroupCpu {
  std::uint64_t usage_usec = 0;
  std::uint64_t user_usec = 0;
  std::uint64_t system_usec = 0;
};

std::optional<CgroupCpu> read_cgroup_cpu(const std::string& daemon_id) {
  const auto text = read_file("/sys/fs/cgroup/mercury/" + daemon_id + "/cpu.stat");
  if (text.empty()) {
    return std::nullopt;
  }
  CgroupCpu cpu;
  bool saw = false;
  std::istringstream in(text);
  std::string key;
  std::uint64_t value = 0;
  while (in >> key >> value) {
    if (key == "usage_usec") {
      cpu.usage_usec = value;
      saw = true;
    } else if (key == "user_usec") {
      cpu.user_usec = value;
    } else if (key == "system_usec") {
      cpu.system_usec = value;
    }
  }
  if (!saw) {
    return std::nullopt;
  }
  return cpu;
}

// "some total" of the daemon cgroup cpu.pressure, in microseconds. Empty when PSI is unavailable.
std::optional<std::uint64_t> read_cgroup_pressure(const std::string& daemon_id) {
  const auto text = read_file("/sys/fs/cgroup/mercury/" + daemon_id + "/cpu.pressure");
  const auto line = text.find("some ");
  if (line == std::string::npos) {
    return std::nullopt;
  }
  const auto total = text.find("total=", line);
  const auto end = text.find('\n', line);
  if (total == std::string::npos || (end != std::string::npos && total > end)) {
    return std::nullopt;
  }
  try {
    return static_cast<std::uint64_t>(std::stoull(text.substr(total + 6)));
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

std::vector<pid_t> child_pids_of(pid_t pid) {
  std::vector<pid_t> children;
  std::istringstream in(read_file("/proc/" + std::to_string(pid) + "/task/" + std::to_string(pid) + "/children"));
  pid_t child = 0;
  while (in >> child) {
    children.push_back(child);
  }
  return children;
}

// Fallback for hosts without PSI: threads in state R across the daemon and its descendants.
std::uint64_t count_runnable(pid_t root) {
  std::uint64_t runnable = 0;
  std::vector<pid_t> todo{root};
  for (std::size_t n = 0; n < todo.size() && n < 4096; ++n) {
    const pid_t pid = todo[n];
    const auto dir = "/proc/" + std::to_string(pid) + "/task";
    if (DIR* tasks = ::opendir(dir.c_str())) {
      while (const auto* entry = ::readdir(tasks)) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') {
          continue;
        }
        const auto stat = read_file(dir + "/" + entry->d_name + "/stat");
        const auto close = stat.rfind(')');
        if (close != std::string::npos && close + 2 < stat.size() && stat[close + 2] == 'R') {
          ++runnable;
        }
      }
      ::closedir(tasks);
    }
    for (const pid_t child : child_pids_of(pid)) {
      todo.push_back(child);
    }
  }
  return runnable;
}

std::uint64_t usec_delta_ns(std::uint64_t after, std::uint64_t before) {
  if (after < before) {
    return 0;
  }
  return (after - before) * 1000ULL;
}

std::string ring_archive_dir() { return rings_dir(); }

void monitor_daemon(const std::string& daemon_id, pid_t root) {
  // PSI decides the fourth counter for the life of this daemon: pressure, or runnable without PSI.
  const bool use_psi = read_cgroup_pressure(daemon_id).has_value();
  // cores is the cpuset size when the sample was taken, so the archive shows allocation beside use.
  const std::vector<std::string> daemon_names{"task-clock", "user", "system", use_psi ? "pressure" : "runnable", "cores"};
  std::shared_ptr<RingArchive> archive;
  const auto archive_path = rings_dir() + "/" + archive_name(daemon_id) + ".mring";
  try {
    archive = std::make_shared<RingArchive>(archive_path, daemon_names);
  } catch (const std::exception& first) {
    // An archive with other counters (an older layout, or the runnable fallback) is moved aside to
    // <id>.mring.<unix seconds> so this daemon is still recorded.
    const auto aside = archive_path + "." + std::to_string(std::time(nullptr));
    try {
      if (std::rename(archive_path.c_str(), aside.c_str()) != 0) {
        throw;
      }
      std::cout << "ring archive daemon_id=" << daemon_id << " " << first.what() << ", moved to " << aside << std::endl;
      archive = std::make_shared<RingArchive>(archive_path, daemon_names);
    } catch (const std::exception& error) {
      std::cerr << "ring archive daemon_id=" << daemon_id << " " << error.what() << std::endl;
    }
  }
  auto daemon_ring = std::make_shared<Ring>(kRingSlots, daemon_names, archive);
  {
    std::lock_guard lock(daemons_mu);
    if (auto* daemon = find_daemon(daemon_id); daemon != nullptr && daemon->pid == root) {
      daemon->ring = daemon_ring;
    }
  }
  std::cout << "counters daemon_id=" << daemon_id << " cgroup " << daemon_names.back() << std::endl;
  auto previous_cpu = read_cgroup_cpu(daemon_id);
  auto previous_pressure = use_psi ? read_cgroup_pressure(daemon_id) : std::nullopt;
  std::unique_ptr<GnnController> gnn;  // only while the gnn policy is attached; its destructor restores affinity
  long gnn_ticks = 0, gnn_logged = 0, gnn_warned = 0, throttle_ticks = 0, shift_ticks = 0, shift_logged = 0;
  std::int64_t gnn_read_ns = 0, gnn_infer_ns = 0, gnn_act_ns = 0, gnn_cpuset_ns = 0, gnn_last_log = 0;
  try {
    while (daemon_running(daemon_id, root)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(kSampleMs));
      if (!daemon_running(daemon_id, root)) {
        break;
      }
      const auto current_cpu = read_cgroup_cpu(daemon_id);
      const auto current_pressure = use_psi ? read_cgroup_pressure(daemon_id) : std::nullopt;
      const bool pressure_ok =
          !use_psi || (previous_pressure && current_pressure && *current_pressure >= *previous_pressure);
      if (previous_cpu && current_cpu && current_cpu->usage_usec >= previous_cpu->usage_usec && pressure_ok) {
        Sample total;
        total.timestamp_ns = monotonic_ns();
        total.nvalues = 5;
        total.value[0] = usec_delta_ns(current_cpu->usage_usec, previous_cpu->usage_usec);
        total.value[1] = usec_delta_ns(current_cpu->user_usec, previous_cpu->user_usec);
        total.value[2] = usec_delta_ns(current_cpu->system_usec, previous_cpu->system_usec);
        total.value[3] = use_psi ? usec_delta_ns(*current_pressure, *previous_pressure) : count_runnable(root);
        const auto cpus = cpuset_of(daemon_id);
        total.value[kCgroupCores] = static_cast<std::uint64_t>(cpus.size());
        note_cpuset(daemon_id, cpus);
        daemon_ring->push(total);
      }
      if (current_cpu) {
        previous_cpu = current_cpu;
      }
      if (current_pressure) {
        previous_pressure = current_pressure;
      }

      ScaleState scale;
      {
        std::lock_guard lock(daemons_mu);
        const auto* daemon = find_daemon(daemon_id);
        if (daemon == nullptr || daemon->pid != root) {
          break;
        }
        scale = daemon->scale;
      }
      if (!scale.enabled || !scale.policy || !scale.gnn) {
        if (gnn) {
          gnn.reset();  // detached or switched away: affinities back to the cpuset-wide mask
          std::cout << "gnn daemon_id=" << daemon_id << " detached, affinity restored" << std::endl;
        }
      }
      if (scale.throttle && ++throttle_ticks % 10 == 0) {
        // Judged every 100 ms on a full window that starts after the last attach, cut, or release.
        const auto now = monotonic_ns();
        const auto window_ns = static_cast<std::int64_t>(scale.throttle_window_ms) * 1000000LL;
        // Busy share of the scope's physical cores, over the scope's CPUs.
        const auto scope = throttle_scope();
        const auto load = now - scale.throttle_at_ns >= window_ns && !scope.empty()
                              ? load_over(names_touching(ResourceLevel::Core, scope), scope.size(), now - window_ns)
                              : std::nullopt;
        std::optional<bool> active;
        if (load && !scale.throttle_active && *load > scale.throttle_above) {
          const int cores = cpuset_cores(daemon_id);
          const auto quota = throttle_quota_us(cores, scale.throttle_cut);
          if (write_cpu_max(daemon_id, std::to_string(quota) + " " + std::to_string(kThrottlePeriodUs))) {
            active = true;
            std::cout << "throttle daemon_id=" << daemon_id << " system_load=" << *load << " scope=" << scope.size()
                      << " cut cores=" << cores
                      << " quota_us=" << quota << "/" << kThrottlePeriodUs << std::endl;
            note_event(daemon_id, "throttle", "cut to " + pct(1 - scale.throttle_cut) + " of " + std::to_string(cores) +
                                                  " CPUs, system load " + pct(*load) + " on " +
                                                  std::to_string(scope.size()) + " CPUs");
          }
        } else if (load && scale.throttle_active) {
          const double daemon_cpus = ring_cpus(*daemon_ring, now - window_ns);
          if (throttle_release_test(*load, daemon_cpus, static_cast<int>(scope.size()), scale.throttle_cut,
                                    scale.throttle_above) &&
              write_cpu_max(daemon_id, "max " + std::to_string(kThrottlePeriodUs))) {
            active = false;
            std::cout << "throttle daemon_id=" << daemon_id << " system_load=" << *load << " daemon_cpus=" << daemon_cpus
                      << " released" << std::endl;
            note_event(daemon_id, "throttle", "released, system load " + pct(*load));
          }
        }
        if (active) {
          std::lock_guard lock(daemons_mu);
          if (auto* daemon = find_daemon(daemon_id); daemon != nullptr && daemon->pid == root && daemon->scale.throttle) {
            daemon->scale.throttle_active = *active;
            daemon->scale.throttle_at_ns = monotonic_ns();
            scale.throttle_active = *active;
          }
        }
      }
      if (scale.shift && ++shift_ticks % 10 == 0) {
        // Judged every 100 ms on a full window that starts after the last attach or move.
        const auto now = monotonic_ns();
        const auto window_ns = static_cast<std::int64_t>(scale.shift_window_ms) * 1000000LL;
        auto have = now - scale.shift_at_ns >= window_ns ? cpuset_of(daemon_id) : std::vector<int>{};
        bool moved = false;
        if (!have.empty()) {
          std::lock_guard place(l3_mu);
          auto blocks = l3_blocks(daemon_id, have);
          std::vector<std::string> cooling;
          for (auto& block : blocks) {
            block.load = load_over({block.name}, block.cpus.size(), now - window_ns).value_or(0);
            if (const auto left = shift_left_at.find(block.name); left != shift_left_at.end() && now - left->second < window_ns) {
              cooling.push_back(block.name);
            }
          }
          const double daemon_cpus = ring_cpus(*daemon_ring, now - window_ns);
          const auto plan = plan_shift(blocks, have, daemon_cpus, scale.shift_above, cooling);
          if (plan && plan->dest) {
            for (const int cpu : plan->leave) {
              have.erase(std::find(have.begin(), have.end(), cpu));
            }
            have.insert(have.end(), plan->take.begin(), plan->take.end());
            std::sort(have.begin(), have.end());
            int after = 0;
            std::string spec;
            if (set_cpuset(daemon_id, have, after, spec)) {
              moved = true;
              shift_left_at[blocks[plan->source].name] = now;
              std::cout << "shift daemon_id=" << daemon_id << " from=" << blocks[plan->source].name
                        << " load=" << blocks[plan->source].load << " to=" << blocks[*plan->dest].name
                        << " load=" << blocks[*plan->dest].load << " daemon_cpus=" << daemon_cpus
                        << " cpus=" << spec << std::endl;
              note_event(daemon_id, "shift", blocks[plan->source].name + " (" + pct(blocks[plan->source].load) + ") -> " +
                                                 blocks[*plan->dest].name + " (" + pct(blocks[*plan->dest].load) + "), cpus " + spec);
            }
          } else if (plan && shift_logged++ % 10 == 0) {
            std::cout << "shift daemon_id=" << daemon_id << " from=" << blocks[plan->source].name
                      << " load=" << blocks[plan->source].load << " daemon_cpus=" << daemon_cpus
                      << " no domain can take it, holding" << std::endl;
          }
        }
        if (moved) {
          std::lock_guard lock(daemons_mu);
          if (auto* daemon = find_daemon(daemon_id); daemon != nullptr && daemon->pid == root) {
            daemon->scale.shift_at_ns = monotonic_ns();
            // The scaling policy also waits a fresh window on the new CPUs.
            daemon->scale.changed_at_ns = daemon->scale.shift_at_ns;
          }
        }
      }
      // While throttled the cpuset is held: the cut lowers occupancy, which would otherwise shrink it.
      if (!scale.enabled || !scale.policy || (scale.throttle && scale.throttle_active)) {
        continue;
      }
      if (scale.gnn) {
        if (!gnn) {
          std::string error;
          const auto weights = gnn_weights(error);
          if (!weights) {
            if (gnn_warned++ % 100 == 0) {
              std::cerr << "gnn daemon_id=" << daemon_id << " " << error << std::endl;
            }
            continue;
          }
          gnn = std::make_unique<GnnController>(daemon_id, weights);
          gnn_ticks = 0;
          std::cout << "gnn daemon_id=" << daemon_id << " attached tick_ms=" << scale.gnn_tick_ms << std::endl;
        }
        if (++gnn_ticks % std::max(1, scale.gnn_tick_ms / kSampleMs) != 0) {
          continue;
        }
        const auto t0 = monotonic_ns();
        int cores = cpuset_cores(daemon_id);
        GnnTickStats st;
        const int target = gnn->observe(cores, scale.min_cores, scale.max_cores, st);
        if (target < 0) {
          break;
        }
        const int before_cores = cores;
        int after = 0;
        std::string spec;
        while (cores < target && add_core(daemon_id, scale.max_cores, after, spec, scale.placement)) {
          cores = after;
        }
        while (cores > target && remove_core(daemon_id, scale.min_cores, after, spec)) {
          cores = after;
        }
        if (cores != before_cores) {
          std::lock_guard lock(daemons_mu);
          if (auto* daemon = find_daemon(daemon_id); daemon != nullptr && daemon->pid == root) {
            daemon->scale.changed_at_ns = monotonic_ns();
          }
        }
        const auto t1 = monotonic_ns();
        if (const auto cpus = parse_cpuset(trimmed(read_file("/sys/fs/cgroup/mercury/" + daemon_id + "/cpuset.cpus")))) {
          if (scale.gnn_pin) {
            gnn->pin(*cpus);
          } else {
            gnn->restore();  // no pins: widen anything pinned earlier (cheap when nothing is)
          }
        }
        const auto t2 = monotonic_ns();
        gnn_read_ns += st.read_ns;
        gnn_infer_ns += st.infer_ns;
        gnn_act_ns += t2 - t0 - st.read_ns - st.infer_ns;
        gnn_cpuset_ns += t1 - t0 - st.read_ns - st.infer_ns;
        ++gnn_logged;
        if (cores != before_cores || t2 - gnn_last_log >= 1000000000LL) {
          std::cout << "gnn daemon_id=" << daemon_id << " cores=" << cores << " required=" << st.required
                    << " target=" << target << " demand=" << st.demand_sum << " pred=" << st.pred_sum << " breaks=" << st.breaks << " nodes=" << st.nodes
                    << " threads=" << st.threads << " ticks=" << gnn_logged
                    << " mean_us read=" << gnn_read_ns / 1000 / gnn_logged << " infer=" << gnn_infer_ns / 1000 / gnn_logged
                    << " cpuset=" << gnn_cpuset_ns / 1000 / gnn_logged << " act=" << gnn_act_ns / 1000 / gnn_logged
                    << std::endl;
          gnn_last_log = t2;
          gnn_read_ns = gnn_infer_ns = gnn_act_ns = gnn_cpuset_ns = 0;
          gnn_logged = 0;
        }
        continue;
      }
      OccupancyWindow window;
      window.cores = cpuset_cores(daemon_id);
      window.min_cores = scale.min_cores;
      window.max_cores = scale.max_cores;
      window.runnable = !use_psi;
      for (const auto& sample : daemon_ring->recent(static_cast<std::size_t>(kScaleWindow) + 1)) {
        if (sample.timestamp_ns > scale.changed_at_ns) {
          window.cgroup.push_back(sample);
        }
      }
      const int before = window.cores;
      const auto decision = scale.policy->decide(window);
      if (decision == Decision::Hold) {
        continue;
      }
      int after = 0;
      std::string spec;
      const bool changed = decision == Decision::AddCore
                               ? add_core(daemon_id, scale.max_cores, after, spec, scale.placement)
                               : remove_core(daemon_id, scale.min_cores, after, spec);
      if (!changed) {
        continue;
      }
      std::cout << "scale daemon_id=" << daemon_id << " cores=" << after << " cpus=" << spec << std::endl;
      note_event(daemon_id, "saturate", (decision == Decision::AddCore ? "added a CPU, cpus " : "removed a CPU, cpus ") + spec);
      // Every change, up or down, waits for a fresh full window.
      (void)before;
      {
        std::lock_guard lock(daemons_mu);
        if (auto* daemon = find_daemon(daemon_id); daemon != nullptr && daemon->pid == root) {
          daemon->scale.changed_at_ns = monotonic_ns();
        }
      }
    }
  } catch (const std::exception& error) {
    std::cerr << "counters daemon_id=" << daemon_id << " " << error.what() << std::endl;
  }
  daemon_ring->flush();
  note_cpuset(daemon_id, {});
  mark_counters_done(daemon_id, root);
}

void arm_daemon(const std::string& daemon_id, const std::string& exec_path, int cpu, int listen_fd, pid_t pid,
                int out_fd, int err_fd, std::vector<std::pair<std::string, std::string>> env) {
  {
    std::lock_guard lock(daemons_mu);
    Daemon created;
    created.daemon_id = daemon_id;
    created.exec_path = exec_path;
    created.pid = pid;
    created.running = true;
    created.cpu = cpu;
    created.listen_fd = listen_fd;
    created.env = std::move(env);
    created.scale.enabled = true;
    created.scale.min_cores = 1;
    created.scale.max_cores = kDefaultMaxCores;
    created.scale.policy = make_policy("saturate");
    created.scale.changed_at_ns = monotonic_ns();
    if (auto* daemon = find_daemon(daemon_id)) {
      if (daemon->listen_fd >= 0 && daemon->listen_fd != listen_fd) {
        ::close(daemon->listen_fd);
      }
      *daemon = std::move(created);
    } else {
      daemons.push_back(std::move(created));
    }
  }
  std::thread(watch_daemon, daemon_id, pid, out_fd, err_fd).detach();
  std::thread(monitor_daemon, daemon_id, pid).detach();
}

void discard_launch(Launched launched) {
  ::kill(launched.pid, SIGKILL);
  ::waitpid(launched.pid, nullptr, 0);
  ::close(launched.out_fd);
  ::close(launched.err_fd);
}

std::optional<std::string> start_route(const std::string& request) {
  if (request_target(request) != "/start") {
    return std::nullopt;
  }

  const auto body = request_body(request);
  const auto daemon_id = json_value(body, "daemon_id");
  const auto exec_path = json_value(body, "exec_path");
  const auto cpu_text = json_value(body, "cpu");
  if (!daemon_id || !exec_path || exec_path->empty() || !cpu_text) {
    return http_response(400, "Bad Request", R"({"pid":0})", kJson);
  }

  int cpu = 0;
  try {
    cpu = std::stoi(*cpu_text);
  } catch (const std::exception&) {
    return http_response(400, "Bad Request", R"({"pid":0})", kJson);
  }
  if (std::find(daemon_cpus.begin(), daemon_cpus.end(), cpu) == daemon_cpus.end() ||
      ::access(exec_path->c_str(), X_OK) != 0) {
    return http_response(400, "Bad Request", R"({"pid":0})", kJson);
  }
  auto env = parse_env(body);
  if (!env.ok) {
    return http_response(400, "Bad Request", R"({"pid":0})", kJson);
  }
  // Every daemon learns where Mercury writes its policy (the degrade system's disabled routes): merged into the
  // inherited environment, so a program that ignores it is unaffected.
  if (std::none_of(env.vars.begin(), env.vars.end(), [](const auto& v) { return v.first == "MERCURY_POLICY"; })) {
    env.vars.emplace_back("MERCURY_POLICY", policy_path(*daemon_id));
  }
  // With the l3 pool on, a start CPU the daemon may not have becomes the allowed CPU the fewest daemons hold
  // (see cpus_route).
  if (const auto excluded = pool_exclude(*daemon_id);
      !excluded.empty() && std::find(excluded.begin(), excluded.end(), cpu) != excluded.end()) {
    if (const auto pick = pool_pick(excluded, {})) {
      std::cout << "start daemon_id=" << *daemon_id << " pool remapped cpu " << cpu << " -> " << *pick << std::endl;
      cpu = *pick;
    }
  }
  {
    std::lock_guard lock(daemons_mu);
    if (const auto* daemon = find_daemon(*daemon_id); daemon != nullptr && daemon->running) {
      return http_response(400, "Bad Request", R"({"pid":0})", kJson);
    }
  }

  int listen_fd = -1;
  if (const auto port_text = json_value(body, "listen_port")) {
    int port = 0;
    try {
      port = std::stoi(*port_text);
    } catch (const std::exception&) {
      return http_response(400, "Bad Request", R"({"pid":0})", kJson);
    }
    if (port < 1 || port > 65535) {
      return http_response(400, "Bad Request", R"({"pid":0})", kJson);
    }
    listen_fd = bind_loopback(port);
    if (listen_fd < 0) {
      return http_response(500, "Internal Server Error", R"({"pid":0})", kJson);
    }
  }

  const auto launched = launch(*daemon_id, *exec_path, cpu, listen_fd, env.vars);
  if (!launched) {
    if (listen_fd >= 0) {
      ::close(listen_fd);
    }
    return http_response(500, "Internal Server Error", R"({"pid":0})", kJson);
  }
  arm_daemon(*daemon_id, *exec_path, cpu, listen_fd, launched->pid, launched->out_fd, launched->err_fd, env.vars);
  return http_response(200, "OK", "{\"pid\":" + std::to_string(launched->pid) + "}", kJson);
}

std::optional<std::string> migrate_route(const std::string& request) {
  if (request_target(request) != "/migrate") {
    return std::nullopt;
  }
  const auto body = request_body(request);
  const auto daemon_id = json_value(body, "daemon_id");
  const auto cpu_text = json_value(body, "cpu");
  if (!daemon_id || !safe_daemon_id(*daemon_id) || !cpu_text) {
    return http_response(400, "Bad Request", R"({"pid":0})", kJson);
  }
  int cpu = 0;
  try {
    cpu = std::stoi(*cpu_text);
  } catch (const std::exception&) {
    return http_response(400, "Bad Request", R"({"pid":0})", kJson);
  }
  if (std::find(daemon_cpus.begin(), daemon_cpus.end(), cpu) == daemon_cpus.end()) {
    return http_response(400, "Bad Request", R"({"pid":0})", kJson);
  }

  pid_t pid = -1;
  int old_cpu = 0;
  {
    std::lock_guard lock(daemons_mu);
    const auto* daemon = find_daemon(*daemon_id);
    if (daemon == nullptr || !daemon->running || daemon->cpu == cpu) {
      return http_response(400, "Bad Request", R"({"pid":0})", kJson);
    }
    pid = daemon->pid;
    old_cpu = daemon->cpu;
  }

  const auto dir = ensure_daemon_cpuset(*daemon_id);
  if (!dir) {
    std::cerr << "migrate daemon_id=" << *daemon_id << " cgroup setup failed" << std::endl;
    return http_response(500, "Internal Server Error", R"({"pid":0})", kJson);
  }
  const std::vector<int> want{cpu};
  const auto path = *dir + "/cpuset.cpus";
  if (!write_cpus_and_mems(*dir, std::to_string(cpu))) {
    std::cerr << "migrate daemon_id=" << *daemon_id << " write " << path << " failed" << std::endl;
    return http_response(500, "Internal Server Error", R"({"pid":0})", kJson);
  }
  bool applied = false;
  for (int attempt = 0; attempt < 50; ++attempt) {
    if (cpuset_applied(*dir, want)) {
      applied = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (!applied) {
    std::cerr << "migrate daemon_id=" << *daemon_id << " cpuset did not take effect" << std::endl;
    return http_response(500, "Internal Server Error", R"({"pid":0})", kJson);
  }

  {
    std::lock_guard lock(daemons_mu);
    auto* daemon = find_daemon(*daemon_id);
    if (daemon == nullptr || !daemon->running || daemon->pid != pid) {
      return http_response(500, "Internal Server Error", R"({"pid":0})", kJson);
    }
    daemon->cpu = cpu;
  }
  std::cout << "migrated daemon_id=" << *daemon_id << " pid " << pid << " cpu " << old_cpu << " -> " << cpu
            << std::endl;
  return http_response(200, "OK", "{\"pid\":" + std::to_string(pid) + "}", kJson);
}

static std::string resource_choice(const std::optional<ResourceGroupState>& state) {
  if (!state) {
    return "none";
  }
  std::ostringstream out;
  out << std::fixed << std::setprecision(2) << state->name << "=" << state->load;
  return out.str();
}

std::optional<std::string> cpus_route(const std::string& request) {
  if (request_target(request) != "/cpus") {
    return std::nullopt;
  }
  const auto body = request_body(request);
  const auto daemon_id = json_value(body, "daemon_id");
  const auto cpus_text = json_value(body, "cpus");
  if (!daemon_id || !safe_daemon_id(*daemon_id) || !cpus_text) {
    return http_response(400, "Bad Request", ok_json(false), kJson);
  }
  auto want = parse_cpuset(trimmed(*cpus_text));
  if (!want || (mercury_linux_cpu >= 0 &&
                std::find(want->begin(), want->end(), mercury_linux_cpu) != want->end())) {
    return http_response(400, "Bad Request", ok_json(false), kJson);
  }
  if (!resource_groups_see(*want)) {
    std::cerr << "cpus daemon_id=" << *daemon_id << " cpus are not in a resource group" << std::endl;
    return http_response(400, "Bad Request", ok_json(false), kJson);
  }
  // With the l3 pool on, a CPU the daemon may not have (outside the pool; inside it for an exempt daemon) is
  // replaced by the allowed CPU the fewest daemons hold (pool_pick), so an operator's fixed home CPUs (hotel: one
  // per service, 1-15) spread over the pool.
  if (const auto excluded = pool_exclude(*daemon_id); !excluded.empty()) {
    std::vector<int> mapped;
    for (const int cpu : *want) {
      if (std::find(excluded.begin(), excluded.end(), cpu) == excluded.end()) {
        mapped.push_back(cpu);
      }
    }
    for (std::size_t missing = want->size() - mapped.size(); missing > 0; --missing) {
      const auto pick = pool_pick(excluded, mapped);
      if (!pick) {
        break;
      }
      mapped.push_back(*pick);
    }
    std::sort(mapped.begin(), mapped.end());
    if (!mapped.empty() && mapped != *want) {
      std::cout << "cpus daemon_id=" << *daemon_id << " pool remapped " << format_cpuset(*want) << " -> "
                << format_cpuset(mapped) << std::endl;
      note_event(*daemon_id, "pool", "cpus " + format_cpuset(*want) + " remapped into the pool: " + format_cpuset(mapped));
      want = mapped;
    }
  }
  const auto spec_text = format_cpuset(*want);

  const auto dir = ensure_daemon_cpuset(*daemon_id);
  if (!dir) {
    std::cerr << "cpus daemon_id=" << *daemon_id << " cgroup setup failed" << std::endl;
    return http_response(500, "Internal Server Error", ok_json(false), kJson);
  }
  const auto path = *dir + "/cpuset.cpus";
  if (!write_cpus_and_mems(*dir, spec_text)) {
    std::cerr << "cpus daemon_id=" << *daemon_id << " write " << path << " failed" << std::endl;
    return http_response(500, "Internal Server Error", ok_json(false), kJson);
  }
  for (int attempt = 0; attempt < 50; ++attempt) {
    if (cpuset_applied(*dir, *want)) {
      std::cout << "cpus daemon_id=" << *daemon_id << " cpus=" << spec_text << " socket "
                << resource_choice(least_loaded_touching(ResourceLevel::Socket, *want)) << " node "
                << resource_choice(least_loaded_touching(ResourceLevel::Node, *want)) << " l3 "
                << resource_choice(least_loaded_touching(ResourceLevel::L3, *want)) << " core "
                << resource_choice(least_loaded_touching(ResourceLevel::Core, *want)) << std::endl;
      return http_response(200, "OK", ok_json(true), kJson);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  std::cerr << "cpus daemon_id=" << *daemon_id << " cpuset did not take effect" << std::endl;
  return http_response(500, "Internal Server Error", ok_json(false), kJson);
}

int scale_int(const std::string& body, const char* key, int fallback, bool& bad) {
  const auto text = json_value(body, key);
  if (!text) {
    return fallback;
  }
  try {
    std::size_t used = 0;
    const int value = std::stoi(*text, &used);
    if (used != text->size()) {
      bad = true;
      return fallback;
    }
    return value;
  } catch (const std::exception&) {
    bad = true;
    return fallback;
  }
}

double scale_double(const std::string& body, const char* key, double fallback, bool& bad) {
  const auto text = json_value(body, key);
  if (!text) {
    return fallback;
  }
  try {
    std::size_t used = 0;
    const double value = std::stod(*text, &used);
    if (used != text->size()) {
      bad = true;
      return fallback;
    }
    return value;
  } catch (const std::exception&) {
    bad = true;
    return fallback;
  }
}

// The gnn model, loaded once from gnn_model_path() (reloaded if that file changes).
std::shared_ptr<const GnnWeights> gnn_weights(std::string& error) {
  static std::mutex mu;
  static std::shared_ptr<const GnnWeights> cached;
  static std::string cached_path;
  static std::int64_t cached_mtime = 0;
  std::lock_guard lock(mu);
  const auto path = gnn_model_path();
  struct stat sb {};
  if (::stat(path.c_str(), &sb) != 0) {
    error = "cannot stat " + path;
    return nullptr;
  }
  if (cached && cached_path == path && cached_mtime == static_cast<std::int64_t>(sb.st_mtime)) {
    return cached;
  }
  auto w = load_gnn_weights(path, error);
  if (w) {
    cached = w;
    cached_path = path;
    cached_mtime = static_cast<std::int64_t>(sb.st_mtime);
    std::cout << "gnn model " << path << " K=" << w->K << " F=" << w->F << " H=" << w->H << std::endl;
  }
  return w;
}

std::string scale_json(int min_cores, int max_cores, int cores, int threads) {
  return "{\"ok\":true,\"min\":" + std::to_string(min_cores) + ",\"max\":" + std::to_string(max_cores) +
         ",\"cores\":" + std::to_string(cores) + ",\"threads\":" + std::to_string(threads) + "}";
}

std::optional<std::string> scale_route(const std::string& request) {
  if (request_target(request) != "/scale") {
    return std::nullopt;
  }
  const auto body = request_body(request);
  const auto daemon_id = json_value(body, "daemon_id");
  if (!daemon_id || !safe_daemon_id(*daemon_id)) {
    return http_response(400, "Bad Request", ok_json(false), kJson);
  }
  bool bad = false;
  const int min_cores = scale_int(body, "min", 1, bad);
  const int max_cores = scale_int(body, "max", kDefaultMaxCores, bad);
  const int tick_ms = scale_int(body, "tick_ms", 10, bad);
  const auto pin_text = json_value(body, "pin");
  const bool gnn_pin = !(pin_text && (*pin_text == "false" || *pin_text == "0"));
  const auto policy_name = json_value(body, "policy");
  const std::string name = policy_name ? *policy_name : "saturate";
  // placement is optional; omitted keeps the daemon's current placement.
  const auto placement_text = json_value(body, "placement");
  std::optional<CpuPlacement> placement;
  if (placement_text) {
    placement = parse_placement(*placement_text);
    if (!placement) {
      return http_response(400, "Bad Request", ok_json(false), kJson);
    }
  }
  if (bad || min_cores < 1 || max_cores < min_cores || tick_ms < kSampleMs) {
    return http_response(400, "Bad Request", ok_json(false), kJson);
  }
  const bool is_gnn = name == "gnn";
  if (is_gnn) {
    std::string error;
    if (!gnn_weights(error)) {
      std::cerr << "scale daemon_id=" << *daemon_id << " gnn model: " << error << std::endl;
      return http_response(500, "Internal Server Error", "{\"ok\":false,\"error\":\"gnn model not loadable\"}", kJson);
    }
  }
  // "none" detaches saturate from this daemon, for callers that hold a cpuset fixed.
  // "full" detaches it too and gives the daemon the whole pool (min and max do not apply).
  const bool is_full = name == "full";
  const auto policy = make_policy(name);
  if (!policy && name != "none" && !is_full) {
    return http_response(400, "Bad Request", ok_json(false), kJson);
  }
  if (!resources_ready() || !least_loaded(ResourceLevel::Core)) {
    std::cerr << "scale daemon_id=" << *daemon_id << " resource groups are not available" << std::endl;
    return http_response(500, "Internal Server Error", ok_json(false), kJson);
  }

  {
    std::lock_guard lock(daemons_mu);
    auto* daemon = find_daemon(*daemon_id);
    if (daemon == nullptr || !daemon->running) {
      return http_response(400, "Bad Request", ok_json(false), kJson);
    }
    daemon->scale.enabled = policy != nullptr;
    daemon->scale.min_cores = min_cores;
    daemon->scale.max_cores = max_cores;
    daemon->scale.policy = policy;
    daemon->scale.gnn = is_gnn;
    daemon->scale.gnn_tick_ms = (tick_ms / kSampleMs) * kSampleMs;
    daemon->scale.gnn_pin = gnn_pin;
    if (placement) {
      daemon->scale.placement = *placement;
    } else if (is_gnn) {
      daemon->scale.placement = CpuPlacement::LeastLoaded;  // gnn defaults to the least_loaded hand-out policy
    }
    placement = daemon->scale.placement;
  }
  // Written after the policy is detached above, so the monitor no longer adds or removes cores.
  if (is_full) {
    int after = 0;
    std::string spec;
    if (!set_full_pool(*daemon_id, after, spec)) {
      std::cerr << "scale daemon_id=" << *daemon_id << " full: cpuset write failed" << std::endl;
      return http_response(500, "Internal Server Error", ok_json(false), kJson);
    }
    {
      std::lock_guard lock(daemons_mu);
      if (auto* daemon = find_daemon(*daemon_id)) {
        daemon->scale.changed_at_ns = monotonic_ns();
      }
    }
    std::cout << "scale daemon_id=" << *daemon_id << " policy=full cores=" << after << " cpus=" << spec << std::endl;
    note_event(*daemon_id, "policy", "full pool, cpus " + spec);
  }
  note_event(*daemon_id, "policy", "scale policy " + name + ", cpus " + format_cpuset(cpuset_of(*daemon_id)));
  std::cout << "scale daemon_id=" << *daemon_id << " placement=" << placement_name(*placement) << " least socket "
            << resource_choice(least_loaded(ResourceLevel::Socket)) << " node "
            << resource_choice(least_loaded(ResourceLevel::Node)) << " l3 "
            << resource_choice(least_loaded(ResourceLevel::L3)) << " core "
            << resource_choice(least_loaded(ResourceLevel::Core)) << std::endl;
  auto json = scale_json(min_cores, max_cores, cpuset_cores(*daemon_id), 0);
  json.insert(json.size() - 1, std::string(",\"placement\":\"") + placement_name(*placement) + "\",\"policy\":\"" + name + "\"" +
                                   (is_gnn ? ",\"tick_ms\":" + std::to_string((tick_ms / kSampleMs) * kSampleMs) + ",\"pin\":" + (gnn_pin ? "true" : "false") : std::string()));
  return http_response(200, "OK", json, kJson);
}

// POST /throttle {daemon_id, above, cut, window_ms}. cut 0 detaches and lifts any cut. Independent of /scale.
std::optional<std::string> throttle_route(const std::string& request) {
  if (request_target(request) != "/throttle") {
    return std::nullopt;
  }
  const auto body = request_body(request);
  const auto daemon_id = json_value(body, "daemon_id");
  if (!daemon_id || !safe_daemon_id(*daemon_id)) {
    return http_response(400, "Bad Request", ok_json(false), kJson);
  }
  bool bad = false;
  const double above = scale_double(body, "above", kThrottleAbove, bad);
  const double cut = scale_double(body, "cut", kThrottleCut, bad);
  const int window_ms = scale_int(body, "window_ms", kThrottleWindowMs, bad);
  if (bad || !(above > 0 && above < 1) || !(cut >= 0 && cut < 1) || window_ms < kSampleMs ||
      window_ms > kHistorySeconds * 1000) {
    return http_response(400, "Bad Request", ok_json(false), kJson);
  }
  if (!resources_ready()) {
    return http_response(500, "Internal Server Error", ok_json(false), kJson);
  }
  const bool attach = cut > 0;
  bool was_active = false;
  {
    std::lock_guard lock(daemons_mu);
    auto* daemon = find_daemon(*daemon_id);
    if (daemon == nullptr || !daemon->running) {
      return http_response(400, "Bad Request", ok_json(false), kJson);
    }
    was_active = daemon->scale.throttle_active;
    daemon->scale.throttle = attach;
    daemon->scale.throttle_active = attach && was_active;
    daemon->scale.throttle_above = above;
    daemon->scale.throttle_cut = cut;
    daemon->scale.throttle_window_ms = window_ms;
    daemon->scale.throttle_at_ns = monotonic_ns();
  }
  // A cut in force keeps its old quota until the next release; detaching lifts it now.
  if (!attach && !write_cpu_max(*daemon_id, "max " + std::to_string(kThrottlePeriodUs))) {
    std::cerr << "throttle daemon_id=" << *daemon_id << " cpu.max write failed" << std::endl;
    return http_response(500, "Internal Server Error", ok_json(false), kJson);
  }
  note_event(*daemon_id, "policy", attach ? "throttle attached above " + pct(above) + ", cut " + pct(cut) : "throttle detached");
  std::cout << "throttle daemon_id=" << *daemon_id << (attach ? " attached" : " detached") << " above=" << above
            << " cut=" << cut << " window_ms=" << window_ms << std::endl;
  std::ostringstream json;
  json << "{\"ok\":true,\"above\":" << above << ",\"cut\":" << cut << ",\"window_ms\":" << window_ms
       << ",\"active\":" << (attach && was_active ? "true" : "false") << "}";
  return http_response(200, "OK", json.str(), kJson);
}

static std::vector<std::string> split_list(const std::string& text, char sep) {
  std::vector<std::string> out;
  std::size_t at = 0;
  while (at <= text.size()) {
    const auto end = std::min(text.find(sep, at), text.size());
    if (end > at) {
      out.push_back(text.substr(at, end - at));
    }
    at = end + 1;
  }
  return out;
}

// up and down in (0, 1) with down below up; both windows within the ring.
static bool pool_config_ok(const PoolState& c) {
  const int ring_ms = kHistorySeconds * 1000;
  return c.up > 0 && c.up < 1 && c.down >= 0 && c.down < c.up && c.pressure_up > 0 && c.pressure_up <= 1 &&
         c.window_ms >= kSampleMs && c.window_ms <= ring_ms &&
         c.down_window_ms >= kSampleMs && c.down_window_ms <= ring_ms;
}

static std::string pool_json() {
  std::lock_guard lock(pool_mu);
  std::ostringstream out;
  out << "{\"ok\":true,\"policy\":\"" << (!pool.enabled ? "off" : pool.grow ? "l3" : "fixed") << "\",\"swap\":"
      << (pool.swap ? "true" : "false") << ",\"cores\":" << pool.active.size()
      << ",\"cpus\":\"" << format_cpuset(pool.active) << "\",\"start\":" << pool.start << ",\"up\":" << pool.up
      << ",\"window_ms\":" << pool.window_ms << ",\"max\":" << pool.max << ",\"pressure\":" << pool.pressure_up
      << ",\"down\":" << pool.down
      << ",\"down_window_ms\":" << pool.down_window_ms
      << ",\"grown\":" << pool.grown.size() << ",\"exempt\":[";
  for (std::size_t i = 0; i < pool.exempt.size(); ++i) {
    out << (i ? "," : "") << '"' << pool.exempt[i] << '"';
  }
  out << "]}";
  return out.str();
}

// GET /pool: the pool. POST /pool {policy: "l3" | "fixed" | "off", start, up, window_ms, pressure, down,
// down_window_ms, swap, exempt: "a,b", seed_cpu}: fixed keeps the start size (swap: true lets the shift system
// swap a domain loaded from outside Mercury for a clean one);
// l3 rebuilds the pool from start CPUs of whole L3 domains; off lets add_core use every allocatable CPU again.
// Daemons keep the CPUs they hold either way.
std::optional<std::string> pool_route(const std::string& request) {
  if (request_target(request) != "/pool") {
    return std::nullopt;
  }
  const auto body = request_body(request);
  const auto policy = json_value(body, "policy");
  if (!policy) {
    return http_response(200, "OK", pool_json(), kJson);
  }
  if (*policy == "off") {
    {
      std::lock_guard lock(pool_mu);
      pool.enabled = false;
    }
    std::cout << "pool off" << std::endl;
    note_event("", "pool", "pool off");
    return http_response(200, "OK", pool_json(), kJson);
  }
  bool bad = *policy != "l3" && *policy != "fixed";
  PoolState config;
  config.grow = *policy == "l3";
  config.pressure_up = scale_double(body, "pressure", kPoolAddPressure, bad);
  config.max = static_cast<std::size_t>(std::max(0, scale_int(body, "max", 0, bad)));
  if (const auto swap = json_value(body, "swap")) {
    bad = bad || (*swap != "true" && *swap != "false");
    config.swap = *swap == "true";
  }
  const int start = scale_int(body, "start", kPoolStartCpus, bad);
  config.up = scale_double(body, "up", kPoolAddLoad, bad);
  config.window_ms = scale_int(body, "window_ms", kPoolWindowMs, bad);
  config.down = scale_double(body, "down", kPoolRemoveLoad, bad);
  config.down_window_ms = scale_int(body, "down_window_ms", kPoolRemoveWindowMs, bad);
  const int seed_cpu = scale_int(body, "seed_cpu", -1, bad);
  config.exempt = split_list(json_value(body, "exempt").value_or(""), ',');
  if (bad || start < 1 || (config.max > 0 && config.max < static_cast<std::size_t>(start)) || !pool_config_ok(config)) {
    return http_response(400, "Bad Request", ok_json(false), kJson);
  }
  config.start = static_cast<std::size_t>(start);
  std::string spec;
  if (!enable_pool(config, seed_cpu, spec)) {
    return http_response(500, "Internal Server Error", ok_json(false), kJson);
  }
  return http_response(200, "OK", pool_json(), kJson);
}

// MERCURY_POOL="l3|fixed[,start=8][,max=0][,up=0.9][,window_ms=1000][,pressure=0.25][,down=0.1][,down_window_ms=30000]
// [,swap=1][,seed_cpu=2][,exempt=hotel-loadgen:hotel-jaeger]" turns
// the pool on at startup, before any daemon starts (the operator places services as soon as it connects).
void init_pool_from_env() {
  const char* env = std::getenv("MERCURY_POOL");
  if (env == nullptr || std::string(env).empty()) {
    return;
  }
  const auto items = split_list(env, ',');
  if (items.empty() || (items.front() != "l3" && items.front() != "fixed")) {
    std::cerr << "MERCURY_POOL: unknown policy in " << env << std::endl;
    return;
  }
  PoolState config;
  config.grow = items.front() == "l3";
  int seed_cpu = -1;
  try {
    for (std::size_t i = 1; i < items.size(); ++i) {
      const auto eq = items[i].find('=');
      const auto key = items[i].substr(0, eq);
      const auto value = eq == std::string::npos ? std::string() : items[i].substr(eq + 1);
      if (key == "start") {
        config.start = std::stoul(value);
      } else if (key == "up") {
        config.up = std::stod(value);
      } else if (key == "window_ms") {
        config.window_ms = std::stoi(value);
      } else if (key == "down") {
        config.down = std::stod(value);
      } else if (key == "down_window_ms") {
        config.down_window_ms = std::stoi(value);
      } else if (key == "pressure") {
        config.pressure_up = std::stod(value);
      } else if (key == "max") {
        config.max = std::stoul(value);
      } else if (key == "swap") {
        config.swap = value == "1" || value == "true";
      } else if (key == "seed_cpu") {
        seed_cpu = std::stoi(value);
      } else if (key == "exempt") {
        config.exempt = split_list(value, ':');
      } else {
        throw std::invalid_argument(key);
      }
    }
  } catch (const std::exception& error) {
    std::cerr << "MERCURY_POOL: bad item " << error.what() << " in " << env << std::endl;
    return;
  }
  if (config.start < 1 || (config.max > 0 && config.max < config.start) || !pool_config_ok(config)) {
    std::cerr << "MERCURY_POOL: values out of range in " << env << std::endl;
    return;
  }
  std::string spec;
  if (!enable_pool(config, seed_cpu, spec)) {
    std::cerr << "MERCURY_POOL: resource groups are not ready" << std::endl;
  }
}

// degrade (SPEC.md "degrade"): the brownout baseline at the application's entry. While the system Mercury runs is
// above `above` busy, one more optional route of `routes` (in shedding order) is disabled per window: Mercury writes
// it into the policy file of every daemon in `daemon_id` (a comma-separated list; policy_path, which every daemon gets
// as MERCURY_POLICY) and those daemons reject the route's requests at once: the hotel frontend with 503, the social
// network's entry services (UserTimelineService, SocialGraphService) with a ServiceException. The load a route removed is measured a window after it was disabled; the
// route disabled last is enabled again only when the load plus that cost stays below the line for `hold` windows.
struct DegradeState {
  bool enabled = false;
  std::string daemon_id;  // as given: one id, or several comma-separated
  std::vector<std::string> daemons;
  std::vector<std::string> routes;
  double above = 0.8;
  int window_ms = 1000;
  int hold = 5;
  std::vector<std::string> disabled;
  std::map<std::string, double> cost;
  std::optional<std::pair<std::string, double>> measuring;  // route just disabled, load before
  int calm = 0;
  std::int64_t changed_at_ns = 0;
};
std::mutex degrade_mu;
DegradeState degrade_state;

// The busy share of the system: the pool's CPUs when there is a pool, else every CPU a daemon holds.
std::optional<double> system_load(std::int64_t from_ns) {
  const auto scope = throttle_scope();
  if (scope.empty()) {
    return std::nullopt;
  }
  return load_over(names_touching(ResourceLevel::Core, scope), scope.size(), from_ns);
}

bool write_policy(const std::string& daemon_id, const std::vector<std::string>& disabled) {
  std::ostringstream body;
  body << "{\"disabled\":[";
  for (std::size_t i = 0; i < disabled.size(); ++i) {
    body << (i ? "," : "") << '"' << disabled[i] << '"';
  }
  body << "]}\n";
  const auto path = policy_path(daemon_id);
  const auto tmp = path + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) {
      return false;
    }
    out << body.str();
  }
  ::chmod(tmp.c_str(), 0644);
  return std::rename(tmp.c_str(), path.c_str()) == 0;
}

void degrade_loop() {
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    DegradeState st;
    {
      std::lock_guard lock(degrade_mu);
      st = degrade_state;
    }
    const auto now = monotonic_ns();
    const auto window_ns = static_cast<std::int64_t>(st.window_ms) * 1000000LL;
    if (!st.enabled || now - st.changed_at_ns < window_ns) {
      continue;
    }
    const auto load = system_load(now - window_ns);
    if (!load) {
      continue;
    }
    std::lock_guard lock(degrade_mu);
    auto& d = degrade_state;
    if (!d.enabled || d.daemon_id != st.daemon_id) {
      continue;
    }
    if (d.measuring) {
      d.cost[d.measuring->first] = std::max(0.02, d.measuring->second - *load);
      d.measuring.reset();
    }
    std::string change;
    if (*load > d.above && d.disabled.size() < d.routes.size()) {
      const auto route = d.routes[d.disabled.size()];
      d.disabled.push_back(route);
      d.measuring = std::make_pair(route, *load);
      d.calm = 0;
      change = "disabled " + route;
    } else if (!d.disabled.empty()) {
      const auto& route = d.disabled.back();
      const double cost = d.cost.count(route) ? d.cost[route] : 0.1;
      d.calm = *load + cost <= d.above - 0.05 ? d.calm + 1 : 0;
      if (d.calm >= d.hold) {
        change = "enabled " + route;
        d.disabled.pop_back();
        d.calm = 0;
      }
    }
    if (change.empty()) {
      continue;
    }
    d.changed_at_ns = monotonic_ns();
    for (const auto& id : d.daemons) {
      if (!write_policy(id, d.disabled)) {
        std::cerr << "degrade daemon_id=" << id << " cannot write " << policy_path(id) << std::endl;
      }
    }
    std::ostringstream disabled;
    for (std::size_t i = 0; i < d.disabled.size(); ++i) {
      disabled << (i ? "," : "") << d.disabled[i];
    }
    std::cout << "degrade daemon_id=" << d.daemon_id << " " << change << " load=" << *load << " disabled=[" << disabled.str()
              << "] t_ns=" << monotonic_ns() << std::endl;
    note_event(d.daemon_id, "degrade", change + " at load " + pct(*load) + ", disabled [" + disabled.str() + "]");
  }
}

static void append_quoted(std::ostringstream& out, const std::vector<std::string>& values);

// GET /degrade: the state. POST /degrade {daemon_id: "a" or "a,b", routes: "a,b", above, window_ms, hold, enabled}:
// routes in the order they are shed, written to every daemon of daemon_id. enabled false stops and enables every
// route again.
std::optional<std::string> degrade_route(const std::string& request) {
  if (request_target(request) != "/degrade") {
    return std::nullopt;
  }
  const auto body = request_body(request);
  const auto daemon_id = json_value(body, "daemon_id");
  if (daemon_id) {
    DegradeState next;
    next.daemon_id = *daemon_id;
    next.daemons = split_list(*daemon_id, ',');
    if (next.daemons.empty() ||
        std::any_of(next.daemons.begin(), next.daemons.end(), [](const auto& id) { return !safe_daemon_id(id); })) {
      return http_response(400, "Bad Request", ok_json(false), kJson);
    }
    bool bad = false;
    next.routes = split_list(json_value(body, "routes").value_or(""), ',');
    next.above = scale_double(body, "above", 0.8, bad);
    next.window_ms = scale_int(body, "window_ms", 1000, bad);
    next.hold = scale_int(body, "hold", 5, bad);
    const auto enabled = json_value(body, "enabled");
    next.enabled = !enabled || *enabled == "true";
    if (bad || (next.enabled && next.routes.empty()) || !(next.above > 0 && next.above < 1) || next.hold < 1 ||
        next.window_ms < kSampleMs || next.window_ms > kHistorySeconds * 1000) {
      return http_response(400, "Bad Request", ok_json(false), kJson);
    }
    next.changed_at_ns = monotonic_ns();
    {
      std::lock_guard lock(degrade_mu);
      degrade_state = next;
    }
    for (const auto& id : next.daemons) {
      write_policy(id, {});
    }
    static std::once_flag started;
    std::call_once(started, [] { std::thread(degrade_loop).detach(); });
    std::cout << "degrade daemon_id=" << *daemon_id << (next.enabled ? " attached" : " detached") << " above=" << next.above
              << " routes=" << json_value(body, "routes").value_or("") << std::endl;
    note_event(*daemon_id, "policy", next.enabled ? "degrade attached" : "degrade detached");
  }
  std::lock_guard lock(degrade_mu);
  const auto& d = degrade_state;
  std::ostringstream out;
  out << "{\"ok\":true,\"enabled\":" << (d.enabled ? "true" : "false") << ",\"daemon_id\":\"" << d.daemon_id
      << "\",\"above\":" << d.above << ",\"routes\":";
  append_quoted(out, d.routes);
  out << ",\"disabled\":";
  append_quoted(out, d.disabled);
  std::vector<std::string> policies;
  for (const auto& id : d.daemons) {
    policies.push_back(policy_path(id));
  }
  out << ",\"policy\":";
  append_quoted(out, policies);
  out << "}";
  return http_response(200, "OK", out.str(), kJson);
}

// POST /shift {daemon_id, above, window_ms, enabled}. enabled false detaches. Independent of /scale and /throttle.
std::optional<std::string> shift_route(const std::string& request) {
  if (request_target(request) != "/shift") {
    return std::nullopt;
  }
  const auto body = request_body(request);
  const auto daemon_id = json_value(body, "daemon_id");
  if (!daemon_id || !safe_daemon_id(*daemon_id)) {
    return http_response(400, "Bad Request", ok_json(false), kJson);
  }
  bool bad = false;
  const double above = scale_double(body, "above", kShiftAbove, bad);
  const int window_ms = scale_int(body, "window_ms", kShiftWindowMs, bad);
  const auto enabled_text = json_value(body, "enabled");
  if (enabled_text && *enabled_text != "true" && *enabled_text != "false") {
    bad = true;
  }
  const bool enabled = !enabled_text || *enabled_text == "true";
  if (bad || !(above > 0 && above < 1) || window_ms < kSampleMs || window_ms > kHistorySeconds * 1000) {
    return http_response(400, "Bad Request", ok_json(false), kJson);
  }
  if (!resources_ready()) {
    return http_response(500, "Internal Server Error", ok_json(false), kJson);
  }
  {
    std::lock_guard lock(daemons_mu);
    auto* daemon = find_daemon(*daemon_id);
    if (daemon == nullptr || !daemon->running) {
      return http_response(400, "Bad Request", ok_json(false), kJson);
    }
    daemon->scale.shift = enabled;
    daemon->scale.shift_above = above;
    daemon->scale.shift_window_ms = window_ms;
    daemon->scale.shift_at_ns = monotonic_ns();
  }
  note_event(*daemon_id, "policy", enabled ? "shift attached above " + pct(above) : "shift detached");
  std::cout << "shift daemon_id=" << *daemon_id << (enabled ? " attached" : " detached") << " above=" << above
            << " window_ms=" << window_ms << std::endl;
  std::ostringstream json;
  json << "{\"ok\":true,\"enabled\":" << (enabled ? "true" : "false") << ",\"above\":" << above
       << ",\"window_ms\":" << window_ms << "}";
  return http_response(200, "OK", json.str(), kJson);
}

static bool cpus_hit(const std::vector<int>& group_cpus, const std::vector<int>& want) {
  for (const int cpu : want) {
    if (std::binary_search(group_cpus.begin(), group_cpus.end(), cpu)) {
      return true;
    }
  }
  return false;
}

static void append_ints(std::ostringstream& out, const std::vector<int>& values) {
  out << '[';
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) {
      out << ',';
    }
    out << values[i];
  }
  out << ']';
}

static void append_quoted(std::ostringstream& out, const std::vector<std::string>& values) {
  out << '[';
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) {
      out << ',';
    }
    out << '"' << values[i] << '"';
  }
  out << ']';
}

static std::string json_load(double load) {
  if (!(load >= 0)) {
    return "0";
  }
  std::ostringstream out;
  out << std::setprecision(6) << load;
  const auto text = out.str();
  if (text.find("nan") != std::string::npos || text.find("inf") != std::string::npos) {
    return "0";
  }
  return text;
}

struct ScheduledDaemon {
  std::string daemon_id;
  pid_t pid = -1;
  std::vector<int> cpus;
};

static std::vector<ScheduledDaemon> scheduled_daemons() {
  struct Live {
    std::string daemon_id;
    pid_t pid = -1;
    int cpu = -1;
  };
  std::vector<Live> live;
  {
    std::lock_guard lock(daemons_mu);
    for (const auto& daemon : daemons) {
      if (!daemon.running) {
        continue;
      }
      live.push_back(Live{daemon.daemon_id, daemon.pid, daemon.cpu});
    }
  }
  std::vector<ScheduledDaemon> scheduled;
  scheduled.reserve(live.size());
  for (const auto& daemon : live) {
    ScheduledDaemon row;
    row.daemon_id = daemon.daemon_id;
    row.pid = daemon.pid;
    const auto text = trimmed(read_file("/sys/fs/cgroup/mercury/" + daemon.daemon_id + "/cpuset.cpus"));
    if (auto cpus = parse_cpuset(text)) {
      row.cpus = std::move(*cpus);
    } else if (daemon.cpu >= 0) {
      row.cpus.push_back(daemon.cpu);
    }
    scheduled.push_back(std::move(row));
  }
  return scheduled;
}

static std::string map_json(const std::vector<ScheduledDaemon>& daemons_on,
                            const std::vector<ResourceGroupInfo>& groups) {
  std::ostringstream out;
  out << "{\"mercury_cpu\":" << mercury_linux_cpu << ",\"allocatable\":";
  append_ints(out, allocatable_cpus());
  out << ",\"daemons\":[";
  for (std::size_t i = 0; i < daemons_on.size(); ++i) {
    if (i != 0) {
      out << ',';
    }
    const auto& daemon = daemons_on[i];
    out << "{\"daemon_id\":\"" << daemon.daemon_id << "\",\"pid\":" << daemon.pid << ",\"cpus\":";
    append_ints(out, daemon.cpus);
    out << '}';
  }
  out << "],\"groups\":[";
  for (std::size_t i = 0; i < groups.size(); ++i) {
    if (i != 0) {
      out << ',';
    }
    const auto& group = groups[i];
    out << "{\"level\":\"" << resource_level_name(group.level) << "\",\"name\":\"" << group.name << "\",\"parent\":\""
        << group.parent << "\",\"children\":";
    append_quoted(out, group.children);
    out << ",\"linux_cpus\":";
    append_ints(out, group.linux_cpus);
    out << ",\"load\":" << json_load(group.load) << ",\"samples\":" << group.samples << ",\"daemons\":[";
    bool first = true;
    for (const auto& daemon : daemons_on) {
      if (!cpus_hit(group.linux_cpus, daemon.cpus)) {
        continue;
      }
      if (!first) {
        out << ',';
      }
      first = false;
      out << '"' << daemon.daemon_id << '"';
    }
    out << "]}";
  }
  out << "]}";
  return out.str();
}

std::optional<std::string> map_route(const std::string& request) {
  if (request_target(request) != "/map") {
    return std::nullopt;
  }
  if (!resources_ready()) {
    return http_response(500, "Internal Server Error", ok_json(false), kJson);
  }
  return http_response(200, "OK", map_json(scheduled_daemons(), resource_groups()), kJson);
}

static std::string rings_json(const std::vector<ResourceRingView>& views) {
  std::ostringstream out;
  out << "{\"sample_ms\":" << kSampleMs << ",\"capacity\":" << kRingSlots << ",\"groups\":[";
  for (std::size_t i = 0; i < views.size(); ++i) {
    if (i != 0) {
      out << ',';
    }
    const auto& view = views[i];
    out << "{\"level\":\"" << resource_level_name(view.level) << "\",\"name\":\"" << view.name << "\",\"counters\":";
    append_quoted(out, view.counters);
    out << ",\"stored\":" << view.stored << ",\"samples\":[";
    for (std::size_t s = 0; s < view.samples.size(); ++s) {
      if (s != 0) {
        out << ',';
      }
      const auto& sample = view.samples[s];
      out << "{\"timestamp_ns\":" << sample.timestamp_ns;
      for (std::size_t c = 0; c < view.counters.size() && c < static_cast<std::size_t>(kMaxCounters); ++c) {
        const auto value = c < static_cast<std::size_t>(sample.nvalues) ? sample.value[c] : 0;
        out << ",\"" << view.counters[c] << "\":" << value;
      }
      out << '}';
    }
    out << "]}";
  }
  out << "]}";
  return out.str();
}

static std::optional<std::size_t> rings_query_count(const std::string& target, bool& bad) {
  constexpr std::string_view path = "/rings";
  if (target == path) {
    return std::nullopt;
  }
  constexpr std::string_view prefix = "/rings?count=";
  if (target.rfind(prefix, 0) != 0 || target.size() == prefix.size()) {
    bad = true;
    return std::nullopt;
  }
  const auto text = target.substr(prefix.size());
  for (const unsigned char c : text) {
    if (std::isdigit(c) == 0) {
      bad = true;
      return std::nullopt;
    }
  }
  try {
    std::size_t used = 0;
    const auto value = std::stoull(text, &used);
    if (used != text.size() || value > kRingSlots) {
      bad = true;
      return std::nullopt;
    }
    return static_cast<std::size_t>(value);
  } catch (const std::exception&) {
    bad = true;
    return std::nullopt;
  }
}

std::optional<std::string> rings_route(const std::string& request) {
  const auto target = request_target(request);
  if (target != "/rings" && target.rfind("/rings?", 0) != 0) {
    return std::nullopt;
  }
  bool bad = false;
  const auto asked = rings_query_count(target, bad);
  if (bad) {
    return http_response(400, "Bad Request", ok_json(false), kJson);
  }
  if (!resources_ready()) {
    return http_response(500, "Internal Server Error", ok_json(false), kJson);
  }
  constexpr std::size_t kDefaultRingSamples = 100;
  const auto count = asked ? *asked : kDefaultRingSamples;
  return http_response(200, "OK", rings_json(resource_rings(count)), kJson);
}

std::optional<std::string> kill_route(const std::string& request) {
  if (request_target(request) != "/kill") {
    return std::nullopt;
  }
  const auto daemon_id = json_value(request_body(request), "daemon_id");
  if (!daemon_id || daemon_id->empty()) {
    return http_response(400, "Bad Request", ok_json(false), kJson);
  }

  std::unique_lock lock(daemons_mu);
  auto* daemon = find_daemon(*daemon_id);
  if (daemon == nullptr) {
    return http_response(200, "OK", ok_json(false), kJson);
  }
  if (!daemon->finished) {
    const auto pid = daemon->pid;
    ::kill(pid, SIGTERM);
    if (!daemons_cv.wait_for(lock, std::chrono::seconds(2), [&] {
          daemon = find_daemon(*daemon_id);
          return daemon != nullptr && daemon->finished.has_value();
        })) {
      ::kill(pid, SIGKILL);
      daemons_cv.wait(lock, [&] {
        daemon = find_daemon(*daemon_id);
        return daemon != nullptr && daemon->finished.has_value();
      });
    }
  }
  if (daemon != nullptr && !daemon->counters_done) {
    daemons_cv.wait_for(lock, std::chrono::seconds(2), [&] {
      daemon = find_daemon(*daemon_id);
      return daemon != nullptr && daemon->counters_done;
    });
  }
  if (daemon != nullptr && daemon->listen_fd >= 0) {
    ::close(daemon->listen_fd);
    daemon->listen_fd = -1;
  }
  if (daemon == nullptr || !daemon->finished) {
    return http_response(200, "OK", ok_json(false), kJson);
  }
  return http_response(200, "OK", kill_json(*daemon->finished), kJson);
}

// ---- LLM interface (SDK-free): GET /llm/state and POST /llm/act (SPEC.md "LLM interface") ----

static std::string policy_label(const ScaleState& scale) {
  if (!scale.enabled || !scale.policy) {
    return "none";
  }
  return scale.gnn ? "gnn" : "saturate";
}

static std::optional<std::string> query_param(const std::string& target, const std::string& key) {
  const auto query = target.find('?');
  if (query == std::string::npos) {
    return std::nullopt;
  }
  std::size_t at = query + 1;
  while (at < target.size()) {
    const auto end = std::min(target.find('&', at), target.size());
    const auto item = target.substr(at, end - at);
    if (item.rfind(key + "=", 0) == 0) {
      return item.substr(key.size() + 1);
    }
    at = end + 1;
  }
  return std::nullopt;
}

static std::string json_escaped(const std::string& text) {
  std::string out;
  for (const char c : text) {
    if (c == '"' || c == '\\') {
      out.push_back('\\');
      out.push_back(c);
    } else if (static_cast<unsigned char>(c) >= 0x20) {
      out.push_back(c);
    }
  }
  return out;
}

static std::string two_places(double value) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(2) << (value >= 0 ? value : 0);
  return out.str();
}

// What happened after a cursor: the events numbered after seq, and each L3 domain's load from
// the cursor's time to now in up to 12 equal slices (at least one window each). The rings hold
// kHistorySeconds, so an older cursor gets history_truncated and the last kHistorySeconds only.
static std::string since_json(std::uint64_t seq, std::int64_t from_ns, std::int64_t now, int window_ms,
                              const std::vector<L3Block>& blocks) {
  std::ostringstream out;
  const std::int64_t oldest_ns = now - static_cast<std::int64_t>(kHistorySeconds) * 1000000000LL;
  const auto start = std::max(from_ns, oldest_ns);
  out << "{\"seconds\":" << two_places(static_cast<double>(now - from_ns) / 1e9) << ",\"events\":[";
  std::uint64_t missed = 0;
  {
    std::lock_guard lock(events_mu);
    if (!events.empty() && events.front().seq > seq + 1) {
      missed = events.front().seq - seq - 1;
    }
    bool first = true;
    for (const auto& event : events) {
      if (event.seq <= seq) {
        continue;
      }
      out << (first ? "" : ",") << "{\"seq\":" << event.seq << ",\"ago_s\":" << two_places(static_cast<double>(now - event.t_ns) / 1e9)
          << ",\"daemon_id\":\"" << json_escaped(event.daemon_id) << "\",\"kind\":\"" << event.kind << "\",\"detail\":\""
          << json_escaped(event.detail) << "\"}";
      first = false;
    }
  }
  const auto span = std::max<std::int64_t>(now - start, 1);
  const auto window_ns = static_cast<std::int64_t>(window_ms) * 1000000LL;
  const auto slices = std::clamp<std::int64_t>(span / window_ns, 1, 12);
  const auto slice = span / slices;
  out << "],\"missed_events\":" << missed << ",\"history_truncated\":" << (from_ns < oldest_ns ? "true" : "false")
      << ",\"slice_s\":" << two_places(static_cast<double>(slice) / 1e9) << ",\"l3_history\":{";
  for (std::size_t i = 0; i < blocks.size(); ++i) {
    out << (i ? "," : "") << '"' << blocks[i].name << "\":[";
    for (std::int64_t k = 0; k < slices; ++k) {
      const auto load = load_over({blocks[i].name}, blocks[i].cpus.size(), start + k * slice, start + (k + 1) * slice);
      out << (k ? "," : "") << (load ? two_places(*load) : "null");
    }
    out << ']';
  }
  out << "}}";
  return out.str();
}

// GET /llm/state[?window_ms=N][&cursor=C]: the host, every L3 domain, and every running daemon, with
// loads and CPU use over the last window, and a cursor. Passing that cursor back adds "since": the
// placement events and per-domain load history after it. Compact: it is the model's whole view.
std::optional<std::string> llm_state_route(const std::string& request) {
  const auto target = request_target(request);
  if (target != "/llm/state" && target.rfind("/llm/state?", 0) != 0) {
    return std::nullopt;
  }
  int window_ms = kPoolWindowMs;
  if (const auto text = query_param(target, "window_ms")) {
    try {
      window_ms = std::stoi(*text);
    } catch (const std::exception&) {
      window_ms = -1;
    }
  }
  if (window_ms < kSampleMs || window_ms > kHistorySeconds * 1000) {
    return http_response(400, "Bad Request", ok_json(false), kJson);
  }
  // cursor is "<last event seq>.<monotonic ms>" from an earlier reply.
  std::optional<std::pair<std::uint64_t, std::int64_t>> cursor;
  if (const auto text = query_param(target, "cursor"); text && !text->empty()) {
    const auto dot = text->find('.');
    try {
      if (dot == std::string::npos) {
        throw std::invalid_argument("cursor");
      }
      cursor = std::make_pair(std::stoull(text->substr(0, dot)), std::stoll(text->substr(dot + 1)));
    } catch (const std::exception&) {
      return http_response(400, "Bad Request", "{\"ok\":false,\"error\":\"bad cursor\"}", kJson);
    }
  }
  if (!resources_ready()) {
    return http_response(500, "Internal Server Error", ok_json(false), kJson);
  }
  struct Row {
    std::string id;
    std::string exec_path;
    std::string policy;
    bool throttle_active = false;
    bool shift = false;
    std::shared_ptr<Ring> ring;
    std::vector<int> cpus;
  };
  std::vector<Row> rows;
  {
    std::lock_guard lock(daemons_mu);
    for (const auto& daemon : daemons) {
      if (daemon.running) {
        rows.push_back({daemon.daemon_id, daemon.exec_path, policy_label(daemon.scale), daemon.scale.throttle_active,
                        daemon.scale.shift, daemon.ring, {}});
      }
    }
  }
  for (auto& row : rows) {
    row.cpus = cpuset_of(row.id);
  }
  const auto now = monotonic_ns();
  const auto from = now - static_cast<std::int64_t>(window_ms) * 1000000LL;
  const auto host = host_groups();
  const auto blocks = l3_blocks("", {});
  // Per-CPU counters (cpustat, when Mercury runs with MERCURY_CPUSTAT=1), aggregated per daemon and per domain:
  // IPC weighted by busy time, LLC misses summed, run-queue wait averaged.
  std::map<int, CpuWindow> per_cpu;
  for (const auto& window : cpustat_window(from)) {
    per_cpu[window.cpu] = window;
  }
  struct Counters {
    double busy = 0, instr_weight = 0, llc = 0, wait = 0;
    int cpus = 0;
    bool perf = false;
  };
  const auto aggregate = [&](const std::vector<int>& cpus) {
    Counters c;
    for (const int cpu : cpus) {
      const auto at = per_cpu.find(cpu);
      if (at == per_cpu.end()) {
        continue;
      }
      c.busy += at->second.busy;
      c.instr_weight += at->second.ipc * at->second.busy;
      c.llc += at->second.llc_per_ms;
      c.wait += at->second.wait;
      c.perf = c.perf || at->second.perf;
      ++c.cpus;
    }
    return c;
  };
  const auto append_counters = [&](std::ostringstream& to, const Counters& c) {
    if (c.cpus == 0) {
      return;
    }
    to << ",\"cpu_wait\":" << two_places(c.wait / c.cpus) << ",\"llc_per_ms\":" << two_places(c.llc);
    if (c.perf && c.busy > 0) {
      to << ",\"ipc\":" << two_places(c.instr_weight / c.busy);
    }
  };
  PoolState pool_now;
  {
    std::lock_guard lock(pool_mu);
    pool_now = pool;
  }
  std::map<std::string, double> foreign_of;
  std::optional<PoolSignals> signals;
  if (pool_now.enabled) {
    signals = pool_signals(pool_now.active, from);
    for (std::size_t i = 0; i < signals->blocks.size(); ++i) {
      foreign_of[signals->blocks[i].name] = signals->foreign[i];
    }
  }
  std::ostringstream out;
  out << "{\"ok\":true,\"window_ms\":" << window_ms << ",\"host\":{\"cpus\":" << host.cpus
      << ",\"load\":" << json_load(groups_load(host.names, from).value_or(0)) << ",\"mercury_cpu\":" << mercury_linux_cpu
      << ",\"pool\":";
  append_ints(out, daemon_cpus);
  out << "},\"l3\":[";
  for (std::size_t i = 0; i < blocks.size(); ++i) {
    const auto& block = blocks[i];
    std::vector<std::string> on;
    for (const auto& row : rows) {
      if (std::any_of(row.cpus.begin(), row.cpus.end(),
                      [&](int cpu) { return std::find(block.cpus.begin(), block.cpus.end(), cpu) != block.cpus.end(); })) {
        on.push_back(row.id);
      }
    }
    out << (i ? "," : "") << "{\"name\":\"" << block.name << "\",\"node\":\"" << block.node << "\",\"socket\":\""
        << block.socket << "\",\"load\":" << json_load(load_over({block.name}, block.cpus.size(), from).value_or(0))
        << ",\"cpus\":";
    append_ints(out, block.cpus);
    out << ",\"free\":";
    append_ints(out, block.free);
    out << ",\"daemons\":";
    append_quoted(out, on);
    append_counters(out, aggregate(block.cpus));
    if (signals) {
      const bool pooled = std::any_of(block.cpus.begin(), block.cpus.end(), [&](int c) {
        return std::find(pool_now.active.begin(), pool_now.active.end(), c) != pool_now.active.end();
      });
      out << ",\"in_pool\":" << (pooled ? "true" : "false") << ",\"foreign_cpus\":" << two_places(foreign_of[block.name]);
    }
    out << '}';
  }
  out << ']';
  if (signals) {
    out << ",\"pool\":{\"policy\":\"" << (pool_now.grow ? "l3" : "fixed") << "\",\"start\":" << pool_now.start
        << ",\"max\":" << pool_now.max
        << ",\"cores\":" << pool_now.active.size()
        << ",\"cpus\":\"" << format_cpuset(pool_now.active) << "\",\"busy\":" << two_places(signals->busy.value_or(0))
        << ",\"pressure\":" << two_places(signals->pressure) << ",\"allocated\":" << signals->allocated
        << ",\"free_domains\":";
    std::vector<std::string> free_domains;
    for (const auto& b : signals->blocks) {
      if (b.held == 0 && !b.partial && b.free.size() == b.cpus.size()) {
        free_domains.push_back(b.name);
      }
    }
    append_quoted(out, free_domains);
    out << '}';
  }
  out << ",\"daemons\":[";
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const auto& row = rows[i];
    const double use = row.ring ? ring_cpus(*row.ring, from) : 0;
    const auto waiting = row.ring ? ring_waiting(*row.ring, from) : std::nullopt;
    out << (i ? "," : "") << "{\"daemon_id\":\"" << row.id << "\",\"cpus\":";
    append_ints(out, row.cpus);
    out << ",\"l3\":";
    append_quoted(out, l3_names_touching(row.cpus));
    out << ",\"cpu_use\":" << json_load(use) << ",\"occupancy\":"
        << json_load(row.cpus.empty() ? 0 : use / static_cast<double>(row.cpus.size()));
    if (waiting) {
      out << (waiting->psi ? ",\"pressure\":" : ",\"runnable\":") << json_load(waiting->value);
    }
    out << ",\"policy\":\"" << row.policy
        << "\",\"throttled\":" << (row.throttle_active ? "true" : "false") << ",\"shift\":" << (row.shift ? "true" : "false");
    append_counters(out, aggregate(row.cpus));
    out << '}';
  }
  out << ']';
  // The CPUs that matter: held by a daemon, or busy (another process there is a neighbour).
  if (!per_cpu.empty()) {
    out << ",\"cpus\":[";
    bool first = true;
    for (const auto& [cpu, window] : per_cpu) {
      std::vector<std::string> on;
      for (const auto& row : rows) {
        if (std::find(row.cpus.begin(), row.cpus.end(), cpu) != row.cpus.end()) {
          on.push_back(row.id);
        }
      }
      if (on.empty() && window.busy < 0.05) {
        continue;
      }
      out << (first ? "" : ",") << "{\"cpu\":" << cpu << ",\"busy\":" << two_places(window.busy) << ",\"wait\":"
          << two_places(window.wait) << ",\"llc_per_ms\":" << two_places(window.llc_per_ms);
      if (window.perf) {
        out << ",\"ipc\":" << two_places(window.ipc);
      }
      out << ",\"daemons\":";
      append_quoted(out, on);
      out << '}';
      first = false;
    }
    out << ']';
  }
  std::uint64_t last_seq = 0;
  {
    std::lock_guard lock(events_mu);
    last_seq = next_event - 1;
  }
  out << ",\"cursor\":\"" << last_seq << '.' << now / 1000000 << '"';
  if (cursor) {
    out << ",\"since\":" << since_json(cursor->first, cursor->second * 1000000LL, now, window_ms, blocks);
  }
  out << '}';
  return http_response(200, "OK", out.str(), kJson);
}

static std::string act_error(const std::string& message) {
  return "{\"ok\":false,\"error\":\"" + message + "\"}";
}

// POST /llm/act {action, daemon_id, ...}: one placement step chosen by a model.
//   move        {from, to}   the daemon's CPUs in L3 domain from go to as many free CPUs of domain to
//   add_l3      {l3?}        all free CPUs of that domain, or of pick_l3_block's choice when l3 is omitted
//   add_cores   {count}      count CPUs, one add_core each (the daemon's placement rule)
//   remove_cores {count}     count CPUs, one remove_core each, never below one
// The daemon's autoscaling stays attached: it keeps deciding how many CPUs, the model where they are.
// After a step the autoscaler's window restarts, as after its own change. Errors are 400 (bad request) or 409 (not possible now) with a reason.
std::optional<std::string> llm_act_route(const std::string& request) {
  if (request_target(request) != "/llm/act") {
    return std::nullopt;
  }
  const auto body = request_body(request);
  const auto action = json_value(body, "action");
  // Pool steps act on the capacity, not on one daemon.
  if (action && (*action == "grow_pool" || *action == "shrink_pool" || *action == "swap_domain")) {
    std::string detail;
    bool ok = false;
    if (*action == "grow_pool") {
      ok = pool_grow_step(json_value(body, "l3"), "llm", detail);
    } else if (*action == "shrink_pool") {
      ok = pool_shrink_step(json_value(body, "l3"), "llm", detail);
    } else {
      const auto from = json_value(body, "from"), to = json_value(body, "to");
      if (!from || !to) {
        return http_response(400, "Bad Request", act_error("swap_domain needs from and to"), kJson);
      }
      ok = pool_swap_step(*from, *to, "llm", detail);
    }
    if (!ok) {
      return http_response(409, "Conflict", act_error(json_escaped(detail)), kJson);
    }
    note_event("", "llm", *action + ": " + detail);
    std::cout << "llm action=" << *action << " " << detail << std::endl;
    return http_response(200, "OK", "{\"ok\":true,\"action\":\"" + *action + "\",\"detail\":\"" + json_escaped(detail) + "\"}",
                         kJson);
  }
  // degrade {daemon_id: "a" or "a,b", disabled: "r1,r2"}: the llm sheds optional routes itself (llmctl --shed), through
  // the same policy files as the degrade system; disabled is the whole set ("" enables every route again). llmctl
  // decides which routes may be shed, so the critical ones never reach here.
  if (action && *action == "degrade") {
    const auto ids = split_list(json_value(body, "daemon_id").value_or(""), ',');
    const auto disabled = split_list(json_value(body, "disabled").value_or(""), ',');
    if (ids.empty() || std::any_of(ids.begin(), ids.end(), [](const auto& id) { return !safe_daemon_id(id); })) {
      return http_response(400, "Bad Request", act_error("degrade needs daemon_id"), kJson);
    }
    for (const auto& id : ids) {
      if (!write_policy(id, disabled)) {
        return http_response(500, "Internal Server Error", act_error("cannot write " + json_escaped(policy_path(id))), kJson);
      }
    }
    std::string list;
    for (std::size_t i = 0; i < disabled.size(); ++i) {
      list += (i ? "," : "") + disabled[i];
    }
    const auto change = json_value(body, "change").value_or("set");
    // The degrade system's line, so the same tools count the steps: load=llm marks who decided.
    std::cout << "degrade daemon_id=" << json_value(body, "daemon_id").value_or("") << " " << change
              << " load=llm disabled=[" << list << "] t_ns=" << monotonic_ns() << std::endl;
    note_event("", "llm", "degrade: disabled [" + list + "]");
    return http_response(200, "OK", "{\"ok\":true,\"action\":\"degrade\",\"detail\":\"disabled [" + json_escaped(list) + "]\"}",
                         kJson);
  }
  const auto daemon_id = json_value(body, "daemon_id");
  if (!action || !daemon_id || !safe_daemon_id(*daemon_id)) {
    return http_response(400, "Bad Request", act_error("action and daemon_id are required"), kJson);
  }
  bool bad = false;
  const int count = scale_int(body, "count", 1, bad);
  const bool cores_action = *action == "add_cores" || *action == "remove_cores";
  if (*action != "move" && *action != "add_l3" && !cores_action) {
    return http_response(400, "Bad Request", act_error("unknown action " + *action), kJson);
  }
  if (bad || (cores_action && (count < 1 || count > 64))) {
    return http_response(400, "Bad Request", act_error("count must be 1..64"), kJson);
  }
  if (!resources_ready()) {
    return http_response(500, "Internal Server Error", act_error("resource groups are not available"), kJson);
  }
  CpuPlacement placement = CpuPlacement::LeastLoaded;
  {
    std::lock_guard lock(daemons_mu);
    auto* daemon = find_daemon(*daemon_id);
    if (daemon == nullptr || !daemon->running) {
      return http_response(400, "Bad Request", act_error("no running daemon " + *daemon_id), kJson);
    }
    // The daemon's autoscaling stays attached: it decides how many CPUs, the model where they are.
    placement = daemon->scale.placement;
  }
  std::lock_guard place(l3_mu);
  auto have = cpuset_of(*daemon_id);
  if (have.empty()) {
    return http_response(409, "Conflict", act_error("daemon has no cpuset yet"), kJson);
  }
  const auto before = format_cpuset(have);
  int after = 0;
  std::string spec;
  std::string detail;
  if (*action == "move" || *action == "add_l3") {
    auto blocks = l3_blocks(*daemon_id, have);
    const auto named = [&](const char* key) -> std::optional<std::size_t> {
      const auto name = json_value(body, key);
      for (std::size_t i = 0; name && i < blocks.size(); ++i) {
        if (blocks[i].name == *name) {
          return i;
        }
      }
      return std::nullopt;
    };
    if (*action == "move") {
      const auto from = named("from");
      const auto to = named("to");
      if (!from || !to || *from == *to) {
        return http_response(400, "Bad Request", act_error("from and to must be two different L3 domain names"), kJson);
      }
      std::vector<int> leave;
      for (const int cpu : have) {
        if (std::find(blocks[*from].cpus.begin(), blocks[*from].cpus.end(), cpu) != blocks[*from].cpus.end()) {
          leave.push_back(cpu);
        }
      }
      if (leave.empty()) {
        return http_response(409, "Conflict", act_error(*daemon_id + " has no CPUs in " + blocks[*from].name), kJson);
      }
      if (const auto excluded = pool_exclude(*daemon_id); !excluded.empty() && !pool_exempt_daemon(*daemon_id) &&
          std::any_of(blocks[*to].cpus.begin(), blocks[*to].cpus.end(),
                      [&](int c) { return std::find(excluded.begin(), excluded.end(), c) != excluded.end(); })) {
        return http_response(409, "Conflict",
                             act_error(blocks[*to].name + " is outside the pool: grow_pool or swap_domain first"), kJson);
      }
      auto free = blocks[*to].free;
      if (free.size() < leave.size()) {
        return http_response(409, "Conflict",
                             act_error(blocks[*to].name + " has " + std::to_string(free.size()) + " free CPUs, the move needs " +
                                       std::to_string(leave.size())),
                             kJson);
      }
      std::sort(free.begin(), free.end());
      for (const int cpu : leave) {
        have.erase(std::find(have.begin(), have.end(), cpu));
      }
      have.insert(have.end(), free.begin(), free.begin() + static_cast<std::ptrdiff_t>(leave.size()));
      std::sort(have.begin(), have.end());
      detail = "moved " + std::to_string(leave.size()) + " CPUs from " + blocks[*from].name + " to " + blocks[*to].name;
    } else {
      std::vector<L3Block> choice = blocks;
      if (json_value(body, "l3")) {
        const auto index = named("l3");
        if (!index) {
          return http_response(400, "Bad Request", act_error("unknown L3 domain"), kJson);
        }
        const auto& block = blocks[*index];
        if (block.held == 0 && block.free.size() != block.cpus.size()) {
          return http_response(409, "Conflict", act_error(block.name + " is used by another daemon"), kJson);
        }
        choice = {block};
      }
      const auto size = have.size();
      if (!l3_take_block(choice, have, 64)) {
        return http_response(409, "Conflict", act_error("no free L3 domain"), kJson);
      }
      detail = "added " + std::to_string(have.size() - size) + " CPUs";
    }
    if (!set_cpuset(*daemon_id, have, after, spec)) {
      return http_response(500, "Internal Server Error", act_error("cpuset write failed"), kJson);
    }
  } else {
    after = static_cast<int>(have.size());
    spec = before;
    int done = 0;
    for (; done < count; ++done) {
      const bool ok = *action == "add_cores" ? add_core(*daemon_id, std::numeric_limits<int>::max(), after, spec, placement)
                                             : remove_core(*daemon_id, 1, after, spec);
      if (!ok) {
        break;
      }
    }
    if (done == 0) {
      return http_response(409, "Conflict",
                           act_error(*action == "add_cores" ? "no allocatable CPU left to add" : "daemon is at one CPU"), kJson);
    }
    detail = (*action == "add_cores" ? "added " : "removed ") + std::to_string(done) + " of " + std::to_string(count) + " CPUs";
  }
  {
    std::lock_guard lock(daemons_mu);
    if (auto* daemon = find_daemon(*daemon_id)) {
      daemon->scale.changed_at_ns = monotonic_ns();
    }
  }
  note_event(*daemon_id, "llm", *action + ": " + detail + ", cpus " + before + " -> " + spec);
  std::cout << "llm daemon_id=" << *daemon_id << " action=" << *action << " " << detail << " cpus " << before << " -> " << spec
            << std::endl;
  std::ostringstream json;
  json << "{\"ok\":true,\"action\":\"" << *action << "\",\"daemon_id\":\"" << *daemon_id << "\",\"detail\":\"" << detail
       << "\",\"before\":\"" << before << "\",\"cpus\":\"" << spec << "\",\"cores\":" << after << "}";
  return http_response(200, "OK", json.str(), kJson);
}
