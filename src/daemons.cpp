#include "daemons.hpp"

#include "counters.hpp"
#include "http.hpp"
#include "policy.hpp"
#include "ring.hpp"
#include "ring_archive.hpp"

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sched.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

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

std::optional<std::string> json_value(const std::string& json, std::string_view key) {
  const std::string pattern = std::string("\"") + std::string(key) + "\"";
  std::size_t from = 0;
  std::size_t key_at = std::string::npos;
  while (from < json.size()) {
    const auto found = json.find(pattern, from);
    if (found == std::string::npos) {
      return std::nullopt;
    }
    auto after = found + pattern.size();
    while (after < json.size() && std::isspace(static_cast<unsigned char>(json[after])) != 0) {
      ++after;
    }
    if (after < json.size() && json[after] == ':') {
      key_at = found;
      break;
    }
    from = found + 1;
  }
  if (key_at == std::string::npos) {
    return std::nullopt;
  }
  auto at = json.find(':', key_at + pattern.size());
  if (at == std::string::npos) {
    return std::nullopt;
  }
  ++at;
  while (at < json.size() && std::isspace(static_cast<unsigned char>(json[at])) != 0) {
    ++at;
  }
  if (at >= json.size()) {
    return std::nullopt;
  }
  if (json[at] == '"') {
    std::string value;
    for (std::size_t i = at + 1; i < json.size(); ++i) {
      if (json[i] == '\\' && i + 1 < json.size()) {
        value.push_back(json[++i]);
        continue;
      }
      if (json[i] == '"') {
        return value;
      }
      value.push_back(json[i]);
    }
    return std::nullopt;
  }
  const auto end = json.find_first_of(",}", at);
  auto token = json.substr(at, end == std::string::npos ? std::string::npos : end - at);
  while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back())) != 0) {
    token.pop_back();
  }
  return token;
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

void set_mercury_cpu(int cpu) { mercury_linux_cpu = cpu - 1; }

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

std::optional<Launched> launch(const std::string& daemon_id, const std::string& exec_path, int cpu, int listen_fd) {
  const int linux_cpu = cpu - 1;
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
    std::vector<char> arg0(exec_path.begin(), exec_path.end());
    arg0.push_back('\0');
    char flag[] = "--listen-fd";
    char fd_text[] = "3";
    char* with_socket[] = {arg0.data(), flag, fd_text, nullptr};
    char* plain[] = {arg0.data(), nullptr};
    ::execv(exec_path.c_str(), listen_fd >= 0 ? with_socket : plain);
    _exit(127);
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

std::vector<int> daemon_linux_pool() {
  std::vector<int> pool;
  for (const int cpu : daemon_cpus) {
    const int linux_cpu = cpu - 1;
    if (linux_cpu >= 0 && linux_cpu != mercury_linux_cpu) {
      pool.push_back(linux_cpu);
    }
  }
  return pool;
}

int cpuset_cores(const std::string& daemon_id) {
  const auto have = parse_cpuset(trimmed(read_file("/sys/fs/cgroup/mercury/" + daemon_id + "/cpuset.cpus")));
  if (!have) {
    return 0;
  }
  return static_cast<int>(have->size());
}

bool add_core(const std::string& daemon_id, int max_cores, int& cores_after, std::string& spec_after) {
  const auto dir = ensure_daemon_cpuset(daemon_id);
  if (!dir) {
    return false;
  }
  auto have = parse_cpuset(trimmed(read_file(*dir + "/cpuset.cpus")));
  if (!have || have->empty() || static_cast<int>(have->size()) >= max_cores) {
    return false;
  }
  int next = -1;
  for (const int cpu : daemon_linux_pool()) {
    if (std::find(have->begin(), have->end(), cpu) == have->end()) {
      next = cpu;
      break;
    }
  }
  if (next < 0) {
    return false;
  }
  have->push_back(next);
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
// task-clock only, which is what core scaling reads, so a wide daemon stays
// inside the process file limit.
constexpr std::size_t kFullCounterThreads = 16;

void monitor_daemon(const std::string& daemon_id, pid_t root) {
  std::vector<LiveThread> live;
  try {
    while (daemon_running(daemon_id, root)) {
      const auto want = daemon_tasks(root);
      for (auto it = live.begin(); it != live.end();) {
        if (std::find(want.begin(), want.end(), it->tid) == want.end()) {
          it->ring->flush();
          forget_thread(daemon_id, root, it->tid);
          it = live.erase(it);
        } else {
          ++it;
        }
      }
      for (const auto tid : want) {
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
          std::cerr << "counters daemon_id=" << daemon_id << " tid=" << tid << " " << error.what() << std::endl;
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
      if (decision == Decision::Hold) {
        continue;
      }
      int after = 0;
      std::string spec;
      const bool changed = decision == Decision::AddCore
                               ? add_core(daemon_id, scale.max_cores, after, spec)
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
  mark_counters_done(daemon_id, root);
}

void arm_daemon(const std::string& daemon_id, const std::string& exec_path, int cpu, int listen_fd, pid_t pid,
                int out_fd, int err_fd) {
  {
    std::lock_guard lock(daemons_mu);
    Daemon created;
    created.daemon_id = daemon_id;
    created.exec_path = exec_path;
    created.pid = pid;
    created.running = true;
    created.cpu = cpu;
    created.listen_fd = listen_fd;
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

  const auto launched = launch(*daemon_id, *exec_path, cpu, listen_fd);
  if (!launched) {
    if (listen_fd >= 0) {
      ::close(listen_fd);
    }
    return http_response(500, "Internal Server Error", R"({"pid":0})", kJson);
  }
  arm_daemon(*daemon_id, *exec_path, cpu, listen_fd, launched->pid, launched->out_fd, launched->err_fd);
  return http_response(200, "OK", "{\"pid\":" + std::to_string(launched->pid) + "}", kJson);
}

std::optional<std::string> migrate_route(const std::string& request) {
  if (request_target(request) != "/migrate") {
    return std::nullopt;
  }
  const auto body = request_body(request);
  const auto daemon_id = json_value(body, "daemon_id");
  const auto cpu_text = json_value(body, "cpu");
  if (!daemon_id || daemon_id->empty() || !cpu_text) {
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

  std::string exec_path;
  int listen_fd = -1;
  pid_t old_pid = -1;
  int old_cpu = 0;
  {
    std::lock_guard lock(daemons_mu);
    const auto* daemon = find_daemon(*daemon_id);
    if (daemon == nullptr || !daemon->running || daemon->listen_fd < 0 || daemon->cpu == cpu) {
      return http_response(400, "Bad Request", R"({"pid":0})", kJson);
    }
    exec_path = daemon->exec_path;
    listen_fd = daemon->listen_fd;
    old_pid = daemon->pid;
    old_cpu = daemon->cpu;
  }

  const auto launched = launch(*daemon_id, exec_path, cpu, listen_fd);
  if (!launched || !wait_until_image(launched->pid, exec_path)) {
    if (launched) {
      discard_launch(*launched);
    }
    return http_response(500, "Internal Server Error", R"({"pid":0})", kJson);
  }

  arm_daemon(*daemon_id, exec_path, cpu, listen_fd, launched->pid, launched->out_fd, launched->err_fd);
  ::kill(old_pid, SIGTERM);
  for (int attempt = 0; attempt < 20; ++attempt) {
    if (::kill(old_pid, 0) < 0 && errno == ESRCH) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (::kill(old_pid, 0) == 0) {
    ::kill(old_pid, SIGKILL);
  }
  std::cout << "migrated daemon_id=" << *daemon_id << " pid " << old_pid << " cpu " << old_cpu << " -> pid "
            << launched->pid << " cpu " << cpu << std::endl;
  return http_response(200, "OK", "{\"pid\":" + std::to_string(launched->pid) + "}", kJson);
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
      std::cout << "cpus daemon_id=" << *daemon_id << " cpus=" << trimmed(*cpus_text) << std::endl;
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
  return http_response(200, "OK", scale_json(min_cores, max_cores, cpuset_cores(*daemon_id), threads), kJson);
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
