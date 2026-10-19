#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr int kMercuryPort = 18133;
constexpr int kServerPort = 18134;
constexpr int kClients = 200;
constexpr int kMaxCores = 16;
// partial_server has one worker per live connection up to this pool. Further
// connections wait, so they do not add CPU.
constexpr int kServedClients = 48;
// partial_server spends 2 ms on a request (33% of that is CPU, the rest asleep).
// A reply later than two of those slices missed the server's deadline.
constexpr std::int64_t kServerSliceNs = 2000000;
constexpr std::int64_t kDeadlineNs = kServerSliceNs * 2;
constexpr const char* kDaemonId = "nearest";
constexpr const char* kCpuset = "/sys/fs/cgroup/mercury/nearest/cpuset.cpus";

pid_t g_server = -1;
std::atomic<std::uint64_t> g_completed{0};
std::atomic<std::uint64_t> g_late{0};

std::int64_t monotonic_ns() {
  timespec now{};
  if (::clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    return 0;
  }
  return static_cast<std::int64_t>(now.tv_sec) * 1000000000LL + now.tv_nsec;
}

std::string deadline_text() {
  const auto done = g_completed.load(std::memory_order_relaxed);
  const auto late = g_late.load(std::memory_order_relaxed);
  std::ostringstream out;
  out << "deadline_misses " << late << "/" << done;
  if (done > 0) {
    const double rate = 100.0 * static_cast<double>(late) / static_cast<double>(done);
    out << std::fixed << std::setprecision(2) << " (" << rate << "%)";
  }
  out << " deadline_ns " << kDeadlineNs;
  return out.str();
}

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
  FILE* file = ::fopen(path.c_str(), "r");
  if (file == nullptr) {
    return {};
  }
  std::string text;
  char buffer[256];
  while (std::fgets(buffer, sizeof(buffer), file) != nullptr) {
    text += buffer;
  }
  ::fclose(file);
  return text;
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
    throw std::runtime_error("readlink failed");
  }
  buf[n] = '\0';
  return dirname_of(buf);
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

void send_all(int fd, std::string_view message) {
  while (!message.empty()) {
    const auto n = ::send(fd, message.data(), message.size(), MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw std::runtime_error("send failed");
    }
    message.remove_prefix(static_cast<std::size_t>(n));
  }
}

std::string recv_all(int fd) {
  std::string response;
  char buffer[4096];
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

std::string http_get(const std::string& target) {
  const std::string request = "GET " + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
  return http_exchange(kMercuryPort, request);
}

std::string http_post(const std::string& target, const std::string& body) {
  const std::string request = "POST " + target +
                              " HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: " +
                              std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
  return http_exchange(kMercuryPort, request);
}

std::vector<int> parse_cpu_list(const std::string& text) {
  std::vector<int> cpus;
  std::size_t i = 0;
  const auto trimmed = trim(text);
  while (i < trimmed.size()) {
    if (!cpus.empty()) {
      if (trimmed[i] != ',') {
        break;
      }
      ++i;
    }
    if (i >= trimmed.size() || std::isdigit(static_cast<unsigned char>(trimmed[i])) == 0) {
      break;
    }
    int first = 0;
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
    for (int cpu = first; cpu <= last; ++cpu) {
      cpus.push_back(cpu);
    }
  }
  std::sort(cpus.begin(), cpus.end());
  cpus.erase(std::unique(cpus.begin(), cpus.end()), cpus.end());
  return cpus;
}

std::vector<int> online_cpus() { return parse_cpu_list(read_file("/sys/devices/system/cpu/online")); }

std::vector<int> cpuset_cpus() { return parse_cpu_list(read_file(kCpuset)); }

bool contains(const std::vector<int>& cpus, int cpu) { return std::binary_search(cpus.begin(), cpus.end(), cpu); }

std::string cpu_text(const std::vector<int>& cpus) {
  std::ostringstream out;
  for (std::size_t i = 0; i < cpus.size(); ++i) {
    if (i != 0) {
      out << ',';
    }
    out << cpus[i];
  }
  return out.str();
}

struct Group {
  std::string level;
  std::string name;
  std::string parent;
  std::vector<int> cpus;
  double load = 0;
  int lowest = 0;
};

struct MapView {
  std::vector<int> allocatable;
  std::vector<Group> groups;
};

std::string json_string(const std::string& text, const std::string& key) {
  const auto pattern = "\"" + key + "\":\"";
  const auto at = text.find(pattern);
  if (at == std::string::npos) {
    return {};
  }
  const auto start = at + pattern.size();
  const auto end = text.find('"', start);
  if (end == std::string::npos) {
    return {};
  }
  return text.substr(start, end - start);
}

std::vector<int> json_ints(const std::string& text, const std::string& key) {
  const auto pattern = "\"" + key + "\":[";
  const auto at = text.find(pattern);
  if (at == std::string::npos) {
    return {};
  }
  const auto start = at + pattern.size();
  const auto end = text.find(']', start);
  if (end == std::string::npos) {
    return {};
  }
  return parse_cpu_list(text.substr(start, end - start));
}

double json_number(const std::string& text, const std::string& key) {
  const auto pattern = "\"" + key + "\":";
  const auto at = text.find(pattern);
  if (at == std::string::npos) {
    return 0;
  }
  try {
    return std::stod(text.substr(at + pattern.size()));
  } catch (const std::exception&) {
    return 0;
  }
}

MapView parse_map(const std::string& body) {
  MapView map;
  const auto alloc = body.find("\"allocatable\":[");
  if (alloc != std::string::npos) {
    const auto start = body.find('[', alloc);
    const auto end = body.find(']', start);
    map.allocatable = parse_cpu_list(body.substr(start + 1, end - start - 1));
  }
  std::size_t at = 0;
  const std::string marker = "{\"level\":\"";
  while ((at = body.find(marker, at)) != std::string::npos) {
    const auto next = body.find(marker, at + marker.size());
    const auto slice = body.substr(at, next == std::string::npos ? std::string::npos : next - at);
    Group group;
    group.level = json_string(slice, "level");
    group.name = json_string(slice, "name");
    group.parent = json_string(slice, "parent");
    group.cpus = json_ints(slice, "linux_cpus");
    group.load = json_number(slice, "load");
    group.lowest = group.cpus.empty() ? CPU_SETSIZE : group.cpus.front();
    map.groups.push_back(std::move(group));
    if (next == std::string::npos) {
      break;
    }
    at = next;
  }
  return map;
}

int held_count(const Group& group, const std::vector<int>& held) {
  int count = 0;
  for (const int cpu : group.cpus) {
    if (contains(held, cpu)) {
      ++count;
    }
  }
  return count;
}

bool more_balanced(const Group& candidate, const Group& best, const std::vector<int>& held) {
  const int candidate_held = held_count(candidate, held);
  const int best_held = held_count(best, held);
  if (candidate_held != best_held) {
    return candidate_held < best_held;
  }
  if (candidate.load != best.load) {
    return candidate.load < best.load;
  }
  if (candidate.lowest != best.lowest) {
    return candidate.lowest < best.lowest;
  }
  return candidate.name < best.name;
}

int lowest_free(const Group& group, const std::vector<int>& allocatable, const std::vector<int>& held) {
  for (const int cpu : group.cpus) {
    if (contains(allocatable, cpu) && !contains(held, cpu)) {
      return cpu;
    }
  }
  return -1;
}

const Group* balanced_sibling(const MapView& map, const std::string& parent, const std::vector<int>& held) {
  const Group* best = nullptr;
  for (const auto& group : map.groups) {
    if (group.parent != parent || lowest_free(group, map.allocatable, held) < 0) {
      continue;
    }
    if (best == nullptr || more_balanced(group, *best, held)) {
      best = &group;
    }
  }
  return best;
}

int balanced_cpu(const MapView& map, const std::vector<int>& held) {
  std::string parent;
  const Group* chosen = nullptr;
  for (int depth = 0; depth < 4; ++depth) {
    const auto* best = balanced_sibling(map, parent, held);
    if (best == nullptr) {
      break;
    }
    chosen = best;
    parent = best->name;
  }
  if (chosen == nullptr) {
    return -1;
  }
  return lowest_free(*chosen, map.allocatable, held);
}

const Group* group_holding(const MapView& map, const char* level, int cpu) {
  for (const auto& group : map.groups) {
    if (group.level == level && contains(group.cpus, cpu)) {
      return &group;
    }
  }
  return nullptr;
}

[[maybe_unused]] std::string check_additions(const MapView& before, std::vector<int> held, const std::vector<int>& now) {
  std::vector<int> added;
  for (const int cpu : now) {
    if (!contains(held, cpu)) {
      added.push_back(cpu);
    }
  }
  std::sort(held.begin(), held.end());
  std::string problems;
  while (!added.empty()) {
    const int offer = balanced_cpu(before, held);
    int cpu = added.front();
    if (offer >= 0 && contains(added, offer)) {
      cpu = offer;
    }
    std::string parent;
    std::ostringstream line;
    line << "added cpu " << cpu;
    for (int depth = 0; depth < 4; ++depth) {
      const auto* best = balanced_sibling(before, parent, held);
      const auto* actual = group_holding(before, best == nullptr ? "" : best->level.c_str(), cpu);
      if (actual == nullptr || actual->parent != parent) {
        actual = nullptr;
        for (const auto& group : before.groups) {
          if (group.parent == parent && contains(group.cpus, cpu)) {
            actual = &group;
            break;
          }
        }
      }
      if (actual == nullptr && best == nullptr) {
        break;
      }
      const auto best_name = best == nullptr ? std::string("none") : best->name;
      const auto actual_name = actual == nullptr ? std::string("none") : actual->name;
      const auto level = actual != nullptr ? actual->level : (best != nullptr ? best->level : std::string("group"));
      const int best_held = best == nullptr ? -1 : held_count(*best, held);
      const int actual_held = actual == nullptr ? -1 : held_count(*actual, held);
      line << ' ' << level << ' ' << actual_name << " held=" << actual_held;
      // Load decides among groups that already hold the same number of CPUs, and the
      // map is older than that decision. A group that is ahead on count is the miss.
      if (best == nullptr || actual == nullptr || actual_held > best_held) {
        line << " balanced=" << best_name << " held=" << best_held;
        problems += level + " balanced " + best_name + " held " + std::to_string(best_held) + " but cpu " +
                    std::to_string(cpu) + " is in " + actual_name + " held " + std::to_string(actual_held) + "\n";
      }
      if (actual == nullptr) {
        break;
      }
      parent = actual->name;
      if (actual->level == "core") {
        break;
      }
    }
    std::cout << line.str() << std::endl;
    added.erase(std::find(added.begin(), added.end(), cpu));
    held.push_back(cpu);
    std::sort(held.begin(), held.end());
  }
  return problems;
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
      ::rmdir("/sys/fs/cgroup/mercury/nearest");
      return;
    }
    sleep_ms(50);
  }
  ::kill(-server, SIGKILL);
  while (::waitpid(server, nullptr, 0) < 0 && errno == EINTR) {
  }
  ::rmdir("/sys/fs/cgroup/mercury/nearest");
}

void on_signal(int) {
  if (g_server > 0) {
    ::kill(-g_server, SIGKILL);
  }
  ::rmdir("/sys/fs/cgroup/mercury/nearest");
  _exit(128);
}

pid_t start_mercury(const std::string& mercury, const std::vector<int>& cpus) {
  std::vector<std::string> args{mercury, std::to_string(kMercuryPort)};
  for (const int cpu : cpus) {
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
    const int log = ::open("/tmp/mercury-least.log", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (log >= 0) {
      ::dup2(log, STDOUT_FILENO);
      ::dup2(log, STDERR_FILENO);
      ::close(log);
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

struct Client {
  std::shared_ptr<std::atomic<bool>> run = std::make_shared<std::atomic<bool>>(true);
  std::thread thread;
};

void client_loop(std::shared_ptr<std::atomic<bool>> run) {
  const std::string request = "GET /burn HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
  while (run->load(std::memory_order_relaxed)) {
    try {
      Socket sock{::socket(AF_INET, SOCK_STREAM, 0)};
      if (sock.get() < 0) {
        throw std::runtime_error("socket failed");
      }
      const int nodelay = 1;
      ::setsockopt(sock.get(), IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
      timeval recv_timeout{};
      recv_timeout.tv_usec = 200000;
      ::setsockopt(sock.get(), SOL_SOCKET, SO_RCVTIMEO, &recv_timeout, sizeof(recv_timeout));
      sockaddr_in address{};
      address.sin_family = AF_INET;
      address.sin_port = htons(static_cast<std::uint16_t>(kServerPort));
      address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      if (::connect(sock.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        throw std::runtime_error("connect failed");
      }
      while (run->load(std::memory_order_relaxed)) {
        const auto began = monotonic_ns();
        send_all(sock.get(), request);
        char buffer[256];
        std::string response;
        while (run->load(std::memory_order_relaxed) && response.find("ok\n") == std::string::npos) {
          const auto n = ::recv(sock.get(), buffer, sizeof(buffer), 0);
          if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
          }
          if (n <= 0) {
            throw std::runtime_error("short read");
          }
          response.append(buffer, static_cast<std::size_t>(n));
        }
        if (!run->load(std::memory_order_relaxed) || response.find("ok\n") == std::string::npos) {
          return;
        }
        const auto service_ns = monotonic_ns() - began;
        g_completed.fetch_add(1, std::memory_order_relaxed);
        if (service_ns > kDeadlineNs) {
          g_late.fetch_add(1, std::memory_order_relaxed);
        }
      }
    } catch (const std::exception&) {
      if (!run->load(std::memory_order_relaxed)) {
        return;
      }
      sleep_ms(20);
    }
  }
}

void drop_clients(std::vector<Client>& clients, int count) {
  count = std::min(count, static_cast<int>(clients.size()));
  if (count <= 0) {
    return;
  }
  const auto first = clients.size() - static_cast<std::size_t>(count);
  for (std::size_t i = first; i < clients.size(); ++i) {
    clients[i].run->store(false, std::memory_order_relaxed);
  }
  sleep_ms(250);
  for (int i = 0; i < count; ++i) {
    if (clients.back().thread.joinable()) {
      clients.back().thread.join();
    }
    clients.pop_back();
  }
}

void stop_clients(std::vector<Client>& clients) {
  for (auto& client : clients) {
    client.run->store(false, std::memory_order_relaxed);
  }
  for (auto& client : clients) {
    if (client.thread.joinable()) {
      client.thread.join();
    }
  }
}

// saturate drops the highest Linux CPU. A batch of removals leaves the lowest CPUs.
// nearest_first tier of cpu against held: 0 core, 1 l3, 2 node, 3 socket, 4 none.
int tier_of(const MapView& map, const std::vector<int>& held, int cpu) {
  const char* levels[] = {"core", "l3", "node", "socket"};
  for (int t = 0; t < 4; ++t) {
    for (const auto& group : map.groups) {
      if (group.level != levels[t] || !contains(group.cpus, cpu)) {
        continue;
      }
      for (const int h : held) {
        if (contains(group.cpus, h)) {
          return t;
        }
      }
    }
  }
  return 4;
}

// Every added CPU must be in the nearest tier any free allocatable CPU offered.
std::string check_nearest_additions(const MapView& before, std::vector<int> held, const std::vector<int>& now) {
  std::sort(held.begin(), held.end());
  std::string problems;
  for (const int cpu : now) {
    if (contains(held, cpu)) {
      continue;
    }
    int best = 4;
    for (const int free_cpu : before.allocatable) {
      if (!contains(held, free_cpu)) {
        best = std::min(best, tier_of(before, held, free_cpu));
      }
    }
    const int got = tier_of(before, held, cpu);
    std::cout << "added cpu " << cpu << " tier " << got << " best " << best << std::endl;
    if (got > best) {
      problems += "added cpu " + std::to_string(cpu) + " at tier " + std::to_string(got) + ", nearest tier was " +
                  std::to_string(best) + "\n";
    }
    held.push_back(cpu);
    std::sort(held.begin(), held.end());
  }
  return problems;
}

std::string check_removals(const std::vector<int>& before, const std::vector<int>& now) {
  std::string problems;
  for (const int cpu : now) {
    if (!contains(before, cpu)) {
      problems += "down added cpu " + std::to_string(cpu) + "\n";
    }
  }
  std::vector<int> expect = before;
  if (now.size() < before.size()) {
    expect.resize(now.size());
  }
  auto got = now;
  std::sort(got.begin(), got.end());
  if (got != expect) {
    problems += "down cpus " + cpu_text(got) + " want " + cpu_text(expect) + "\n";
  }
  return problems;
}

// Each live connection burns 33% of a core. Saturate grows while that usage is
// at least 90% of the cpuset and sheds a core only when the smaller cpuset
// would stay under 90%.
int cores_for_clients(int clients) {
  // Each served connection burns a whole core (PARTIAL_BURN_PCT=100): N clients -> N cores.
  const int served = std::min(clients, kServedClients);
  return std::max(1, std::min(served, kMaxCores));
}

}  // namespace

int main() {
  struct sigaction action {};
  action.sa_handler = on_signal;
  ::sigaction(SIGINT, &action, nullptr);
  ::sigaction(SIGTERM, &action, nullptr);

  std::vector<Client> clients;
  try {
    if (::geteuid() != 0) {
      throw std::runtime_error("autoscale_nearest_first must run as root to write /sys/fs/cgroup");
    }
    auto online = online_cpus();
    // Keep a few CPUs for the client threads. The rest are Mercury's pool.
    constexpr int kClientCpus = 4;
    if (online.size() < static_cast<std::size_t>(kMaxCores + 1 + kClientCpus)) {
      throw std::runtime_error("need more online cpus");
    }
    cpu_set_t self;
    CPU_ZERO(&self);
    for (int i = 0; i < kClientCpus; ++i) {
      CPU_SET(online[online.size() - 1 - static_cast<std::size_t>(i)], &self);
    }
    online.resize(online.size() - static_cast<std::size_t>(kClientCpus));
    if (::sched_setaffinity(0, sizeof(self), &self) != 0) {
      throw std::runtime_error("failed to pin autoscale_nearest_first");
    }

    const auto bindir = dirname_of(exe_dir());
    const auto mercury = bindir + "/mercury";
    const auto partial = exe_dir() + "/partial_server";
    if (::access(mercury.c_str(), X_OK) != 0 || ::access(partial.c_str(), X_OK) != 0) {
      throw std::runtime_error("missing mercury or partial_server");
    }

    ::setenv("MERCURY_RINGS", "/tmp/mercury-nearest-rings", 1);
    ::rmdir("/sys/fs/cgroup/mercury/nearest");
    start_mercury(mercury, online);
    wait_until_listening();
    MapView map;
    for (int attempt = 0; attempt < 50; ++attempt) {
      const auto response = http_get("/map");
      if (response_status(response) == 200) {
        map = parse_map(response_body(response));
        if (!map.groups.empty() && map.groups.front().load >= 0) {
          break;
        }
      }
      sleep_ms(50);
    }
    if (map.allocatable.size() < static_cast<std::size_t>(kMaxCores)) {
      throw std::runtime_error("allocatable pool is smaller than 16");
    }

    const int daemon_cpu = online[1];
    const std::string body = std::string("{\"daemon_id\":\"") + kDaemonId + "\",\"exec_path\":\"" + json_escape(partial) +
                             "\",\"cpu\":" + std::to_string(daemon_cpu) + ",\"listen_port\":" +
                             std::to_string(kServerPort) + ",\"env\":{\"PARTIAL_BURN_PCT\":\"100\"}}";
    const auto started = http_post("/start", body);
    if (response_status(started) != 200) {
      throw std::runtime_error("start partial failed: " + trim(response_body(started)));
    }
    for (int attempt = 0; attempt < 50 && cpuset_cpus().empty(); ++attempt) {
      sleep_ms(50);
    }

    const auto attached = http_post("/scale", std::string("{\"daemon_id\":\"") + kDaemonId +
                                                  "\",\"min\":1,\"max\":" + std::to_string(kMaxCores) +
                                                  ",\"policy\":\"saturate\",\"placement\":\"nearest_first\"}");
    if (response_status(attached) != 200) {
      throw std::runtime_error("scale attach failed: " + trim(response_body(attached)));
    }
    if (response_body(attached).find("\"placement\":\"nearest_first\"") == std::string::npos) {
      throw std::runtime_error("scale did not report nearest_first: " + trim(response_body(attached)));
    }
    std::cout << "scale attached saturate placement nearest_first min 1 max " << kMaxCores << std::endl;

    auto held = cpuset_cpus();
    map = parse_map(response_body(http_get("/map")));
    std::cout << "clients 0 cores " << held.size() << " cpus " << cpu_text(held) << std::endl;

    int printed = static_cast<int>(held.size());
    std::string problems;
    for (int n = 1; n <= kClients; ++n) {
      Client client;
      client.thread = std::thread(client_loop, client.run);
      clients.push_back(std::move(client));
      const auto snapshot = map;
      for (int tick = 0; tick < 4; ++tick) {
        sleep_ms(50);
        const auto now = cpuset_cpus();
        if (now != held) {
          problems += check_nearest_additions(snapshot, held, now);
          held = now;
        }
      }
      const auto settled = cpuset_cpus();
      if (settled != held) {
        problems += check_nearest_additions(snapshot, held, settled);
        held = settled;
      }
      map = parse_map(response_body(http_get("/map")));
      if (static_cast<int>(held.size()) != printed || n == kClients) {
        std::cout << "clients " << n << " cores " << held.size() << " cpus " << cpu_text(held) << " "
                  << deadline_text() << std::endl;
        printed = static_cast<int>(held.size());
      }
    }

    for (int tick = 0; tick < 40; ++tick) {
      const auto snapshot = map;
      const auto now = cpuset_cpus();
      if (now != held) {
        problems += check_nearest_additions(snapshot, held, now);
        held = now;
        std::cout << "clients " << kClients << " cores " << held.size() << " cpus " << cpu_text(held) << " "
                  << deadline_text() << std::endl;
      }
      if (static_cast<int>(held.size()) >= 14) {
        break;
      }
      sleep_ms(100);
      map = parse_map(response_body(http_get("/map")));
    }

    if (held.size() < 14 || held.size() > static_cast<std::size_t>(kMaxCores)) {
      problems += "200 clients settled at " + std::to_string(held.size()) + " cores, want about 16\n";
    }
    std::cout << "clients " << kClients << " cores " << held.size() << " " << deadline_text() << std::endl;

    std::cout << "down" << std::endl;
    while (static_cast<int>(clients.size()) > kServedClients) {
      const int batch = std::min(32, static_cast<int>(clients.size()) - kServedClients);
      drop_clients(clients, batch);
      const auto now = cpuset_cpus();
      if (now != held) {
        std::vector<int> removed;
        for (const int cpu : held) {
          if (!contains(now, cpu)) {
            removed.push_back(cpu);
          }
        }
        if (!removed.empty()) {
          std::cout << "removed " << cpu_text(removed) << std::endl;
        }
        problems += check_removals(held, now);
        held = now;
        std::cout << "down clients " << clients.size() << " cores " << held.size() << " cpus " << cpu_text(held)
                  << " " << deadline_text() << std::endl;
      }
    }

    while (clients.size() > 1) {
      drop_clients(clients, 1);
      const int want = static_cast<int>(clients.size());
      // Each live connection burns a whole core. Saturate keeps the smallest
      // cpuset that is not 90% full.
      const int goal = cores_for_clients(want);
      const bool expect_drop = goal < static_cast<int>(held.size());
      const auto began = monotonic_ns();
      while (monotonic_ns() - began < (expect_drop ? 3000000000LL : 200000000LL)) {
        const auto now = cpuset_cpus();
        if (now != held) {
          std::vector<int> removed;
          for (const int cpu : held) {
            if (!contains(now, cpu)) {
              removed.push_back(cpu);
            }
          }
          if (!removed.empty()) {
            std::cout << "removed " << cpu_text(removed) << std::endl;
          }
          problems += check_removals(held, now);
          held = now;
          std::cout << "down clients " << want << " cores " << held.size() << " cpus " << cpu_text(held) << " "
                    << deadline_text() << std::endl;
        }
        if (static_cast<int>(held.size()) == goal &&
            (!expect_drop || monotonic_ns() - began >= 300000000LL)) {
          break;
        }
        sleep_ms(50);
      }
      if (static_cast<int>(held.size()) != goal) {
        problems += "down clients " + std::to_string(want) + " settled at " + std::to_string(held.size()) +
                    " cores, want " + std::to_string(goal) + "\n";
      }
    }
    std::cout << "down clients " << clients.size() << " cores " << held.size() << " cpus " << cpu_text(held) << " "
              << deadline_text() << std::endl;
    if (!problems.empty()) {
      throw std::runtime_error(problems);
    }

    stop_clients(clients);
    http_post("/kill", std::string("{\"daemon_id\":\"") + kDaemonId + "\"}");
    stop_server();
    std::cout << "autoscale_nearest_first ok" << std::endl;
  } catch (const std::exception& error) {
    stop_clients(clients);
    stop_server();
    std::cerr << error.what() << std::endl;
    return 1;
  }
}
