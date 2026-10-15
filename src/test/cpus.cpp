#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
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

namespace {

constexpr int kPort = 18082;
constexpr const char* kParent = "/sys/fs/cgroup/mercury";
constexpr const char* kDaemon = "/sys/fs/cgroup/mercury/cpus_test";
constexpr const char* kCpus = "/sys/fs/cgroup/mercury/cpus_test/cpuset.cpus";
constexpr const char* kEffective = "/sys/fs/cgroup/mercury/cpus_test/cpuset.cpus.effective";
constexpr const char* kRootControl = "/sys/fs/cgroup/cgroup.subtree_control";
constexpr const char* kParentControl = "/sys/fs/cgroup/mercury/cgroup.subtree_control";

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
    const auto path = std::string(kParent) + "/" + entry->d_name;
    if (is_dir(path)) {
      found = true;
      break;
    }
  }
  ::closedir(dir);
  return found;
}

void cleanup_cgroups() {
  ::rmdir(kDaemon);
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
  if (is_dir(kDaemon)) {
    throw std::runtime_error("daemon cgroup was not removed");
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
  ::rmdir(kDaemon);
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

std::string http_post(const std::string& target, const std::string& body) {
  const std::string request = "POST " + target + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(kPort) +
                              "\r\nContent-Type: application/json\r\nContent-Length: " +
                              std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
  return http_exchange(request);
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
    ::execl(mercury.c_str(), mercury.c_str(), port.c_str(), "0", nullptr);
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

void expect_cpuset(const std::string& spec) {
  const auto configured = trim(read_file(kCpus));
  const auto effective = trim(read_file(kEffective));
  if (configured != spec || effective != spec) {
    throw std::runtime_error("cpuset.cpus " + configured + " effective " + effective + ", expected " + spec);
  }
}

std::int64_t apply_cpus(const std::string& spec) {
  const std::string body = std::string("{\"daemon_id\":\"cpus_test\",\"cpus\":\"") + spec + "\"}";
  const auto began = monotonic_ns();
  const auto response = http_post("/cpus", body);
  const auto elapsed = monotonic_ns() - began;
  if (response_status(response) != 200 || response_body(response) != R"({"ok":true})") {
    throw std::runtime_error("cpus " + spec + " failed: " + trim(response_body(response)));
  }
  expect_cpuset(spec);
  return elapsed;
}

void expect_rejected(const std::string& body, int status) {
  const auto response = http_post("/cpus", body);
  if (response_status(response) != status || response_body(response) != R"({"ok":false})") {
    throw std::runtime_error("expected HTTP " + std::to_string(status) + ", got " +
                             trim(response.substr(0, response.find("\r\n"))) + " " +
                             trim(response_body(response)));
  }
}

int repeats_from(int argc, char** argv) {
  if (argc == 1) {
    return 1;
  }
  if (argc != 2) {
    throw std::runtime_error("usage: cpus_test [repeats]");
  }
  try {
    std::size_t used = 0;
    const int repeats = std::stoi(argv[1], &used);
    if (used != std::strlen(argv[1]) || repeats < 1) {
      throw std::runtime_error("invalid repeats");
    }
    return repeats;
  } catch (const std::runtime_error&) {
    throw;
  } catch (const std::exception&) {
    throw std::runtime_error("usage: cpus_test [repeats]");
  }
}

}  // namespace

int main(int argc, char** argv) {
  struct sigaction action {};
  action.sa_handler = on_signal;
  ::sigaction(SIGINT, &action, nullptr);
  ::sigaction(SIGTERM, &action, nullptr);

  try {
    if (::geteuid() != 0) {
      throw std::runtime_error("cpus_test must run as root to write /sys/fs/cgroup");
    }
    const int repeats = repeats_from(argc, argv);
    const auto mercury = dirname_of(exe_dir()) + "/mercury";
    if (::access(mercury.c_str(), X_OK) != 0) {
      throw std::runtime_error("mercury binary not found at " + mercury);
    }

    g_remove_parent = is_dir(kParent) ? 0 : 1;
    g_restore_root = has_controller(read_file(kRootControl), "cpuset") ? 0 : 1;

    const pid_t mercury_pid = start_mercury(mercury);
    wait_until_listening(mercury_pid);

    const auto setup_ns = apply_cpus("1-4");
    std::cout << "setup 1-4 elapsed_ns " << setup_ns << std::endl;

    std::int64_t total = 0;
    int samples = 0;
    for (int i = 0; i < repeats; ++i) {
      const auto range_ns = apply_cpus("1-4");
      std::cout << "cpus 1-4 elapsed_ns " << range_ns << std::endl;
      const auto single_ns = apply_cpus("4");
      std::cout << "cpus 4 elapsed_ns " << single_ns << std::endl;
      total += range_ns + single_ns;
      samples += 2;
    }
    expect_rejected(R"({"daemon_id":"cpus_test","cpus":"nope"})", 400);
    expect_rejected(R"({"daemon_id":"cpus_test","cpus":"0"})", 400);
    expect_rejected(R"({"daemon_id":"cpus_test","cpus":"0-4"})", 400);
    expect_cpuset("4");
    expect_rejected(R"({"daemon_id":"../cpus_test","cpus":"4"})", 400);
    expect_cpuset("4");

    std::cout << "samples " << samples << " elapsed_ns " << total << " mean_ns " << total / samples << std::endl;
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
