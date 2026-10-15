#include "scale_stats.hpp"

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr int kMercuryPort = 18086;
constexpr int kServerPort = 18090;
constexpr int kMercuryLinuxCpu = 0;
constexpr int kTestLinuxCpu = 58;
constexpr int kInitialLinuxCpu = 60;
constexpr const char* kServerId = "scale_server";
constexpr const char* kClientId = "scale_client";
constexpr const char* kParent = "/sys/fs/cgroup/mercury";
constexpr const char* kRootControl = "/sys/fs/cgroup/cgroup.subtree_control";
constexpr const char* kParentControl = "/sys/fs/cgroup/mercury/cgroup.subtree_control";
constexpr const char* kGoPath = "/tmp/mercury-scale-go";
constexpr const char* kStatsPath = "/tmp/mercury-scale-stats";
constexpr std::int64_t kMeasureNs = 1000000000LL;
constexpr int kScaleCores[] = {1, 2, 4, 8, 16, 32};

pid_t g_server = -1;
volatile sig_atomic_t g_remove_parent = 0;
volatile sig_atomic_t g_restore_root = 0;
ScaleStats* g_stats = nullptr;

class Socket {
 public:
  explicit Socket(int fd) : fd_(fd) {}
  ~Socket() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  int get() const { return fd_; }

 private:
  int fd_;
};

void sleep_ms(int ms) {
  timespec delay{};
  delay.tv_sec = ms / 1000;
  delay.tv_nsec = static_cast<long>(ms % 1000) * 1000000L;
  ::nanosleep(&delay, nullptr);
}

std::int64_t monotonic_ns() {
  timespec now{};
  if (::clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    throw std::runtime_error("clock_gettime failed");
  }
  return static_cast<std::int64_t>(now.tv_sec) * 1000000000LL + now.tv_nsec;
}

std::string trim(std::string value) {
  while (!value.empty() && (value.back() == '\n' || value.back() == '\r' || value.back() == ' ' || value.back() == '\t')) {
    value.pop_back();
  }
  std::size_t start = 0;
  while (start < value.size() && (value[start] == ' ' || value[start] == '\t')) {
    ++start;
  }
  return value.substr(start);
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

std::string status_field(const std::string& status, const std::string& key) {
  const std::string prefix = key + ":";
  std::istringstream lines(status);
  std::string line;
  while (std::getline(lines, line)) {
    if (line.rfind(prefix, 0) == 0) {
      return trim(line.substr(prefix.size()));
    }
  }
  return {};
}

std::string dirname_of(const std::string& path) {
  const auto slash = path.rfind('/');
  if (slash == std::string::npos) {
    return ".";
  }
  if (slash == 0) {
    return "/";
  }
  return path.substr(0, slash);
}

std::string exe_dir() {
  char buf[4096];
  const auto n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n < 0) {
    throw std::runtime_error("readlink /proc/self/exe failed");
  }
  buf[n] = '\0';
  return dirname_of(buf);
}

void send_all(int fd, std::string_view message) {
  while (!message.empty()) {
    const auto n = ::send(fd, message.data(), message.size(), MSG_NOSIGNAL);
    if (n < 0) {
      throw std::runtime_error("send failed");
    }
    message.remove_prefix(static_cast<std::size_t>(n));
  }
}

std::string recv_all(int fd) {
  std::string response;
  char buffer[1024];
  while (true) {
    const auto n = ::recv(fd, buffer, sizeof(buffer), 0);
    if (n == 0) {
      return response;
    }
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw std::runtime_error("recv failed");
    }
    response.append(buffer, static_cast<std::size_t>(n));
  }
}

std::string http_exchange(int port, const std::string& request) {
  Socket sock{::socket(AF_INET, SOCK_STREAM, 0)};
  if (sock.get() < 0) {
    throw std::runtime_error("socket failed");
  }
  timeval timeout{};
  timeout.tv_sec = 5;
  ::setsockopt(sock.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  ::setsockopt(sock.get(), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<std::uint16_t>(port));
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(sock.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
    throw std::runtime_error("connect failed");
  }
  send_all(sock.get(), request);
  return recv_all(sock.get());
}

std::string response_body(const std::string& response) {
  const auto at = response.find("\r\n\r\n");
  if (at == std::string::npos) {
    return response;
  }
  return response.substr(at + 4);
}

int response_status(const std::string& response) {
  const auto line_end = response.find("\r\n");
  const auto line = response.substr(0, line_end == std::string::npos ? response.size() : line_end);
  const auto first = line.find(' ');
  if (first == std::string::npos) {
    return 0;
  }
  try {
    return std::stoi(line.substr(first + 1));
  } catch (const std::exception&) {
    return 0;
  }
}

int response_pid(const std::string& response) {
  const auto body = response_body(response);
  const auto key = body.find("\"pid\"");
  if (key == std::string::npos) {
    return -1;
  }
  try {
    return std::stoi(body.substr(body.find(':', key) + 1));
  } catch (const std::exception&) {
    return -1;
  }
}

std::string json_escape(const std::string& text) {
  std::string out;
  for (const char c : text) {
    if (c == '\\' || c == '"') {
      out.push_back('\\');
    }
    out.push_back(c);
  }
  return out;
}

pid_t start_mercury(const std::string& mercury) {
  const auto port = std::to_string(kMercuryPort);
  const auto self = std::to_string(kMercuryLinuxCpu);
  const auto daemon = std::to_string(kInitialLinuxCpu);
  const pid_t pid = ::fork();
  if (pid < 0) {
    throw std::runtime_error("fork failed");
  }
  if (pid == 0) {
    if (::setpgid(0, 0) != 0) {
      _exit(127);
    }
    const int devnull = ::open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      ::dup2(devnull, STDOUT_FILENO);
      ::close(devnull);
    }
    ::execl(mercury.c_str(), mercury.c_str(), port.c_str(), self.c_str(), daemon.c_str(), nullptr);
    _exit(127);
  }
  ::setpgid(pid, pid);
  g_server = pid;
  return pid;
}

void stop_server() {
  const pid_t server = g_server;
  if (server <= 0) {
    return;
  }
  g_server = -1;
  ::kill(-server, SIGTERM);
  for (int i = 0; i < 20; ++i) {
    const auto got = ::waitpid(server, nullptr, WNOHANG);
    if (got == server || (got < 0 && errno == ECHILD)) {
      return;
    }
    sleep_ms(50);
  }
  ::kill(-server, SIGKILL);
  while (::waitpid(server, nullptr, 0) < 0 && errno == EINTR) {
  }
}

bool has_controller(const std::string& text, const std::string& name) {
  std::istringstream in(text);
  std::string token;
  while (in >> token) {
    if (token == name) {
      return true;
    }
  }
  return false;
}

bool is_dir(const std::string& path) {
  struct stat st {};
  return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

void write_sysfs_best_effort(const char* path, const char* value) {
  const int fd = ::open(path, O_WRONLY | O_CLOEXEC);
  if (fd < 0) {
    return;
  }
  const auto len = std::strlen(value);
  std::size_t sent = 0;
  while (sent < len) {
    const auto n = ::write(fd, value + sent, len - sent);
    if (n < 0) {
      break;
    }
    sent += static_cast<std::size_t>(n);
  }
  ::close(fd);
}

void rmdir_id(const char* id) {
  const auto path = std::string(kParent) + "/" + id;
  ::rmdir(path.c_str());
}

bool parent_has_children() {
  DIR* dir = ::opendir(kParent);
  if (dir == nullptr) {
    return false;
  }
  bool found = false;
  while (const auto* entry = ::readdir(dir)) {
    if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    if (is_dir(std::string(kParent) + "/" + entry->d_name)) {
      found = true;
      break;
    }
  }
  ::closedir(dir);
  return found;
}

void cleanup_cgroups() {
  rmdir_id(kServerId);
  rmdir_id(kClientId);
  if (g_remove_parent && !parent_has_children()) {
    write_sysfs_best_effort(kParentControl, "-cpuset\n");
    ::rmdir(kParent);
  }
  if (g_restore_root) {
    write_sysfs_best_effort(kRootControl, "-cpuset\n");
  }
}

void on_signal(int) {
  if (g_server > 0) {
    ::kill(-g_server, SIGKILL);
  }
  ::unlink(kGoPath);
  ::rmdir((std::string(kParent) + "/" + kServerId).c_str());
  ::rmdir((std::string(kParent) + "/" + kClientId).c_str());
  if (g_remove_parent) {
    write_sysfs_best_effort(kParentControl, "-cpuset\n");
    ::rmdir(kParent);
  }
  if (g_restore_root) {
    write_sysfs_best_effort(kRootControl, "-cpuset\n");
  }
  _exit(128);
}

void wait_until_listening() {
  const std::string request = "GET /ping HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
  for (int attempt = 0; attempt < 50; ++attempt) {
    int status = 0;
    if (::waitpid(g_server, &status, WNOHANG) == g_server) {
      g_server = -1;
      throw std::runtime_error("mercury exited before listening");
    }
    try {
      if (response_body(http_exchange(kMercuryPort, request)).find("pong") != std::string::npos) {
        return;
      }
    } catch (const std::exception&) {
    }
    sleep_ms(50);
  }
  throw std::runtime_error("mercury did not answer");
}

std::string http_post(const std::string& target, const std::string& body) {
  const std::string request = "POST " + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: " +
                              std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
  return http_exchange(kMercuryPort, request);
}

pid_t start_daemon(const std::string& id, const std::string& path, bool listen) {
  std::string body = std::string("{\"daemon_id\":\"") + id + "\",\"exec_path\":\"" + json_escape(path) +
                     "\",\"cpu\":" + std::to_string(kInitialLinuxCpu);
  if (listen) {
    body += ",\"listen_port\":" + std::to_string(kServerPort);
  }
  body += "}";
  const auto response = http_post("/start", body);
  const pid_t pid = response_pid(response);
  if (response_status(response) != 200 || pid <= 0) {
    throw std::runtime_error("start " + id + " failed: " + trim(response_body(response)));
  }
  return pid;
}

void expect_rejected_cpus(const std::string& id, const std::string& spec) {
  const std::string body = std::string("{\"daemon_id\":\"") + id + "\",\"cpus\":\"" + spec + "\"}";
  const auto response = http_post("/cpus", body);
  if (response_status(response) != 400 || response_body(response) != R"({"ok":false})") {
    throw std::runtime_error("cpus " + id + " " + spec + " was not rejected: " + trim(response_body(response)));
  }
}

std::int64_t set_cpus(const std::string& id, const std::string& spec) {
  const std::string body = std::string("{\"daemon_id\":\"") + id + "\",\"cpus\":\"" + spec + "\"}";
  const auto began = monotonic_ns();
  const auto response = http_post("/cpus", body);
  const auto elapsed = monotonic_ns() - began;
  if (response_status(response) != 200 || response_body(response) != R"({"ok":true})") {
    throw std::runtime_error("cpus " + id + " " + spec + " failed: " + trim(response_body(response)));
  }
  return elapsed;
}

std::vector<int> task_ids(pid_t pid) {
  std::vector<int> ids;
  const auto path = "/proc/" + std::to_string(pid) + "/task";
  DIR* dir = ::opendir(path.c_str());
  if (dir == nullptr) {
    return ids;
  }
  while (const auto* entry = ::readdir(dir)) {
    if (entry->d_name[0] >= '1' && entry->d_name[0] <= '9') {
      ids.push_back(std::atoi(entry->d_name));
    }
  }
  ::closedir(dir);
  return ids;
}

std::string allowed_of(pid_t pid, int tid) {
  const auto status = read_file("/proc/" + std::to_string(pid) + "/task/" + std::to_string(tid) + "/status");
  return status_field(status, "Cpus_allowed_list");
}

void wait_until_allowed(pid_t pid, const std::string& spec) {
  for (int attempt = 0; attempt < 50; ++attempt) {
    const auto ids = task_ids(pid);
    bool ok = !ids.empty();
    for (const int tid : ids) {
      const auto allowed = allowed_of(pid, tid);
      if (allowed.empty()) {
        continue;
      }
      if (allowed != spec) {
        ok = false;
        break;
      }
    }
    if (ok) {
      return;
    }
    sleep_ms(20);
  }
  throw std::runtime_error("pid " + std::to_string(pid) + " did not settle on " + spec + ", saw " +
                           allowed_of(pid, pid));
}

void wait_until_port(int port) {
  for (int attempt = 0; attempt < 50; ++attempt) {
    Socket sock{::socket(AF_INET, SOCK_STREAM, 0)};
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (sock.get() >= 0 && ::connect(sock.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
      return;
    }
    sleep_ms(50);
  }
  throw std::runtime_error("server did not accept");
}

std::string cpu_spec(int cores) {
  if (cores <= 1) {
    return "1";
  }
  return "1-" + std::to_string(cores);
}

std::string window_spec(int start, int width) {
  return std::to_string(start) + "-" + std::to_string(start + width - 1);
}

std::atomic_ref<std::uint64_t> stat_ref(std::uint64_t& slot) { return std::atomic_ref<std::uint64_t>(slot); }

std::uint64_t load_completed() {
  std::uint64_t total = 0;
  for (auto& worker : g_stats->workers) {
    total += stat_ref(worker.completed).load(std::memory_order_relaxed);
  }
  return total;
}

std::uint64_t load_errors() {
  std::uint64_t total = 0;
  for (auto& worker : g_stats->workers) {
    total += stat_ref(worker.late).load(std::memory_order_relaxed);
    total += stat_ref(worker.failed).load(std::memory_order_relaxed);
  }
  return total;
}

std::uint64_t load_failed() {
  std::uint64_t total = 0;
  for (auto& worker : g_stats->workers) {
    total += stat_ref(worker.failed).load(std::memory_order_relaxed);
  }
  return total;
}

std::uint64_t load_service_ns() {
  std::uint64_t total = 0;
  for (auto& worker : g_stats->workers) {
    total += stat_ref(worker.service_ns).load(std::memory_order_relaxed);
  }
  return total;
}

std::uint64_t load_sent() {
  std::uint64_t total = 0;
  for (auto& worker : g_stats->workers) {
    total += stat_ref(worker.sent).load(std::memory_order_relaxed);
  }
  return total;
}

std::uint64_t per_second(std::uint64_t count, std::int64_t elapsed_ns) {
  return elapsed_ns > 0 ? count * 1000000000ULL / static_cast<std::uint64_t>(elapsed_ns) : 0;
}

std::string error_pct(std::uint64_t errors, std::uint64_t attempts) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(2);
  if (attempts == 0) {
    out << "0.00";
  } else {
    out << (100.0 * static_cast<double>(errors) / static_cast<double>(attempts));
  }
  return out.str();
}

struct Sample {
  std::uint64_t arrivals = 0;
  std::uint64_t arrival_rate = 0;
  std::uint64_t requests = 0;
  std::uint64_t errors = 0;
  std::uint64_t attempts = 0;
  std::int64_t elapsed_ns = 0;
  std::uint64_t rate_per_s = 0;
  std::uint64_t service_ns = 0;
};

Sample measure() {
  const auto before_sent = load_sent();
  const auto before_completed = load_completed();
  const auto before_service = load_service_ns();
  const auto before_errors = load_errors();
  const auto before_failed = load_failed();
  const auto began = monotonic_ns();
  sleep_ms(static_cast<int>(kMeasureNs / 1000000LL));
  const auto elapsed = monotonic_ns() - began;
  const auto requests = load_completed() - before_completed;
  const auto failed = load_failed() - before_failed;
  Sample sample;
  sample.arrivals = load_sent() - before_sent;
  sample.arrival_rate = per_second(sample.arrivals, elapsed);
  sample.requests = requests;
  sample.errors = load_errors() - before_errors;
  sample.attempts = requests + failed;
  sample.elapsed_ns = elapsed;
  sample.rate_per_s = per_second(requests, elapsed);
  sample.service_ns = requests > 0 ? (load_service_ns() - before_service) / requests : 0;
  return sample;
}

std::string load_fields(const Sample& sample) {
  std::ostringstream out;
  out << "arrivals " << sample.arrivals << " arrival_rate " << sample.arrival_rate << " requests " << sample.requests
      << " elapsed_ns " << sample.elapsed_ns << " rate_per_s " << sample.rate_per_s << " service_ns " << sample.service_ns
      << " errors " << sample.errors
      << " error_pct " << error_pct(sample.errors, sample.attempts);
  return out.str();
}

void prepare_stats() {
  ::unlink(kGoPath);
  const int fd = ::open(kStatsPath, O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    throw std::runtime_error("create stats file failed");
  }
  if (::ftruncate(fd, sizeof(ScaleStats)) != 0) {
    ::close(fd);
    throw std::runtime_error("ftruncate stats failed");
  }
  void* mapped = ::mmap(nullptr, sizeof(ScaleStats), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  ::close(fd);
  if (mapped == MAP_FAILED) {
    throw std::runtime_error("mmap stats failed");
  }
  g_stats = static_cast<ScaleStats*>(mapped);
  for (auto& worker : g_stats->workers) {
    stat_ref(worker.sent).store(0, std::memory_order_relaxed);
    stat_ref(worker.completed).store(0, std::memory_order_relaxed);
    stat_ref(worker.service_ns).store(0, std::memory_order_relaxed);
    stat_ref(worker.late).store(0, std::memory_order_relaxed);
    stat_ref(worker.failed).store(0, std::memory_order_relaxed);
  }
}

}  // namespace

int main() {
  struct sigaction action {};
  action.sa_handler = on_signal;
  ::sigaction(SIGINT, &action, nullptr);
  ::sigaction(SIGTERM, &action, nullptr);

  try {
    if (::geteuid() != 0) {
      throw std::runtime_error("scale_test must run as root to write /sys/fs/cgroup");
    }
    cpu_set_t self;
    CPU_ZERO(&self);
    CPU_SET(kTestLinuxCpu, &self);
    if (::sched_setaffinity(0, sizeof(self), &self) != 0) {
      throw std::runtime_error("failed to pin scale_test");
    }
    const auto bindir = dirname_of(exe_dir());
    const auto testdir = exe_dir();
    const auto mercury = bindir + "/mercury";
    const auto server_bin = testdir + "/scale_server";
    const auto client_bin = testdir + "/scale_client";
    if (::access(mercury.c_str(), X_OK) != 0 || ::access(server_bin.c_str(), X_OK) != 0 ||
        ::access(client_bin.c_str(), X_OK) != 0) {
      throw std::runtime_error("missing mercury, scale_server, or scale_client");
    }

    g_remove_parent = is_dir(kParent) ? 0 : 1;
    g_restore_root = has_controller(read_file(kRootControl), "cpuset") ? 0 : 1;
    ::setenv("SCALE_STATS", kStatsPath, 1);
    ::setenv("SCALE_GO", kGoPath, 1);
    prepare_stats();

    start_mercury(mercury);
    wait_until_listening();

    const pid_t server_pid = start_daemon(kServerId, server_bin, true);
    wait_until_port(kServerPort);
    const pid_t client_pid = start_daemon(kClientId, client_bin, false);
    const auto client_set = set_cpus(kClientId, "60-63");
    wait_until_allowed(client_pid, "60-63");
    std::cout << "client cpus 60-63 allowed " << allowed_of(client_pid, client_pid) << " set_ns " << client_set
              << " deadline_ns " << kResponseDeadlineNs << std::endl;
    std::cout << "mercury cpu " << kMercuryLinuxCpu << " allowed " << allowed_of(g_server, g_server) << std::endl;
    expect_rejected_cpus(kServerId, "0");
    expect_rejected_cpus(kServerId, "0-31");
    std::cout << "rejected cpus 0 and 0-31" << std::endl;

    std::uint64_t steady_rate = 0;
    std::cout << "scale" << std::endl;
    for (const int cores : kScaleCores) {
      const auto spec = cpu_spec(cores);
      const auto set_ns = set_cpus(kServerId, spec);
      wait_until_allowed(server_pid, spec);
      if (::access(kGoPath, F_OK) != 0) {
        const int fd = ::open(kGoPath, O_CREAT | O_WRONLY, 0644);
        if (fd < 0) {
          throw std::runtime_error("create go file failed");
        }
        ::close(fd);
        const auto began = monotonic_ns();
        while (load_completed() == 0) {
          if (monotonic_ns() - began > 5000000000LL) {
            throw std::runtime_error("client did not start calling");
          }
          sleep_ms(20);
        }
      }
      sleep_ms(200);
      const auto sample = measure();
      if (cores == 8) {
        steady_rate = sample.rate_per_s;
      }
      std::cout << "cores " << cores << " cpus " << spec << " allowed " << allowed_of(server_pid, server_pid)
                << " set_ns " << set_ns << " " << load_fields(sample) << std::endl;
    }

    std::cout << "move" << std::endl;
    const auto sweep_sent_before = load_sent();
    const auto sweep_before = load_completed();
    const auto sweep_service_before = load_service_ns();
    const auto sweep_errors_before = load_errors();
    const auto sweep_failed_before = load_failed();
    const auto sweep_began = monotonic_ns();
    int windows = 0;
    std::int64_t move_ns = 0;
    for (int start = 1; start + 8 - 1 <= 32; ++start) {
      const auto spec = window_spec(start, 8);
      const auto sent_before = load_sent();
      const auto before = load_completed();
      const auto service_before = load_service_ns();
      const auto errors_before = load_errors();
      const auto failed_before = load_failed();
      const auto elapsed = set_cpus(kServerId, spec);
      const auto requests = load_completed() - before;
      const auto failed = load_failed() - failed_before;
      Sample sample;
      sample.arrivals = load_sent() - sent_before;
      sample.arrival_rate = per_second(sample.arrivals, elapsed);
      sample.requests = requests;
      sample.errors = load_errors() - errors_before;
      sample.attempts = requests + failed;
      sample.elapsed_ns = elapsed;
      sample.rate_per_s = per_second(requests, elapsed);
      sample.service_ns = requests > 0 ? (load_service_ns() - service_before) / requests : 0;
      move_ns += elapsed;
      ++windows;
      std::cout << "cpus " << spec << " " << load_fields(sample) << std::endl;
    }
    const auto sweep_elapsed = monotonic_ns() - sweep_began;
    const auto sweep_arrivals = load_sent() - sweep_sent_before;
    const auto sweep_requests = load_completed() - sweep_before;
    const auto sweep_service = sweep_requests > 0 ? (load_service_ns() - sweep_service_before) / sweep_requests : 0;
    const auto sweep_failed = load_failed() - sweep_failed_before;
    const auto sweep_errors = load_errors() - sweep_errors_before;
    const auto sweep_rate = per_second(sweep_requests, sweep_elapsed);
    std::cout << "windows " << windows << " move_ns " << move_ns << " elapsed_ns " << sweep_elapsed << " arrivals "
              << sweep_arrivals << " arrival_rate " << per_second(sweep_arrivals, sweep_elapsed) << " requests "
              << sweep_requests << " rate_per_s " << sweep_rate << " service_ns " << sweep_service << " errors "
              << sweep_errors << " error_pct "
              << error_pct(sweep_errors, sweep_requests + sweep_failed) << " steady_rate_per_s " << steady_rate
              << std::endl;

    http_post("/kill", std::string("{\"daemon_id\":\"") + kClientId + "\"}");
    http_post("/kill", std::string("{\"daemon_id\":\"") + kServerId + "\"}");
    stop_server();
    ::unlink(kGoPath);
    ::unlink(kStatsPath);
    cleanup_cgroups();
  } catch (const std::exception& error) {
    stop_server();
    ::unlink(kGoPath);
    try {
      cleanup_cgroups();
    } catch (const std::exception& cleanup_error) {
      std::cerr << cleanup_error.what() << std::endl;
    }
    std::cerr << error.what() << std::endl;
    return 1;
  }
}
