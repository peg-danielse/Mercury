#include "daemons.hpp"

#include "counters.hpp"
#include "http.hpp"
#include "json.hpp"
#include "policy.hpp"
#include "resources.hpp"
#include "ring.hpp"
#include "ring_archive.hpp"

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
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
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

struct ThreadRing {
  pid_t tid = -1;
  std::shared_ptr<Ring> ring;
};

struct ScaleState {
  bool enabled = false;
  int min_cores = 1;
  int max_cores = kDefaultMaxCores;
  std::shared_ptr<Policy> policy;
  std::int64_t added_at_ns = 0;
  // Allocatable CPUs outside the policy ceiling (policies.md). add_core never returns these.
  std::vector<int> outside;
};

struct Daemon {
  std::string daemon_id;
  std::string exec_path;
  pid_t pid = -1;
  bool running = false;
  int exit_code = -1;
  std::string log;
  std::optional<std::int64_t> finished;
  std::vector<ThreadRing> threads;
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

std::vector<pid_t> child_pids(pid_t parent) {
  std::vector<pid_t> kids;
  const auto path = "/proc/" + std::to_string(parent) + "/task/" + std::to_string(parent) + "/children";
  std::ifstream in(path);
  pid_t pid = 0;
  while (in >> pid) {
    kids.push_back(pid);
  }
  return kids;
}

std::vector<pid_t> descendants(pid_t root) {
  std::vector<pid_t> pending{root};
  std::vector<pid_t> out;
  for (std::size_t i = 0; i < pending.size(); ++i) {
    for (const auto pid : child_pids(pending[i])) {
      pending.push_back(pid);
      out.push_back(pid);
    }
  }
  return out;
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
    if (!write_sysfs(*dir + "/cpuset.cpus", std::to_string(linux_cpu))) {
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

std::optional<Launched> launch(const std::string& daemon_id, const std::string& exec_path, int cpu, int listen_fd,
                               const std::vector<std::pair<std::string, std::string>>& env) {
  const int linux_cpu = cpu;
  const auto placement = prepare_placement(daemon_id, linux_cpu);
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

std::uint64_t cpu_ticks(pid_t tid) {
  std::ifstream in("/proc/" + std::to_string(tid) + "/stat");
  std::string line;
  if (!in || !std::getline(in, line)) {
    return 0;
  }
  const auto paren = line.rfind(')');
  if (paren == std::string::npos) {
    return 0;
  }
  std::istringstream rest(line.substr(paren + 1));
  std::string state;
  unsigned long skip = 0;
  unsigned long utime = 0;
  unsigned long stime = 0;
  rest >> state;
  for (int i = 0; i < 10; ++i) {
    rest >> skip;
  }
  rest >> utime >> stime;
  if (!rest) {
    return 0;
  }
  return static_cast<std::uint64_t>(utime) + static_cast<std::uint64_t>(stime);
}

std::vector<pid_t> busiest_threads(const std::vector<pid_t>& want, const std::vector<pid_t>& already,
                                   std::unordered_map<pid_t, std::uint64_t>& seen, std::size_t cap) {
  struct Rank {
    pid_t tid = -1;
    std::uint64_t delta = 0;
  };
  std::vector<Rank> ranked;
  ranked.reserve(want.size());
  for (const auto tid : want) {
    const auto now = cpu_ticks(tid);
    const auto previous = seen.find(tid);
    const auto delta = previous == seen.end() ? 0 : now - previous->second;
    seen[tid] = now;
    ranked.push_back(Rank{tid, delta});
  }
  for (auto it = seen.begin(); it != seen.end();) {
    if (std::find(want.begin(), want.end(), it->first) == want.end()) {
      it = seen.erase(it);
    } else {
      ++it;
    }
  }
  const auto by_delta = [](const Rank& a, const Rank& b) {
    if (a.delta != b.delta) {
      return a.delta > b.delta;
    }
    return a.tid < b.tid;
  };
  std::sort(ranked.begin(), ranked.end(), by_delta);
  std::vector<Rank> members;
  std::vector<Rank> outsiders;
  members.reserve(std::min(already.size(), cap));
  for (const auto& rank : ranked) {
    if (std::find(already.begin(), already.end(), rank.tid) != already.end()) {
      members.push_back(rank);
    } else {
      outsiders.push_back(rank);
    }
  }
  if (members.size() > cap) {
    members.resize(cap);
  }
  for (const auto& outsider : outsiders) {
    if (members.size() < cap) {
      members.push_back(outsider);
      continue;
    }
    // Equal CPU time keeps the thread already open, so the boundary does not flap.
    if (outsider.delta <= members.back().delta) {
      break;
    }
    members.back() = outsider;
    std::sort(members.begin(), members.end(), by_delta);
  }
  std::vector<pid_t> keep;
  keep.reserve(members.size());
  for (const auto& member : members) {
    keep.push_back(member.tid);
  }
  std::sort(keep.begin(), keep.end());
  return keep;
}

std::vector<pid_t> task_ids(pid_t pid) {
  std::vector<pid_t> ids;
  const auto path = "/proc/" + std::to_string(pid) + "/task";
  DIR* dir = ::opendir(path.c_str());
  if (dir == nullptr) {
    return ids;
  }
  while (const auto* entry = ::readdir(dir)) {
    if (entry->d_name[0] >= '1' && entry->d_name[0] <= '9') {
      ids.push_back(static_cast<pid_t>(std::atoi(entry->d_name)));
    }
  }
  ::closedir(dir);
  return ids;
}

std::vector<pid_t> daemon_tasks(pid_t root) {
  std::vector<pid_t> procs{root};
  const auto kids = descendants(root);
  procs.insert(procs.end(), kids.begin(), kids.end());
  std::vector<pid_t> tids;
  for (const auto pid : procs) {
    const auto tasks = task_ids(pid);
    tids.insert(tids.end(), tasks.begin(), tasks.end());
  }
  std::sort(tids.begin(), tids.end());
  tids.erase(std::unique(tids.begin(), tids.end()), tids.end());
  return tids;
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

int cpuset_cores(const std::string& daemon_id) {
  const auto have = parse_cpuset(trimmed(read_file("/sys/fs/cgroup/mercury/" + daemon_id + "/cpuset.cpus")));
  if (!have) {
    return 0;
  }
  return static_cast<int>(have->size());
}

bool add_core(const std::string& daemon_id, int max_cores, const std::vector<int>& outside, int& cores_after,
              std::string& spec_after) {
  const auto dir = ensure_daemon_cpuset(daemon_id);
  if (!dir) {
    return false;
  }
  auto have = parse_cpuset(trimmed(read_file(*dir + "/cpuset.cpus")));
  if (!have || have->empty() || static_cast<int>(have->size()) >= max_cores) {
    return false;
  }
  // Least-loaded core. Another daemon may already run on the CPU it returns.
  std::vector<int> exclude = *have;
  exclude.insert(exclude.end(), outside.begin(), outside.end());
  auto next = request_cpu(exclude);
  if (!next) {
    return false;
  }
  have->push_back(*next);
  std::sort(have->begin(), have->end());
  const auto spec = format_cpuset(*have);
  if (!write_sysfs(*dir + "/cpuset.cpus", spec)) {
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
  if (!write_sysfs(*dir + "/cpuset.cpus", spec)) {
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

struct LiveThread {
  pid_t tid = -1;
  CounterGroup counters;
  std::shared_ptr<Ring> ring;
};

void remember_thread(const std::string& daemon_id, pid_t root, pid_t tid, const std::shared_ptr<Ring>& ring) {
  std::lock_guard lock(daemons_mu);
  if (auto* daemon = find_daemon(daemon_id); daemon != nullptr && daemon->pid == root) {
    daemon->threads.push_back(ThreadRing{tid, ring});
  }
}

void forget_thread(const std::string& daemon_id, pid_t root, pid_t tid) {
  std::lock_guard lock(daemons_mu);
  if (auto* daemon = find_daemon(daemon_id); daemon != nullptr && daemon->pid == root) {
    std::erase_if(daemon->threads, [&](const ThreadRing& thread) { return thread.tid == tid; });
  }
}

// A full group is several perf fds plus an archive file. Further threads keep
// task-clock only. Past the cap we open nothing: one fd per thread is what
// exhausts the process file limit.
constexpr std::size_t kFullCounterThreads = 16;
constexpr std::size_t kMaxMonitoredThreads = 64;

// cpu.stat is the kernel total for every task in the daemon cgroup, including
// threads that never get their own ring.
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

std::uint64_t usec_delta_ns(std::uint64_t after, std::uint64_t before) {
  if (after < before) {
    return 0;
  }
  return (after - before) * 1000ULL;
}

void monitor_daemon(const std::string& daemon_id, pid_t root) {
  std::vector<LiveThread> live;
  std::unordered_map<pid_t, std::uint64_t> cpu_seen;
  bool reported_cap = false;
  bool reported_open = false;
  const std::vector<std::string> daemon_names{"task-clock", "user", "system"};
  std::shared_ptr<Ring> daemon_ring;
  try {
    auto archive = std::make_shared<RingArchive>(rings_dir() + "/" + archive_name(daemon_id) + ".mring", daemon_names);
    daemon_ring = std::make_shared<Ring>(kRingSlots, daemon_names, archive);
    std::cout << "counters daemon_id=" << daemon_id << " cgroup" << std::endl;
  } catch (const std::exception& error) {
    std::cerr << "ring archive daemon_id=" << daemon_id << " " << error.what() << std::endl;
  }
  auto previous_cpu = read_cgroup_cpu(daemon_id);
  try {
    while (daemon_running(daemon_id, root)) {
      const auto want = daemon_tasks(root);
      std::vector<pid_t> keep = want;
      if (want.size() > kMaxMonitoredThreads) {
        if (!reported_cap) {
          std::cerr << "counters daemon_id=" << daemon_id << " thread cap " << kMaxMonitoredThreads << std::endl;
          reported_cap = true;
        }
        std::vector<pid_t> already;
        already.reserve(live.size());
        for (const auto& thread : live) {
          already.push_back(thread.tid);
        }
        keep = busiest_threads(want, already, cpu_seen, kMaxMonitoredThreads);
      } else {
        cpu_seen.clear();
      }
      for (auto it = live.begin(); it != live.end();) {
        if (std::find(keep.begin(), keep.end(), it->tid) == keep.end()) {
          it->ring->flush();
          forget_thread(daemon_id, root, it->tid);
          it = live.erase(it);
        } else {
          ++it;
        }
      }
      bool unmonitored = false;
      for (const auto tid : keep) {
        if (std::any_of(live.begin(), live.end(), [&](const LiveThread& thread) { return thread.tid == tid; })) {
          continue;
        }
        try {
          const bool full = live.size() < kFullCounterThreads;
          auto counters = full ? open_counters(tid) : open_clock(tid);
          counters.arm();
          std::shared_ptr<RingArchive> archive;
          if (full) {
            try {
              archive = std::make_shared<RingArchive>(
                  rings_dir() + "/" + archive_name(daemon_id) + "." + std::to_string(tid) + ".mring", counters.names());
            } catch (const std::exception& error) {
              std::cerr << "ring archive daemon_id=" << daemon_id << " tid=" << tid << " " << error.what() << std::endl;
            }
          }
          auto ring = std::make_shared<Ring>(kRingSlots, counters.names(), archive);
          remember_thread(daemon_id, root, tid, ring);
          std::cout << "counters daemon_id=" << daemon_id << " tid=" << tid << std::endl;
          live.push_back(LiveThread{tid, std::move(counters), std::move(ring)});
        } catch (const std::exception& error) {
          // One failure is enough. Retrying every thread in this tick just spends
          // the remaining descriptors and repeats the same error.
          if (!reported_open) {
            std::cerr << "counters daemon_id=" << daemon_id << " tid=" << tid << " " << error.what() << std::endl;
            reported_open = true;
          }
          unmonitored = true;
          break;
        }
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(kSampleMs));
      if (!daemon_running(daemon_id, root)) {
        break;
      }
      for (auto it = live.begin(); it != live.end();) {
        Sample sample;
        if (!it->counters.pull(sample)) {
          it->ring->flush();
          forget_thread(daemon_id, root, it->tid);
          it = live.erase(it);
          continue;
        }
        it->ring->push(sample);
        ++it;
      }
      if (daemon_ring) {
        const auto current_cpu = read_cgroup_cpu(daemon_id);
        if (previous_cpu && current_cpu && current_cpu->usage_usec >= previous_cpu->usage_usec) {
          Sample total;
          total.timestamp_ns = monotonic_ns();
          total.nvalues = 3;
          total.value[0] = usec_delta_ns(current_cpu->usage_usec, previous_cpu->usage_usec);
          total.value[1] = usec_delta_ns(current_cpu->user_usec, previous_cpu->user_usec);
          total.value[2] = usec_delta_ns(current_cpu->system_usec, previous_cpu->system_usec);
          daemon_ring->push(total);
        }
        if (current_cpu) {
          previous_cpu = current_cpu;
        }
      }

      ScaleState scale;
      std::vector<std::shared_ptr<Ring>> rings;
      {
        std::lock_guard lock(daemons_mu);
        const auto* daemon = find_daemon(daemon_id);
        if (daemon == nullptr || daemon->pid != root) {
          break;
        }
        scale = daemon->scale;
        rings.reserve(daemon->threads.size());
        for (const auto& thread : daemon->threads) {
          rings.push_back(thread.ring);
        }
      }
      if (!scale.enabled || !scale.policy) {
        continue;
      }
      OccupancyWindow window;
      window.cores = cpuset_cores(daemon_id);
      window.min_cores = scale.min_cores;
      window.max_cores = scale.max_cores;
      window.threads.reserve(rings.size());
      for (const auto& ring : rings) {
        if (!ring) {
          continue;
        }
        ThreadSamples thread;
        thread.names = ring->names();
        for (const auto& sample : ring->recent(static_cast<std::size_t>(kScaleWindow) + 1)) {
          if (sample.timestamp_ns > scale.added_at_ns) {
            thread.samples.push_back(sample);
          }
        }
        window.threads.push_back(std::move(thread));
      }
      const int before = window.cores;
      const auto decision = scale.policy->decide(window);
      // A thread we could not open is invisible. Removing a core from that
      // partial view would drop capacity the daemon is still using.
      if (decision == Decision::Hold || (unmonitored && decision == Decision::RemoveCore)) {
        continue;
      }
      int after = 0;
      std::string spec;
      const bool changed = decision == Decision::AddCore
                               ? add_core(daemon_id, scale.max_cores, scale.outside, after, spec)
                               : remove_core(daemon_id, scale.min_cores, after, spec);
      if (!changed) {
        continue;
      }
      std::cout << "scale daemon_id=" << daemon_id << " cores=" << after << " cpus=" << spec << std::endl;
      // Scale-up must wait for a fresh window or the same saturated samples add every core.
      // Scale-down keeps the window and sheds one core per sample until busy threads fit.
      if (decision == Decision::AddCore && before >= scale.min_cores) {
        std::lock_guard lock(daemons_mu);
        if (auto* daemon = find_daemon(daemon_id); daemon != nullptr && daemon->pid == root) {
          daemon->scale.added_at_ns = monotonic_ns();
        }
      }
    }
  } catch (const std::exception& error) {
    std::cerr << "counters daemon_id=" << daemon_id << " " << error.what() << std::endl;
  }
  for (auto& thread : live) {
    if (thread.ring) {
      thread.ring->flush();
    }
    forget_thread(daemon_id, root, thread.tid);
  }
  if (daemon_ring) {
    daemon_ring->flush();
  }
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
  if (!write_sysfs(path, std::to_string(cpu))) {
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
  if (!write_sysfs(path, trimmed(*cpus_text))) {
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
  const auto policy_name = json_value(body, "policy");
  const std::string name = policy_name ? *policy_name : "saturate";
  if (bad || min_cores < 1 || max_cores < min_cores) {
    return http_response(400, "Bad Request", ok_json(false), kJson);
  }
  const auto policy = make_policy(name);
  if (!policy) {
    return http_response(400, "Bad Request", ok_json(false), kJson);
  }
  if (!resources_ready() || !least_loaded(ResourceLevel::Core)) {
    std::cerr << "scale daemon_id=" << *daemon_id << " resource groups are not available" << std::endl;
    return http_response(500, "Internal Server Error", ok_json(false), kJson);
  }

  int threads = 0;
  {
    std::lock_guard lock(daemons_mu);
    auto* daemon = find_daemon(*daemon_id);
    if (daemon == nullptr || !daemon->running) {
      return http_response(400, "Bad Request", ok_json(false), kJson);
    }
    const bool first = !daemon->scale.enabled || daemon->scale.policy == nullptr;
    daemon->scale.enabled = true;
    daemon->scale.min_cores = min_cores;
    daemon->scale.max_cores = max_cores;
    daemon->scale.policy = policy;
    if (first) {
      daemon->scale.added_at_ns = monotonic_ns();
    }
    threads = static_cast<int>(daemon->threads.size());
  }
  std::cout << "scale daemon_id=" << *daemon_id << " least socket "
            << resource_choice(least_loaded(ResourceLevel::Socket)) << " node "
            << resource_choice(least_loaded(ResourceLevel::Node)) << " l3 "
            << resource_choice(least_loaded(ResourceLevel::L3)) << " core "
            << resource_choice(least_loaded(ResourceLevel::Core)) << std::endl;
  return http_response(200, "OK", scale_json(min_cores, max_cores, cpuset_cores(*daemon_id), threads), kJson);
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

// ---------------------------------------------------------------------------
// Resource and application policies (policies.md): POST/GET /policies and policy_loop.
// The decisions are the pure functions in src/policy.cpp. This section only gathers
// ring data, applies cpuset writes, and writes rings/<daemon_id>.policy.
// ---------------------------------------------------------------------------
namespace {

constexpr int kEventCap = 2000;

struct TuneValue {
  std::string text;
  bool number = false;
};

struct PolicyDoc {
  pid_t pid = -1;
  std::uint64_t generation = 0;
  bool shift = false;
  ShiftRule rule;
  std::string level = "core";
  std::string ceiling;
  std::vector<int> ceiling_cpus;  // sorted; empty means no ceiling
  bool degrade = false;
  double degrade_limit = kDegradeLimit;
  std::vector<DegradeComponent> components;
  bool tune = false;
  double tune_limit = kDegradeLimit;
  std::vector<TuneParameter> parameters;
  std::vector<std::vector<TuneValue>> raw_values;
  std::int64_t attached_tick = 0;
  std::int64_t shifted_tick = -1000000;
  std::int64_t degrade_tick = -1000000;
  std::int64_t tune_tick = -1000000;
  std::string path;
};

std::mutex policies_mu;
std::map<std::string, PolicyDoc> policy_docs;
std::vector<std::string> policy_events;
std::map<std::string, IntervalLoads> latest_group_loads;
std::uint64_t policy_generation = 0;
std::atomic<std::int64_t> policy_tick{0};

const char* trigger_name(ShiftTrigger trigger) {
  switch (trigger) {
    case ShiftTrigger::Hotspot:
      return "hotspot";
    case ShiftTrigger::Mean:
      return "mean";
    case ShiftTrigger::Either:
      return "either";
  }
  return "hotspot";
}

std::string fmt_double(double value) {
  std::ostringstream out;
  out << std::setprecision(6) << value;
  const auto text = out.str();
  if (text.find("nan") != std::string::npos || text.find("inf") != std::string::npos) {
    return "0";
  }
  return text;
}

void record_event(std::string json_fields) {
  std::ostringstream out;
  out << "{\"tick\":" << policy_tick.load() << ",\"t_ms\":" << now_ms() << "," << json_fields << "}";
  std::cout << "policy " << out.str() << std::endl;
  std::lock_guard lock(policies_mu);
  policy_events.push_back(out.str());
  if (policy_events.size() > static_cast<std::size_t>(kEventCap)) {
    policy_events.erase(policy_events.begin(), policy_events.begin() + (kEventCap / 4));
  }
}

bool valid_policy_name(const std::string& name) { return valid_env_name(name); }

std::string policy_file_json(const PolicyDoc& doc) {
  std::ostringstream out;
  out << "{\"components\":[";
  for (std::size_t i = 0; i < doc.components.size(); ++i) {
    if (i != 0) {
      out << ',';
    }
    out << "{\"name\":\"" << doc.components[i].name << "\",\"enabled\":" << (doc.components[i].enabled ? "true" : "false")
        << '}';
  }
  out << "],\"parameters\":{";
  for (std::size_t i = 0; i < doc.parameters.size(); ++i) {
    if (i != 0) {
      out << ',';
    }
    const auto& value = doc.raw_values[i][doc.parameters[i].index];
    out << '"' << doc.parameters[i].name << "\":";
    if (value.number) {
      out << value.text;
    } else {
      out << '"' << json_escape(value.text) << '"';
    }
  }
  out << "}}";
  return out.str();
}

bool write_policy_file(const PolicyDoc& doc) {
  if (doc.path.empty()) {
    return false;
  }
  const auto tmp = doc.path + ".tmp";
  {
    std::ofstream file(tmp, std::ios::trunc);
    if (!file) {
      return false;
    }
    file << policy_file_json(doc) << "\n";
    if (!file) {
      return false;
    }
  }
  return std::rename(tmp.c_str(), doc.path.c_str()) == 0;
}

std::vector<int> cpus_inside(const std::vector<int>& cpus, const std::vector<int>& sorted_group) {
  std::vector<int> out;
  for (const int cpu : cpus) {
    if (std::binary_search(sorted_group.begin(), sorted_group.end(), cpu)) {
      out.push_back(cpu);
    }
  }
  return out;
}

std::vector<int> cpus_outside(const std::vector<int>& cpus, const std::vector<int>& sorted_group) {
  std::vector<int> out;
  for (const int cpu : cpus) {
    if (!std::binary_search(sorted_group.begin(), sorted_group.end(), cpu)) {
      out.push_back(cpu);
    }
  }
  return out;
}

bool fraction(const JsonValue* value, double& out, bool allow_one_plus = false) {
  if (value == nullptr) {
    return true;
  }
  if (!value->is_number() || !(value->number > 0) || (!allow_one_plus && value->number > 1.0)) {
    return false;
  }
  out = value->number;
  return true;
}

// Parse a policies.md document into doc. Returns an error string, empty on success.
std::string parse_policy_doc(const JsonValue& body, PolicyDoc& doc, bool& empty) {
  static const std::set<std::string> known = {"daemon_id", "exec_path", "shift", "restrict", "widen", "degrade", "tune"};
  empty = true;
  for (const auto& [key, _] : body.fields) {
    if (known.count(key) == 0) {
      return "unknown field " + key;
    }
    if (key != "daemon_id" && key != "exec_path") {
      empty = false;
    }
  }
  if (body.get("widen") != nullptr) {
    // Planned: grant widening by least_allocated (policies.md "Adding a group"). The decision
    // least_allocated is in src/policy.cpp; the grant bookkeeping is not built on this branch.
    return "widen is not implemented on this branch";
  }
  if (const auto* shift = body.get("shift")) {
    if (shift->is_bool()) {
      doc.shift = shift->boolean;
    } else if (shift->is_object()) {
      doc.shift = true;
      for (const auto& [key, value] : shift->fields) {
        if (key == "trigger") {
          if (!value.is_string()) {
            return "shift.trigger must be a string";
          }
          if (value.text == "hotspot") {
            doc.rule.trigger = ShiftTrigger::Hotspot;
          } else if (value.text == "mean") {
            doc.rule.trigger = ShiftTrigger::Mean;
          } else if (value.text == "either") {
            doc.rule.trigger = ShiftTrigger::Either;
          } else {
            return "shift.trigger must be hotspot, mean, or either";
          }
        } else if (key == "util") {
          if (!fraction(&value, doc.rule.hot_util)) {
            return "shift.util must be in (0, 1]";
          }
        } else if (key == "theta") {
          if (!fraction(&value, doc.rule.mean_theta)) {
            return "shift.theta must be in (0, 1]";
          }
        } else {
          return "unknown field shift." + key;
        }
      }
    } else {
      return "shift must be true, false, or an object";
    }
  }
  if (const auto* restrict = body.get("restrict")) {
    if (!restrict->is_object()) {
      return "restrict must be an object";
    }
    for (const auto& [key, value] : restrict->fields) {
      if (key == "level") {
        if (!value.is_string() ||
            (value.text != "core" && value.text != "l3" && value.text != "node" && value.text != "socket")) {
          return "restrict.level must be core, l3, node, or socket";
        }
        doc.level = value.text;
      } else if (key == "ceiling") {
        if (!value.is_string()) {
          return "restrict.ceiling must be a group name";
        }
        doc.ceiling = value.text;
      } else {
        return "unknown field restrict." + key;
      }
    }
  }
  if (const auto* degrade = body.get("degrade")) {
    if (!degrade->is_object()) {
      return "degrade must be an object";
    }
    doc.degrade = true;
    for (const auto& [key, value] : degrade->fields) {
      if (key == "limit") {
        if (!fraction(&value, doc.degrade_limit)) {
          return "degrade.limit must be in (0, 1]";
        }
      } else if (key == "components") {
        if (!value.is_array()) {
          return "degrade.components must be a list";
        }
        for (const auto& item : value.items) {
          const auto* name = item.get("name");
          const auto* cost = item.get("cost");
          if (!item.is_object() || item.fields.size() != 2 || name == nullptr || !name->is_string() ||
              !valid_policy_name(name->text) || cost == nullptr) {
            return "degrade.components entries are {name, cost}";
          }
          DegradeComponent component;
          component.name = name->text;
          if (!fraction(cost, component.cost)) {
            return "degrade cost must be in (0, 1]";
          }
          for (const auto& other : doc.components) {
            if (other.name == component.name) {
              return "duplicate component " + component.name;
            }
          }
          doc.components.push_back(component);
        }
      } else {
        return "unknown field degrade." + key;
      }
    }
  }
  if (const auto* tune = body.get("tune")) {
    if (!tune->is_object()) {
      return "tune must be an object";
    }
    doc.tune = true;
    for (const auto& [key, value] : tune->fields) {
      if (key == "limit") {
        if (!fraction(&value, doc.tune_limit)) {
          return "tune.limit must be in (0, 1]";
        }
      } else if (key == "parameters") {
        if (!value.is_array()) {
          return "tune.parameters must be a list";
        }
        for (const auto& item : value.items) {
          const auto* name = item.get("name");
          const auto* values = item.get("values");
          if (!item.is_object() || item.fields.size() != 2 || name == nullptr || !name->is_string() ||
              !valid_policy_name(name->text) || values == nullptr || !values->is_array() ||
              values->items.size() < 2) {
            return "tune.parameters entries are {name, values} with at least two values";
          }
          TuneParameter parameter;
          parameter.name = name->text;
          std::vector<TuneValue> raw;
          for (const auto& entry : values->items) {
            if (entry.is_number()) {
              std::ostringstream text;
              text << std::setprecision(15) << entry.number;
              raw.push_back(TuneValue{text.str(), true});
            } else if (entry.is_string()) {
              raw.push_back(TuneValue{entry.text, false});
            } else {
              return "tune values are numbers or strings";
            }
            parameter.values.push_back(raw.back().text);
          }
          parameter.index = parameter.values.size() - 1;
          for (const auto& other : doc.parameters) {
            if (other.name == parameter.name) {
              return "duplicate parameter " + parameter.name;
            }
          }
          doc.parameters.push_back(std::move(parameter));
          doc.raw_values.push_back(std::move(raw));
        }
      } else {
        return "unknown field tune." + key;
      }
    }
  }
  return {};
}

std::string doc_state_json(const std::string& daemon_id, const PolicyDoc& doc) {
  std::ostringstream out;
  out << "{\"daemon_id\":\"" << json_escape(daemon_id) << "\",\"pid\":" << doc.pid
      << ",\"shift\":" << (doc.shift ? "true" : "false") << ",\"trigger\":\"" << trigger_name(doc.rule.trigger)
      << "\",\"util\":" << fmt_double(doc.rule.hot_util) << ",\"theta\":" << fmt_double(doc.rule.mean_theta)
      << ",\"level\":\"" << doc.level << "\",\"ceiling\":\"" << json_escape(doc.ceiling) << "\",\"degrade_limit\":"
      << fmt_double(doc.degrade_limit) << ",\"components\":[";
  for (std::size_t i = 0; i < doc.components.size(); ++i) {
    if (i != 0) {
      out << ',';
    }
    out << "{\"name\":\"" << doc.components[i].name << "\",\"cost\":" << fmt_double(doc.components[i].cost)
        << ",\"enabled\":" << (doc.components[i].enabled ? "true" : "false") << '}';
  }
  out << "],\"parameters\":[";
  for (std::size_t i = 0; i < doc.parameters.size(); ++i) {
    if (i != 0) {
      out << ',';
    }
    out << "{\"name\":\"" << doc.parameters[i].name << "\",\"index\":" << doc.parameters[i].index << ",\"value\":\""
        << json_escape(doc.parameters[i].values[doc.parameters[i].index]) << "\"}";
  }
  out << "],\"path\":\"" << json_escape(doc.path) << "\"}";
  return out.str();
}

std::string policies_state_json() {
  std::ostringstream out;
  std::lock_guard lock(policies_mu);
  out << "{\"tick\":" << policy_tick.load() << ",\"interval_ms\":1000,\"daemons\":[";
  bool first = true;
  for (const auto& [daemon_id, doc] : policy_docs) {
    if (!first) {
      out << ',';
    }
    first = false;
    out << doc_state_json(daemon_id, doc);
  }
  out << "],\"groups\":[";
  first = true;
  for (const auto& [name, loads] : latest_group_loads) {
    if (!first) {
      out << ',';
    }
    first = false;
    out << "{\"name\":\"" << name << "\",\"loads\":[";
    for (std::size_t i = 0; i < loads.size(); ++i) {
      if (i != 0) {
        out << ',';
      }
      out << (loads[i] ? fmt_double(*loads[i]) : std::string("null"));
    }
    out << "]}";
  }
  out << "],\"events\":[";
  for (std::size_t i = 0; i < policy_events.size(); ++i) {
    if (i != 0) {
      out << ',';
    }
    out << policy_events[i];
  }
  out << "]}";
  return out.str();
}

std::string bad_policy(const std::string& why) {
  std::cerr << "policies " << why << std::endl;
  return http_response(400, "Bad Request", "{\"ok\":false,\"error\":\"" + json_escape(why) + "\"}", kJson);
}

std::vector<int> read_cpuset_of(const std::string& daemon_id) {
  const auto text = trimmed(read_file("/sys/fs/cgroup/mercury/" + daemon_id + "/cpuset.cpus"));
  if (auto cpus = parse_cpuset(text)) {
    return *cpus;
  }
  return {};
}

bool write_daemon_cpuset(const std::string& daemon_id, const std::vector<int>& want) {
  const auto dir = ensure_daemon_cpuset(daemon_id);
  if (!dir) {
    return false;
  }
  if (!write_sysfs(*dir + "/cpuset.cpus", format_cpuset(want))) {
    return false;
  }
  for (int attempt = 0; attempt < 50; ++attempt) {
    if (cpuset_applied(*dir, want)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

IntervalLoads max_loads(const std::vector<const IntervalLoads*>& parts) {
  IntervalLoads out(static_cast<std::size_t>(kPolicyIntervals));
  for (const auto* loads : parts) {
    for (std::size_t i = 0; i < out.size() && i < loads->size(); ++i) {
      if ((*loads)[i] && (!out[i] || *(*loads)[i] > *out[i])) {
        out[i] = (*loads)[i];
      }
    }
  }
  return out;
}

struct LiveDaemon {
  std::string daemon_id;
  pid_t pid = -1;
  std::vector<std::shared_ptr<Ring>> rings;
};

std::uint64_t recent_task_clock(const std::vector<std::shared_ptr<Ring>>& rings, std::int64_t since_ns,
                                bool& seen) {
  std::uint64_t total = 0;
  for (const auto& ring : rings) {
    if (!ring) {
      continue;
    }
    std::optional<std::size_t> index;
    for (std::size_t i = 0; i < ring->names().size(); ++i) {
      if (ring->names()[i] == "task-clock") {
        index = i;
        break;
      }
    }
    if (!index) {
      continue;
    }
    for (const auto& sample : ring->recent(static_cast<std::size_t>(kIntervalNs / 1000000 / kSampleMs) + 1)) {
      if (sample.timestamp_ns > since_ns && static_cast<int>(*index) < sample.nvalues) {
        total += sample.value[*index];
        seen = true;
      }
    }
  }
  return total;
}

void policy_pass(const std::vector<ResourceGroupInfo>& tree, const std::vector<int>& allocatable) {
  const std::int64_t tick = ++policy_tick;

  // 1. Interval loads per group from the resource rings.
  const auto views = resource_rings(static_cast<std::size_t>(kPolicyIntervals + 1) * 1000 / kSampleMs);
  std::int64_t end_ns = 0;
  for (const auto& view : views) {
    if (!view.samples.empty()) {
      end_ns = std::max(end_ns, view.samples.back().timestamp_ns);
    }
  }
  std::map<std::string, IntervalLoads> loads;
  for (const auto& view : views) {
    loads[view.name] = interval_loads(view.counters, view.samples, end_ns);
  }

  // 2. Running daemons and their documents.
  std::vector<LiveDaemon> live;
  {
    std::lock_guard lock(daemons_mu);
    for (const auto& daemon : daemons) {
      if (!daemon.running) {
        continue;
      }
      LiveDaemon row{daemon.daemon_id, daemon.pid, {}};
      for (const auto& thread : daemon.threads) {
        row.rings.push_back(thread.ring);
      }
      live.push_back(std::move(row));
    }
  }
  std::map<std::string, PolicyDoc> docs;
  {
    std::lock_guard lock(policies_mu);
    latest_group_loads = loads;
    for (auto it = policy_docs.begin(); it != policy_docs.end();) {
      const auto found = std::find_if(live.begin(), live.end(), [&](const LiveDaemon& d) {
        return d.daemon_id == it->first && d.pid == it->second.pid;
      });
      if (found == live.end()) {
        it = policy_docs.erase(it);
        continue;
      }
      docs[it->first] = it->second;
      ++it;
    }
  }
  if (docs.empty()) {
    return;
  }
  std::map<std::string, std::vector<int>> cpusets;
  for (const auto& [daemon_id, _] : docs) {
    cpusets[daemon_id] = read_cpuset_of(daemon_id);
  }
  std::vector<const ResourceGroupInfo*> nodes;
  for (const auto& group : tree) {
    if (group.level == ResourceLevel::Node) {
      nodes.push_back(&group);
    }
  }
  auto loads_of = [&](const std::string& name) -> const IntervalLoads& {
    static const IntervalLoads none(static_cast<std::size_t>(kPolicyIntervals));
    const auto it = loads.find(name);
    return it == loads.end() ? none : it->second;
  };
  std::set<std::string> changed;

  // 3. shift: one daemon leaves a hot node per five intervals.
  static std::map<std::string, std::int64_t> node_left_tick;
  const std::int64_t since_ns = end_ns - kIntervalNs;
  for (const auto* node : nodes) {
    if (tick - (node_left_tick.count(node->name) ? node_left_tick[node->name] : -1000000) < kPolicyIntervals) {
      continue;
    }
    struct Candidate {
      std::string daemon_id;
      std::uint64_t rank = 0;
    };
    std::vector<Candidate> candidates;
    for (const auto& [daemon_id, doc] : docs) {
      if (!doc.shift || tick - doc.attached_tick < kPolicyIntervals || tick - doc.shifted_tick < kPolicyIntervals) {
        continue;
      }
      const auto& cpus = cpusets[daemon_id];
      if (cpus.empty() || cpus_inside(cpus, node->linux_cpus).empty()) {
        continue;
      }
      if (!shift_triggered(loads_of(node->name), doc.rule)) {
        continue;
      }
      const auto daemon = std::find_if(live.begin(), live.end(), [&](const LiveDaemon& d) { return d.daemon_id == daemon_id; });
      bool seen = false;
      const auto rank = recent_task_clock(daemon->rings, since_ns, seen);
      if (!seen) {
        continue;
      }
      candidates.push_back(Candidate{daemon_id, rank});
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
      return a.rank != b.rank ? a.rank > b.rank : a.daemon_id < b.daemon_id;
    });
    for (const auto& candidate : candidates) {
      auto& doc = docs[candidate.daemon_id];
      const auto& cpus = cpusets[candidate.daemon_id];
      std::vector<NodeCandidate> options;
      for (const auto* other : nodes) {
        NodeCandidate option;
        option.name = other->name;
        const auto& other_loads = loads_of(other->name);
        option.load = other_loads.empty() || !other_loads.back() ? 0.0 : *other_loads.back();
        option.lowest_cpu = other->linux_cpus.empty() ? 0 : other->linux_cpus.front();
        option.hot = shift_triggered(other_loads, doc.rule);
        option.inside_ceiling = doc.ceiling_cpus.empty() || cpus_outside(other->linux_cpus, doc.ceiling_cpus).empty();
        option.allocatable = static_cast<int>(cpus_inside(allocatable, other->linux_cpus).size());
        options.push_back(option);
      }
      const auto destination = shift_destination(node->name, options, static_cast<int>(cpus.size()));
      if (!destination) {
        record_event("\"daemon_id\":\"" + json_escape(candidate.daemon_id) + "\",\"action\":\"none\",\"policy\":\"shift\",\"reason\":\"" +
                     node->name + " is hot; no legal node to move to\"");
        continue;
      }
      const auto dest = std::find_if(nodes.begin(), nodes.end(), [&](const ResourceGroupInfo* g) { return g->name == *destination; });
      std::vector<int> exclude = cpus_outside(allocatable, (*dest)->linux_cpus);
      std::vector<int> chosen;
      while (chosen.size() < cpus.size()) {
        const auto next = request_cpu(exclude);
        if (!next) {
          break;
        }
        chosen.push_back(*next);
        exclude.push_back(*next);
      }
      if (chosen.size() < cpus.size()) {
        continue;
      }
      std::sort(chosen.begin(), chosen.end());
      if (!write_daemon_cpuset(candidate.daemon_id, chosen)) {
        std::cerr << "policy shift daemon_id=" << candidate.daemon_id << " cpuset did not take effect" << std::endl;
        continue;
      }
      {
        std::lock_guard lock(daemons_mu);
        if (auto* daemon = find_daemon(candidate.daemon_id); daemon != nullptr && daemon->pid == doc.pid) {
          daemon->cpu = chosen.front();
          daemon->scale.added_at_ns = monotonic_ns();
        }
      }
      doc.shifted_tick = tick;
      node_left_tick[node->name] = tick;
      cpusets[candidate.daemon_id] = chosen;
      changed.insert(candidate.daemon_id);
      const auto& source_loads = loads_of(node->name);
      record_event("\"daemon_id\":\"" + json_escape(candidate.daemon_id) +
                   "\",\"action\":\"move\",\"policy\":\"shift\",\"trigger\":\"" + trigger_name(doc.rule.trigger) +
                   "\",\"from\":\"" + node->name + "\",\"to\":\"" + *destination + "\",\"cpus\":\"" +
                   format_cpuset(chosen) + "\",\"source_load\":" +
                   fmt_double(source_loads.back() ? *source_loads.back() : 0.0) + ",\"reason\":\"hotspot " + node->name +
                   " moved to least-loaded node " + *destination + "\"");
      break;
    }
  }

  // 4. degrade and tune, then one file write per changed daemon.
  for (auto& [daemon_id, doc] : docs) {
    if ((!doc.degrade && !doc.tune) || tick - doc.attached_tick < kPolicyIntervals) {
      continue;
    }
    std::vector<const IntervalLoads*> parts;
    if (!doc.ceiling.empty()) {
      parts.push_back(&loads_of(doc.ceiling));
    } else {
      for (const auto* node : nodes) {
        if (!cpus_inside(cpusets[daemon_id], node->linux_cpus).empty()) {
          parts.push_back(&loads_of(node->name));
        }
      }
    }
    if (parts.empty()) {
      continue;
    }
    const auto u = max_loads(parts);
    bool wrote = false;
    if (doc.degrade && tick - doc.degrade_tick >= kPolicyIntervals) {
      if (const auto index = degrade_step(doc.components, u, doc.degrade_limit)) {
        doc.degrade_tick = tick;
        wrote = true;
        const auto& component = doc.components[*index];
        record_event("\"daemon_id\":\"" + json_escape(daemon_id) + "\",\"action\":\"" +
                     (component.enabled ? "restore" : "degrade") + "\",\"policy\":\"degrade\",\"component\":\"" +
                     component.name + "\",\"load\":" + fmt_double(u.back() ? *u.back() : 0.0) + ",\"limit\":" +
                     fmt_double(doc.degrade_limit) + ",\"reason\":\"load against limit with brownout cost " +
                     fmt_double(component.cost) + "\"");
      }
    }
    if (doc.tune && tick - doc.tune_tick >= kPolicyIntervals) {
      bool degraded = false;
      for (const auto& component : doc.components) {
        degraded = degraded || !component.enabled;
      }
      std::vector<std::size_t> before;
      for (const auto& parameter : doc.parameters) {
        before.push_back(parameter.index);
      }
      if (const auto index = tune_step(doc.parameters, u, degraded, doc.tune_limit)) {
        doc.tune_tick = tick;
        wrote = true;
        const auto& parameter = doc.parameters[*index];
        record_event("\"daemon_id\":\"" + json_escape(daemon_id) + "\",\"action\":\"tune\",\"policy\":\"tune\",\"parameter\":\"" +
                     parameter.name + "\",\"value\":\"" + json_escape(parameter.values[parameter.index]) +
                     "\",\"direction\":\"" + (parameter.index < before[*index] ? "down" : "up") + "\"");
      }
    }
    if (wrote) {
      write_policy_file(doc);
      changed.insert(daemon_id);
    }
  }

  // 5. Publish state back unless the document was replaced meanwhile.
  if (changed.empty()) {
    return;
  }
  std::lock_guard lock(policies_mu);
  for (const auto& daemon_id : changed) {
    auto it = policy_docs.find(daemon_id);
    if (it != policy_docs.end() && it->second.generation == docs[daemon_id].generation) {
      it->second = docs[daemon_id];
    }
  }
}

void policy_loop() {
  // The topology is fixed after init_resources.
  while (!resources_ready()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  const auto tree = resource_groups();
  const auto pool = [] {
    auto cpus = allocatable_cpus();
    std::sort(cpus.begin(), cpus.end());
    return cpus;
  }();
  auto next = std::chrono::steady_clock::now();
  while (true) {
    next += std::chrono::seconds(1);
    std::this_thread::sleep_until(next);
    try {
      policy_pass(tree, pool);
    } catch (const std::exception& error) {
      std::cerr << "policy_loop " << error.what() << std::endl;
    }
  }
}

}  // namespace

void start_policy_loop() { std::thread(policy_loop).detach(); }

std::optional<std::string> policies_route(const std::string& request) {
  const auto target = request_target(request);
  if (target != "/policies") {
    return std::nullopt;
  }
  if (request.rfind("GET ", 0) == 0) {
    return http_response(200, "OK", policies_state_json(), kJson);
  }
  const auto parsed = parse_json(request_body(request));
  if (!parsed || !parsed->is_object()) {
    return bad_policy("body is not a JSON object");
  }
  const auto* id_value = parsed->get("daemon_id");
  if (id_value == nullptr || !id_value->is_string() || !safe_daemon_id(id_value->text)) {
    return bad_policy("daemon_id is missing or unsafe");
  }
  const std::string daemon_id = id_value->text;
  PolicyDoc doc;
  bool empty = true;
  if (const auto why = parse_policy_doc(*parsed, doc, empty); !why.empty()) {
    if (why.rfind("widen", 0) == 0) {
      return http_response(501, "Not Implemented", "{\"ok\":false,\"error\":\"" + why + "\"}", kJson);
    }
    return bad_policy(why);
  }
  if (!resources_ready()) {
    return http_response(500, "Internal Server Error", ok_json(false), kJson);
  }
  pid_t pid = -1;
  {
    std::lock_guard lock(daemons_mu);
    const auto* daemon = find_daemon(daemon_id);
    if (daemon == nullptr || !daemon->running) {
      return bad_policy("daemon " + daemon_id + " is not running");
    }
    if (const auto* exec_path = parsed->get("exec_path")) {
      if (!exec_path->is_string() || exec_path->text != daemon->exec_path) {
        return bad_policy("exec_path does not match the running daemon");
      }
    }
    pid = daemon->pid;
  }
  const auto path = rings_dir() + "/" + archive_name(daemon_id) + ".policy";
  std::vector<int> outside;
  if (!doc.ceiling.empty()) {
    const auto groups = resource_groups();
    const auto found = std::find_if(groups.begin(), groups.end(), [&](const ResourceGroupInfo& g) { return g.name == doc.ceiling; });
    if (found == groups.end()) {
      return bad_policy("unknown ceiling " + doc.ceiling);
    }
    doc.ceiling_cpus = found->linux_cpus;
    std::sort(doc.ceiling_cpus.begin(), doc.ceiling_cpus.end());
    if (!cpus_outside(read_cpuset_of(daemon_id), doc.ceiling_cpus).empty()) {
      return bad_policy("cpuset sticks out of ceiling " + doc.ceiling);
    }
    outside = cpus_outside(allocatable_cpus(), doc.ceiling_cpus);
  }
  {
    std::lock_guard lock(daemons_mu);
    if (auto* daemon = find_daemon(daemon_id); daemon != nullptr && daemon->pid == pid) {
      daemon->scale.outside = outside;
    }
  }
  if (empty) {
    {
      std::lock_guard lock(policies_mu);
      policy_docs.erase(daemon_id);
    }
    std::remove(path.c_str());
    record_event("\"daemon_id\":\"" + json_escape(daemon_id) + "\",\"action\":\"detach\"");
    return http_response(200, "OK", "{\"ok\":true}", kJson);
  }
  doc.pid = pid;
  doc.attached_tick = policy_tick.load();
  bool wrote = false;
  if (doc.degrade || doc.tune) {
    std::filesystem::create_directories(rings_dir());
    doc.path = path;
    wrote = write_policy_file(doc);
    if (!wrote) {
      std::cerr << "policies daemon_id=" << daemon_id << " could not write " << path << std::endl;
      doc.path.clear();
    }
  } else {
    std::remove(path.c_str());
  }
  {
    std::lock_guard lock(policies_mu);
    doc.generation = ++policy_generation;
    policy_docs[daemon_id] = doc;
  }
  record_event("\"daemon_id\":\"" + json_escape(daemon_id) + "\",\"action\":\"attach\",\"shift\":" +
               (doc.shift ? "true" : "false") + ",\"trigger\":\"" + trigger_name(doc.rule.trigger) +
               "\",\"ceiling\":\"" + json_escape(doc.ceiling) + "\",\"components\":" +
               std::to_string(doc.components.size()) + ",\"parameters\":" + std::to_string(doc.parameters.size()));
  std::string body = "{\"ok\":true";
  if (wrote) {
    body += ",\"path\":\"" + json_escape(path) + "\"";
  }
  body += "}";
  return http_response(200, "OK", body, kJson);
}
