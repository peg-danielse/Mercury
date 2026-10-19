#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sched.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr int kMercuryPort = 18088;
constexpr int kHelloPort = 18092;
constexpr int kMercuryLinuxCpu = 0;
constexpr int kTestLinuxCpu = 58;
constexpr int kPoolCores = 16;
constexpr int kSleepClients = 4;
constexpr const char* kHelloId = "hello";
constexpr const char* kParent = "/sys/fs/cgroup/mercury";
constexpr const char* kRootControl = "/sys/fs/cgroup/cgroup.subtree_control";
constexpr const char* kParentControl = "/sys/fs/cgroup/mercury/cgroup.subtree_control";
constexpr const char* kRings = "/tmp/mercury-autoscale-rings";
constexpr std::int64_t kMeasureNs = 1000000000LL;
constexpr double kRateSlack = 0.25;

pid_t g_server = -1;
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
  Socket(Socket&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
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

int json_int(const std::string& body, const std::string& key) {
  const auto pattern = "\"" + key + "\":";
  const auto at = body.find(pattern);
  if (at == std::string::npos) {
    throw std::runtime_error("missing " + key + " in " + body);
  }
  return std::stoi(body.substr(at + pattern.size()));
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

void rmdir_hello() { ::rmdir((std::string(kParent) + "/" + kHelloId).c_str()); }

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
  rmdir_hello();
  if (g_remove_parent && !parent_has_children()) {
    write_sysfs_best_effort(kParentControl, "-cpuset\n");
    ::rmdir(kParent);
  }
  if (g_restore_root) {
    write_sysfs_best_effort(kRootControl, "-cpuset\n");
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
  rmdir_hello();
  if (g_remove_parent) {
    write_sysfs_best_effort(kParentControl, "-cpuset\n");
    ::rmdir(kParent);
  }
  if (g_restore_root) {
    write_sysfs_best_effort(kRootControl, "-cpuset\n");
  }
  _exit(128);
}

pid_t start_mercury(const std::string& mercury) {
  std::vector<std::string> args{mercury, std::to_string(kMercuryPort)};
  for (int cpu = 0; cpu <= kPoolCores; ++cpu) {
    args.push_back(std::to_string(cpu));
  }
  const pid_t pid = ::fork();
  if (pid < 0) {
    throw std::runtime_error("fork failed");
  }
  if (pid == 0) {
    if (::setpgid(0, 0) != 0) {
      _exit(127);
    }
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& arg : args) {
      argv.push_back(arg.data());
    }
    argv.push_back(nullptr);
    ::execv(argv[0], argv.data());
    _exit(127);
  }
  ::setpgid(pid, pid);
  g_server = pid;
  return pid;
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
  const std::string request = "POST " + target +
                              " HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: " +
                              std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
  return http_exchange(kMercuryPort, request);
}

pid_t start_hello(const std::string& path) {
  const std::string body = std::string("{\"daemon_id\":\"") + kHelloId + "\",\"exec_path\":\"" + json_escape(path) +
                           "\",\"cpu\":1,\"listen_port\":" + std::to_string(kHelloPort) + "}";
  const auto response = http_post("/start", body);
  const pid_t pid = response_pid(response);
  if (response_status(response) != 200 || pid <= 0) {
    throw std::runtime_error("start hello failed: " + trim(response_body(response)));
  }
  return pid;
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
  throw std::runtime_error("hello did not accept");
}

std::string cpu_spec(int cores) {
  if (cores <= 1) {
    return "1";
  }
  return "1-" + std::to_string(cores);
}

int count_cpuset(const std::string& text) {
  int count = 0;
  std::size_t i = 0;
  const auto trimmed = trim(text);
  if (trimmed.empty()) {
    return 0;
  }
  while (i < trimmed.size()) {
    if (count > 0) {
      if (trimmed[i] != ',') {
        break;
      }
      ++i;
    }
    int first = 0;
    if (i >= trimmed.size() || std::isdigit(static_cast<unsigned char>(trimmed[i])) == 0) {
      break;
    }
    while (i < trimmed.size() && std::isdigit(static_cast<unsigned char>(trimmed[i])) != 0) {
      first = first * 10 + (trimmed[i] - '0');
      ++i;
    }
    int last = first;
    if (i < trimmed.size() && trimmed[i] == '-') {
      ++i;
      last = 0;
      while (i < trimmed.size() && std::isdigit(static_cast<unsigned char>(trimmed[i])) != 0) {
        last = last * 10 + (trimmed[i] - '0');
        ++i;
      }
    }
    count += last - first + 1;
  }
  return count;
}

int current_cores() {
  return count_cpuset(read_file(std::string(kParent) + "/" + kHelloId + "/cpuset.cpus"));
}

// Each /cpu client burns one core. Saturate adds while the 64-sample mean
// occupancy is >= 0.85 with run-queue pressure >= 0.10, and removes only when
// the cgroup would still fit on one core less, so N busy clients settle at N.
int cores_for_clients(int clients) {
  return std::max(1, std::min(clients, kPoolCores));
}

void set_cpus(const std::string& spec) {
  const std::string body = std::string("{\"daemon_id\":\"") + kHelloId + "\",\"cpus\":\"" + spec + "\"}";
  const auto response = http_post("/cpus", body);
  if (response_status(response) != 200 || response_body(response) != R"({"ok":true})") {
    throw std::runtime_error("cpus " + spec + " failed: " + trim(response_body(response)));
  }
  for (int attempt = 0; attempt < 50; ++attempt) {
    if (trim(read_file(std::string(kParent) + "/" + kHelloId + "/cpuset.cpus")) == spec) {
      return;
    }
    sleep_ms(20);
  }
  throw std::runtime_error("cpuset did not become " + spec);
}

bool read_response(int fd) {
  std::string response;
  char buffer[1024];
  while (response.find("\r\n\r\n") == std::string::npos) {
    const auto n = ::recv(fd, buffer, sizeof(buffer), 0);
    if (n == 0) {
      return false;
    }
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return false;
      }
      return false;
    }
    response.append(buffer, static_cast<std::size_t>(n));
  }
  const auto header_end = response.find("\r\n\r\n");
  const auto key = response.find("Content-Length:");
  if (key == std::string::npos || key > header_end) {
    return false;
  }
  const auto length = static_cast<std::size_t>(std::stoul(response.substr(key + 15)));
  while (response.size() - (header_end + 4) < length) {
    const auto n = ::recv(fd, buffer, sizeof(buffer), 0);
    if (n <= 0) {
      return false;
    }
    response.append(buffer, static_cast<std::size_t>(n));
  }
  return response_status(response) == 200;
}

Socket connect_hello() {
  Socket sock{::socket(AF_INET, SOCK_STREAM, 0)};
  if (sock.get() < 0) {
    throw std::runtime_error("socket failed");
  }
  const int nodelay = 1;
  ::setsockopt(sock.get(), IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
  timeval timeout{};
  timeout.tv_sec = 3;
  timeout.tv_usec = 0;
  ::setsockopt(sock.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<std::uint16_t>(kHelloPort));
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(sock.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
    throw std::runtime_error("connect hello failed");
  }
  return sock;
}

void client_loop(const char* target, std::atomic<bool>& shared, std::atomic<bool>& own,
                 std::atomic<std::uint64_t>& done) {
  const std::string request = std::string("GET ") + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
  while (shared.load(std::memory_order_relaxed) && own.load(std::memory_order_relaxed)) {
    try {
      Socket sock = connect_hello();
      while (shared.load(std::memory_order_relaxed) && own.load(std::memory_order_relaxed)) {
        send_all(sock.get(), request);
        if (!read_response(sock.get())) {
          break;
        }
        done.fetch_add(1, std::memory_order_relaxed);
      }
    } catch (const std::exception&) {
      if (!shared.load(std::memory_order_relaxed) || !own.load(std::memory_order_relaxed)) {
        return;
      }
      sleep_ms(20);
    }
  }
}

struct Slot {
  std::atomic<bool> run{true};
  std::thread thread;
};

struct Load {
  std::atomic<bool> run{false};
  std::atomic<std::uint64_t> cpu_done{0};
  std::atomic<std::uint64_t> sleep_done{0};
  std::vector<std::unique_ptr<Slot>> cpu;
  std::vector<std::unique_ptr<Slot>> sleepers;

  void add_workers(std::vector<std::unique_ptr<Slot>>& slots, const char* target, int count,
                   std::atomic<std::uint64_t>& done) {
    run.store(true, std::memory_order_relaxed);
    for (int i = 0; i < count; ++i) {
      auto slot = std::make_unique<Slot>();
      Slot* raw = slot.get();
      raw->thread = std::thread(client_loop, target, std::ref(run), std::ref(raw->run), std::ref(done));
      slots.push_back(std::move(slot));
    }
  }

  void add_cpu(int count) { add_workers(cpu, "/cpu", count, cpu_done); }

  void add_sleep(int count) { add_workers(sleepers, "/sleep", count, sleep_done); }

  void drop_cpu() {
    if (cpu.empty()) {
      return;
    }
    cpu.back()->run.store(false, std::memory_order_relaxed);
    if (cpu.back()->thread.joinable()) {
      cpu.back()->thread.join();
    }
    cpu.pop_back();
  }

  void stop() {
    run.store(false, std::memory_order_relaxed);
    auto join_all = [](std::vector<std::unique_ptr<Slot>>& slots) {
      for (auto& slot : slots) {
        if (slot && slot->thread.joinable()) {
          slot->thread.join();
        }
      }
      slots.clear();
    };
    join_all(cpu);
    join_all(sleepers);
  }

  std::uint64_t measure_cpu() {
    const auto before = cpu_done.load(std::memory_order_relaxed);
    const auto began = monotonic_ns();
    sleep_ms(static_cast<int>(kMeasureNs / 1000000LL));
    const auto elapsed = monotonic_ns() - began;
    const auto done = cpu_done.load(std::memory_order_relaxed) - before;
    if (elapsed <= 0) {
      return 0;
    }
    return done * 1000000000ULL / static_cast<std::uint64_t>(elapsed);
  }
};

}  // namespace

int main() {
  struct sigaction action {};
  action.sa_handler = on_signal;
  ::sigaction(SIGINT, &action, nullptr);
  ::sigaction(SIGTERM, &action, nullptr);

  Load load;
  try {
    if (::geteuid() != 0) {
      throw std::runtime_error("autoscale_test must run as root to write /sys/fs/cgroup");
    }
    cpu_set_t self;
    CPU_ZERO(&self);
    CPU_SET(kTestLinuxCpu, &self);
    if (::sched_setaffinity(0, sizeof(self), &self) != 0) {
      throw std::runtime_error("failed to pin autoscale_test");
    }
    const auto bindir = dirname_of(exe_dir());
    const auto testdir = exe_dir();
    const auto mercury = bindir + "/mercury";
    const auto hello = testdir + "/hello_server";
    if (::access(mercury.c_str(), X_OK) != 0 || ::access(hello.c_str(), X_OK) != 0) {
      throw std::runtime_error("missing mercury or hello_server");
    }

    g_remove_parent = is_dir(kParent) ? 0 : 1;
    g_restore_root = has_controller(read_file(kRootControl), "cpuset") ? 0 : 1;
    rmdir_hello();
    std::filesystem::remove_all(kRings);
    ::setenv("MERCURY_RINGS", kRings, 1);

    start_mercury(mercury);
    wait_until_listening();
    const pid_t hello_pid = start_hello(hello);
    wait_until_port(kHelloPort);
    if (current_cores() != 1) {
      throw std::runtime_error("hello started on " + std::to_string(current_cores()) + " cores");
    }
    std::cout << "hello pid " << hello_pid << " cores 1" << std::endl;

    const auto rejected = http_post("/scale", std::string("{\"daemon_id\":\"") + kHelloId + "\",\"policy\":\"nope\"}");
    if (response_status(rejected) != 400) {
      throw std::runtime_error("unknown policy was accepted");
    }

    load.add_sleep(kSleepClients);
    load.add_cpu(kPoolCores);
    sleep_ms(300);
    if (load.sleep_done.load() == 0) {
      throw std::runtime_error("sleep route produced no replies");
    }

    // arm_daemon attaches saturate by default; detach it so the fixed steps hold.
    const auto detached = http_post("/scale", std::string("{\"daemon_id\":\"") + kHelloId + "\",\"policy\":\"none\"}");
    if (response_status(detached) != 200) {
      throw std::runtime_error("scale detach failed: " + trim(response_body(detached)));
    }

    std::uint64_t fixed[5]{};
    const int steps[] = {1, 2, 4, 8, 16};
    std::cout << "fixed" << std::endl;
    for (int i = 0; i < 5; ++i) {
      set_cpus(cpu_spec(steps[i]));
      sleep_ms(1500);
      fixed[i] = load.measure_cpu();
      std::cout << "cores " << steps[i] << " rate_per_s " << fixed[i] << std::endl;
    }
    if (fixed[4] < fixed[0] * 8) {
      throw std::runtime_error("16 cores rate " + std::to_string(fixed[4]) + " is not well above 1 core rate " +
                               std::to_string(fixed[0]));
    }

    set_cpus("1");
    const auto attached =
        http_post("/scale", std::string("{\"daemon_id\":\"") + kHelloId + "\",\"policy\":\"saturate\"" +
                                (std::getenv("AUTOSCALE_PLACEMENT") != nullptr
                                     ? std::string(",\"placement\":\"") + std::getenv("AUTOSCALE_PLACEMENT") + "\""
                                     : std::string()) +
                                "}");
    if (response_status(attached) != 200) {
      throw std::runtime_error("scale attach failed: " + trim(response_body(attached)));
    }
    const auto body = response_body(attached);
    const int min_cores = json_int(body, "min");
    const int max_cores = json_int(body, "max");
    std::cout << "scale min " << min_cores << " max " << max_cores << std::endl;
    if (min_cores != 1 || max_cores != 16) {
      throw std::runtime_error("default core range is " + std::to_string(min_cores) + "-" + std::to_string(max_cores));
    }

    std::cout << "autoscale" << std::endl;
    int last_cores = current_cores();
    std::cout << "autoscale cores " << last_cores << std::endl;
    const auto grow_began = monotonic_ns();
    while (monotonic_ns() - grow_began < 60000000000LL) {  // one core per 64-sample window plus cooldown
      const int cores = current_cores();
      if (cores != last_cores) {
        std::cout << "autoscale cores " << cores << std::endl;
        last_cores = cores;
      }
      if (cores == kPoolCores) {
        break;
      }
      sleep_ms(100);
    }
    if (current_cores() != kPoolCores) {
      throw std::runtime_error("autoscale stopped at " + std::to_string(current_cores()) + " cores");
    }
    sleep_ms(400);
    const auto scaled = load.measure_cpu();
    std::cout << "autoscale cores " << current_cores() << " rate_per_s " << scaled << std::endl;
    const double low = static_cast<double>(fixed[4]) * (1.0 - kRateSlack);
    const double high = static_cast<double>(fixed[4]) * (1.0 + kRateSlack);
    if (static_cast<double>(scaled) < low || static_cast<double>(scaled) > high) {
      throw std::runtime_error("autoscale rate " + std::to_string(scaled) + " disagrees with 16 core rate " +
                               std::to_string(fixed[4]));
    }

    load.stop();
    std::cout << "drain" << std::endl;
    int drained = current_cores();
    std::cout << "drain cores " << drained << std::endl;
    const auto drain_began = monotonic_ns();
    while (monotonic_ns() - drain_began < 25000000000LL) {
      const int cores = current_cores();
      if (cores != drained) {
        std::cout << "drain cores " << cores << std::endl;
        drained = cores;
      }
      if (cores == 1) {
        break;
      }
      sleep_ms(100);
    }
    sleep_ms(500);
    if (current_cores() != 1) {
      throw std::runtime_error("cpuset left autoscale at " + std::to_string(current_cores()) + " after the load stopped");
    }

    std::cout << "ramp" << std::endl;
    load.add_sleep(kSleepClients);
    int printed = 0;
    for (int clients = 1; clients <= kPoolCores; ++clients) {
      load.add_cpu(1);
      const auto began = monotonic_ns();
      int cores = current_cores();
      while (monotonic_ns() - began < 8000000000LL) {
        cores = current_cores();
        if (cores != printed) {
          std::cout << "ramp clients " << clients << " cores " << cores << std::endl;
          printed = cores;
        }
        if (cores == cores_for_clients(clients) && monotonic_ns() - began >= 1500000000LL) {
          break;
        }
        sleep_ms(100);
      }
      if (cores != cores_for_clients(clients)) {
        throw std::runtime_error("ramp clients " + std::to_string(clients) + " settled at " + std::to_string(cores) +
                                 " cores");
      }
    }

    std::cout << "down" << std::endl;
    printed = current_cores();
    for (int clients = kPoolCores - 1; clients >= 1; --clients) {
      load.drop_cpu();
      const auto began = monotonic_ns();
      int cores = current_cores();
      while (monotonic_ns() - began < 8000000000LL) {
        cores = current_cores();
        if (cores != printed) {
          std::cout << "down clients " << clients << " cores " << cores << std::endl;
          printed = cores;
        }
        if (cores == cores_for_clients(clients) && monotonic_ns() - began >= 1500000000LL) {
          break;
        }
        sleep_ms(100);
      }
      if (cores != cores_for_clients(clients)) {
        throw std::runtime_error("down clients " + std::to_string(clients) + " settled at " + std::to_string(cores) +
                                 " cores");
      }
    }

    load.stop();
    http_post("/kill", std::string("{\"daemon_id\":\"") + kHelloId + "\"}");
    stop_server();
    cleanup_cgroups();
    std::filesystem::remove_all(kRings);
    std::cout << "autoscale ok" << std::endl;
  } catch (const std::exception& error) {
    load.stop();
    stop_server();
    cleanup_cgroups();
    std::cerr << error.what() << std::endl;
    return 1;
  }
}
