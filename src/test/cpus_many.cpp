#include <arpa/inet.h>
#include <barrier>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sched.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr int kPort = 18083;
constexpr int kMercuryLinuxCpu = 0;
constexpr int kDefaultRow = 16;
constexpr int kDefaultClients = 8;
constexpr int kMaxClients = 16;
constexpr const char* kParent = "/sys/fs/cgroup/mercury";
constexpr const char* kRootControl = "/sys/fs/cgroup/cgroup.subtree_control";
constexpr const char* kParentControl = "/sys/fs/cgroup/mercury/cgroup.subtree_control";
constexpr const char* kRowId = "row";

pid_t g_server = -1;
volatile sig_atomic_t g_clients = 0;
volatile sig_atomic_t g_remove_parent = 0;
volatile sig_atomic_t g_restore_root = 0;

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

struct Sample {
  std::string id;
  std::string spec;
  int cpu = -1;
  std::int64_t began = 0;
  std::int64_t elapsed_ns = 0;
  std::string error;
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
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\n' ||
                            value.back() == '\r')) {
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

std::string daemon_path(const std::string& id) { return std::string(kParent) + "/" + id; }

void rmdir_id(const char* id) {
  char path[96];
  char* out = path;
  for (const char* p = "/sys/fs/cgroup/mercury/"; *p != '\0'; ++p) {
    *out++ = *p;
  }
  for (const char* p = id; *p != '\0'; ++p) {
    *out++ = *p;
  }
  *out = '\0';
  ::rmdir(path);
}

void rmdir_client(int index) {
  char id[8];
  id[0] = 'c';
  if (index >= 10) {
    id[1] = static_cast<char>('0' + index / 10);
    id[2] = static_cast<char>('0' + index % 10);
    id[3] = '\0';
  } else {
    id[1] = static_cast<char>('0' + index);
    id[2] = '\0';
  }
  rmdir_id(id);
}

void rmdir_ours() {
  rmdir_id(kRowId);
  for (int i = 0; i < g_clients; ++i) {
    rmdir_client(i);
  }
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
  rmdir_ours();
  if (is_dir(daemon_path(kRowId))) {
    throw std::runtime_error("row cgroup was not removed");
  }
  for (int i = 0; i < g_clients; ++i) {
    const auto id = "c" + std::to_string(i);
    if (is_dir(daemon_path(id))) {
      throw std::runtime_error("daemon cgroup was not removed: " + id);
    }
  }
  if (g_remove_parent && !parent_has_children()) {
    write_sysfs_best_effort(kParentControl, "-cpuset\n");
    if (::rmdir(kParent) != 0 && errno != ENOENT) {
      throw std::runtime_error("rmdir /sys/fs/cgroup/mercury failed");
    }
  }
  if (g_restore_root) {
    write_sysfs_best_effort(kRootControl, "-cpuset\n");
    if (has_controller(read_file(kRootControl), "cpuset")) {
      throw std::runtime_error("cpuset stayed enabled on the root cgroup");
    }
  }
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

void on_signal(int) {
  if (g_server > 0) {
    ::kill(-g_server, SIGKILL);
  }
  rmdir_ours();
  if (g_remove_parent) {
    write_sysfs_best_effort(kParentControl, "-cpuset\n");
    ::rmdir(kParent);
  }
  if (g_restore_root) {
    write_sysfs_best_effort(kRootControl, "-cpuset\n");
  }
  _exit(128);
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

std::string http_exchange(const std::string& request) {
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
  address.sin_port = htons(kPort);
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

std::string http_post(const std::string& body) {
  const std::string request = "POST /cpus HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(kPort) +
                              "\r\nContent-Type: application/json\r\nContent-Length: " +
                              std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
  return http_exchange(request);
}

std::string cpus_body(const std::string& id, const std::string& spec) {
  return "{\"daemon_id\":\"" + id + "\",\"cpus\":\"" + spec + "\"}";
}

const char* spec_for(int index) { return index % 2 == 0 ? "1-4" : "4"; }

void pin_to(int linux_cpu) {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(linux_cpu, &set);
  if (::sched_setaffinity(0, sizeof(set), &set) != 0) {
    throw std::runtime_error("sched_setaffinity failed for cpu " + std::to_string(linux_cpu));
  }
}

int allowed_cpu() {
  cpu_set_t set;
  CPU_ZERO(&set);
  if (::sched_getaffinity(0, sizeof(set), &set) != 0) {
    throw std::runtime_error("sched_getaffinity failed");
  }
  int found = -1;
  for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
    if (CPU_ISSET(cpu, &set) == 0) {
      continue;
    }
    if (found >= 0) {
      return -1;
    }
    found = cpu;
  }
  return found;
}

std::string stat_field(const std::string& stat, int field) {
  const auto close = stat.rfind(')');
  if (close == std::string::npos || field < 3) {
    return {};
  }
  std::istringstream fields(stat.substr(close + 1));
  std::string token;
  for (int at = 3; at <= field; ++at) {
    if (!(fields >> token)) {
      return {};
    }
  }
  return token;
}

int last_cpu() {
  const auto token = stat_field(read_file("/proc/thread-self/stat"), 39);
  if (token.empty()) {
    return -1;
  }
  try {
    return std::stoi(token);
  } catch (const std::exception&) {
    return -1;
  }
}

std::vector<int> online_cpus() {
  const auto text = trim(read_file("/sys/devices/system/cpu/online"));
  std::vector<int> cpus;
  std::size_t at = 0;
  while (at < text.size()) {
    const auto comma = text.find(',', at);
    const auto part = text.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
    const auto dash = part.find('-');
    try {
      if (dash == std::string::npos) {
        cpus.push_back(std::stoi(part));
      } else {
        const int first = std::stoi(part.substr(0, dash));
        const int last = std::stoi(part.substr(dash + 1));
        for (int cpu = first; cpu <= last; ++cpu) {
          cpus.push_back(cpu);
        }
      }
    } catch (const std::exception&) {
      return {};
    }
    if (comma == std::string::npos) {
      break;
    }
    at = comma + 1;
  }
  return cpus;
}

std::vector<int> client_cpus(int clients) {
  std::vector<int> chosen;
  for (const int cpu : online_cpus()) {
    if (cpu == kMercuryLinuxCpu) {
      continue;
    }
    chosen.push_back(cpu);
    if (static_cast<int>(chosen.size()) == clients) {
      return chosen;
    }
  }
  throw std::runtime_error("need " + std::to_string(clients) + " online cpus besides mercury");
}

const char* other_spec(const std::string& spec) { return spec == "4" ? "1-4" : "4"; }

void expect_cpuset(const std::string& id, const std::string& spec) {
  const auto dir = daemon_path(id);
  const auto configured = trim(read_file(dir + "/cpuset.cpus"));
  const auto effective = trim(read_file(dir + "/cpuset.cpus.effective"));
  if (configured != spec || effective != spec) {
    throw std::runtime_error(id + " cpuset.cpus " + configured + " effective " + effective + ", expected " +
                             spec);
  }
}

std::int64_t apply_cpus(const std::string& id, const std::string& spec) {
  const auto began = monotonic_ns();
  const auto response = http_post(cpus_body(id, spec));
  const auto elapsed = monotonic_ns() - began;
  if (response_status(response) != 200 || response_body(response) != R"({"ok":true})") {
    throw std::runtime_error(id + " cpus " + spec + " failed: " + trim(response_body(response)));
  }
  expect_cpuset(id, spec);
  return elapsed;
}

pid_t start_mercury(const std::string& mercury) {
  const auto port = std::to_string(kPort);
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
    const auto cpu = std::to_string(kMercuryLinuxCpu + 1);
    ::execl(mercury.c_str(), mercury.c_str(), port.c_str(), cpu.c_str(), nullptr);
    _exit(127);
  }
  ::setpgid(pid, pid);
  g_server = pid;
  return pid;
}

void wait_until_listening(pid_t mercury) {
  const std::string request = "GET /ping HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(kPort) +
                              "\r\nConnection: close\r\n\r\n";
  for (int attempt = 0; attempt < 50; ++attempt) {
    int status = 0;
    if (::waitpid(mercury, &status, WNOHANG) == mercury) {
      g_server = -1;
      throw std::runtime_error("mercury exited before listening");
    }
    try {
      if (response_body(http_exchange(request)).find("pong") != std::string::npos) {
        return;
      }
    } catch (const std::exception&) {
    }
    sleep_ms(50);
  }
  throw std::runtime_error("mercury did not answer on port " + std::to_string(kPort));
}

void parse_args(int argc, char** argv, int& row, int& clients) {
  row = kDefaultRow;
  clients = kDefaultClients;
  if (argc == 1) {
    return;
  }
  if (argc > 3) {
    throw std::runtime_error("usage: cpus_many_test [row [clients]]");
  }
  auto number = [](const char* text, int min, int max, const char* name) {
    try {
      std::size_t used = 0;
      const int value = std::stoi(text, &used);
      if (used != std::strlen(text) || value < min || value > max) {
        throw std::runtime_error(std::string("invalid ") + name);
      }
      return value;
    } catch (const std::runtime_error&) {
      throw;
    } catch (const std::exception&) {
      throw std::runtime_error(std::string("invalid ") + name);
    }
  };
  row = number(argv[1], 1, 100000, "row");
  if (argc == 3) {
    clients = number(argv[2], 1, kMaxClients, "clients");
  }
}

void run_row(int row) {
  std::int64_t total = 0;
  for (int i = 0; i < row; ++i) {
    const char* spec = spec_for(i);
    const auto elapsed = apply_cpus(kRowId, spec);
    std::cout << "row " << spec << " elapsed_ns " << elapsed << std::endl;
    total += elapsed;
  }
  expect_cpuset(kRowId, spec_for(row - 1));
  std::cout << "row samples " << row << " elapsed_ns " << total << " mean_ns " << total / row << std::endl;
}

void run_concurrent(int clients) {
  const auto warmup_began = monotonic_ns();
  for (int i = 0; i < clients; ++i) {
    const auto id = "c" + std::to_string(i);
    apply_cpus(id, other_spec(spec_for(i)));
  }
  std::cout << "warmup clients " << clients << " elapsed_ns " << monotonic_ns() - warmup_began << std::endl;

  const auto cpus = client_cpus(clients);
  std::vector<Sample> samples(static_cast<std::size_t>(clients));
  for (int i = 0; i < clients; ++i) {
    samples[static_cast<std::size_t>(i)].id = "c" + std::to_string(i);
    samples[static_cast<std::size_t>(i)].spec = spec_for(i);
    samples[static_cast<std::size_t>(i)].cpu = cpus[static_cast<std::size_t>(i)];
  }

  std::barrier gate(static_cast<std::ptrdiff_t>(clients));
  std::vector<std::thread> threads;
  threads.reserve(static_cast<std::size_t>(clients));
  for (int i = 0; i < clients; ++i) {
    threads.emplace_back([&, i] {
      auto& sample = samples[static_cast<std::size_t>(i)];
      try {
        pin_to(sample.cpu);
        if (allowed_cpu() != sample.cpu) {
          sample.error = sample.id + " affinity is not cpu " + std::to_string(sample.cpu);
        }
      } catch (const std::exception& error) {
        sample.error = error.what();
      }
      try {
        gate.arrive_and_wait();
        if (!sample.error.empty()) {
          return;
        }
        const auto began = monotonic_ns();
        const auto response = http_post(cpus_body(sample.id, sample.spec));
        sample.began = began;
        sample.elapsed_ns = monotonic_ns() - began;
        if (response_status(response) != 200 || response_body(response) != R"({"ok":true})") {
          sample.error = sample.id + " cpus " + sample.spec + " failed: " + trim(response_body(response));
        } else if (last_cpu() != sample.cpu) {
          sample.error = sample.id + " ran on cpu " + std::to_string(last_cpu()) + ", expected " +
                         std::to_string(sample.cpu);
        }
      } catch (const std::exception& error) {
        sample.error = error.what();
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }

  std::int64_t sum = 0;
  std::int64_t max_ns = 0;
  std::int64_t first = 0;
  std::int64_t last = 0;
  for (const auto& sample : samples) {
    if (!sample.error.empty()) {
      throw std::runtime_error(sample.error);
    }
    expect_cpuset(sample.id, sample.spec);
    std::cout << "concurrent " << sample.id << " cpu " << sample.cpu << " " << sample.spec << " elapsed_ns "
              << sample.elapsed_ns << std::endl;
    sum += sample.elapsed_ns;
    if (sample.elapsed_ns > max_ns) {
      max_ns = sample.elapsed_ns;
    }
    const auto ended = sample.began + sample.elapsed_ns;
    if (first == 0 || sample.began < first) {
      first = sample.began;
    }
    if (ended > last) {
      last = ended;
    }
  }
  std::cout << "concurrent clients " << clients << " wall_ns " << (last - first) << " mean_ns " << sum / clients
            << " max_ns " << max_ns << std::endl;
}

}  // namespace

int main(int argc, char** argv) {
  struct sigaction action {};
  action.sa_handler = on_signal;
  ::sigaction(SIGINT, &action, nullptr);
  ::sigaction(SIGTERM, &action, nullptr);

  try {
    if (::geteuid() != 0) {
      throw std::runtime_error("cpus_many_test must run as root to write /sys/fs/cgroup");
    }
    int row = 0;
    int clients = 0;
    parse_args(argc, argv, row, clients);
    g_clients = clients;

    const auto mercury = dirname_of(exe_dir()) + "/mercury";
    if (::access(mercury.c_str(), X_OK) != 0) {
      throw std::runtime_error("mercury binary not found at " + mercury);
    }

    g_remove_parent = is_dir(kParent) ? 0 : 1;
    g_restore_root = has_controller(read_file(kRootControl), "cpuset") ? 0 : 1;

    pin_to(kMercuryLinuxCpu);
    const pid_t mercury_pid = start_mercury(mercury);
    wait_until_listening(mercury_pid);

    const auto setup_ns = apply_cpus(kRowId, other_spec(spec_for(0)));
    std::cout << "setup " << other_spec(spec_for(0)) << " elapsed_ns " << setup_ns << std::endl;
    run_row(row);
    run_concurrent(clients);

    stop_server();
    cleanup_cgroups();
  } catch (const std::exception& error) {
    stop_server();
    try {
      cleanup_cgroups();
    } catch (const std::exception& cleanup_error) {
      std::cerr << cleanup_error.what() << std::endl;
    }
    std::cerr << error.what() << std::endl;
    return 1;
  }
}
