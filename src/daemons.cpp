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
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
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
  std::vector<std::pair<std::string, std::string>> env;
};

std::mutex daemons_mu;
std::condition_variable daemons_cv;
std::vector<Daemon> daemons;

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

bool ensure_cpuset_controller(const std::string& subtree_control) {
  if (controller_enabled(read_file(subtree_control), "cpuset")) {
    return true;
  }
  if (write_sysfs(subtree_control, "+cpuset")) {
    return true;
  }
  return controller_enabled(read_file(subtree_control), "cpuset");
}

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
  // Another daemon may already run on the CPU placement returns.
  auto next = request_cpu_for(placement, *have, {});
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
  std::cout << "counters daemon_id=" << daemon_id << " cgroup " << daemon_names.back() << std::endl;
  auto previous_cpu = read_cgroup_cpu(daemon_id);
  auto previous_pressure = use_psi ? read_cgroup_pressure(daemon_id) : std::nullopt;
  std::unique_ptr<GnnController> gnn;  // only while the gnn policy is attached; its destructor restores affinity
  long gnn_ticks = 0, gnn_logged = 0, gnn_warned = 0;
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
      if (!scale.enabled || !scale.policy) {
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
  const auto env = parse_env(body);
  if (!env.ok) {
    return http_response(400, "Bad Request", R"({"pid":0})", kJson);
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
  const auto want = parse_cpuset(trimmed(*cpus_text));
  if (!want || (mercury_linux_cpu >= 0 &&
                std::find(want->begin(), want->end(), mercury_linux_cpu) != want->end())) {
    return http_response(400, "Bad Request", ok_json(false), kJson);
  }
  if (!resource_groups_see(*want)) {
    std::cerr << "cpus daemon_id=" << *daemon_id << " cpus are not in a resource group" << std::endl;
    return http_response(400, "Bad Request", ok_json(false), kJson);
  }

  const auto dir = ensure_daemon_cpuset(*daemon_id);
  if (!dir) {
    std::cerr << "cpus daemon_id=" << *daemon_id << " cgroup setup failed" << std::endl;
    return http_response(500, "Internal Server Error", ok_json(false), kJson);
  }
  const auto path = *dir + "/cpuset.cpus";
  if (!write_cpus_and_mems(*dir, trimmed(*cpus_text))) {
    std::cerr << "cpus daemon_id=" << *daemon_id << " write " << path << " failed" << std::endl;
    return http_response(500, "Internal Server Error", ok_json(false), kJson);
  }
  for (int attempt = 0; attempt < 50; ++attempt) {
    if (cpuset_applied(*dir, *want)) {
      std::cout << "cpus daemon_id=" << *daemon_id << " cpus=" << trimmed(*cpus_text) << " socket "
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
  }
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
