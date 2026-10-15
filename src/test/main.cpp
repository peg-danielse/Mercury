#include <dirent.h>
#include <sched.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr int kPort = 8080;
constexpr int kHelloPorts[] = {8081, 8082};

pid_t g_group = -1;
pid_t g_hellos[] = {-1, -1};

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

std::string trim(std::string value) {
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) {
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

std::vector<int> parse_cpu_list(const std::string& text) {
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

int last_cpu(const std::string& stat) {
  try {
    const auto token = stat_field(stat, 39);
    if (token.empty()) {
      return -1;
    }
    return std::stoi(token);
  } catch (const std::exception&) {
    return -1;
  }
}

struct Identity {
  bool present = false;
  char state = '?';
  std::string starttime;
};

Identity identity_of(pid_t pid) {
  Identity identity;
  const auto stat = read_file("/proc/" + std::to_string(pid) + "/stat");
  if (stat.empty()) {
    return identity;
  }
  identity.present = true;
  const auto state = stat_field(stat, 3);
  identity.state = state.empty() ? '?' : state.front();
  identity.starttime = stat_field(stat, 22);
  return identity;
}

std::string cpu_text(const std::vector<int>& cpus) {
  if (cpus.empty()) {
    return "none";
  }
  std::ostringstream out;
  for (std::size_t i = 0; i < cpus.size(); ++i) {
    if (i != 0) {
      out << ',';
    }
    out << cpus[i];
  }
  return out.str();
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

std::vector<std::string> stress_ng_dirs() {
  std::vector<std::string> dirs;
  if (const char* env = std::getenv("STRESS_NG_DIR")) {
    dirs.emplace_back(env);
  }
  if (const char* home = std::getenv("HOME")) {
    dirs.push_back(std::string(home) + "/benchmarks/stress-ng");
    dirs.push_back(std::string(home) + "/Benchmarks/stress-ng");
  }
  return dirs;
}

std::string resolve_executable(const std::string& line, const std::string& txt_dir) {
  if (!line.empty() && line.front() == '/') {
    return line;
  }
  const std::string beside = txt_dir + "/" + line;
  if (::access(beside.c_str(), X_OK) == 0) {
    return beside;
  }
  for (const auto& dir : stress_ng_dirs()) {
    const std::string candidate = dir + "/" + line;
    if (::access(candidate.c_str(), X_OK) == 0) {
      return candidate;
    }
  }
  return beside;
}

int cpu_number(const std::string& text) {
  if (text.empty() || text.front() == '+' || text.front() == '-') {
    throw std::runtime_error("invalid cpu " + text);
  }
  try {
    std::size_t used = 0;
    const int cpu = std::stoi(text, &used);
    if (used != text.size() || cpu < 0 || cpu + 1 > CPU_SETSIZE) {
      throw std::runtime_error("invalid cpu " + text);
    }
    return cpu;
  } catch (const std::runtime_error&) {
    throw;
  } catch (const std::exception&) {
    throw std::runtime_error("invalid cpu " + text);
  }
}

void append_cpu(std::vector<int>& cpus, int cpu) {
  if (std::find(cpus.begin(), cpus.end(), cpu) != cpus.end()) {
    throw std::runtime_error("duplicate cpu " + std::to_string(cpu));
  }
  cpus.push_back(cpu);
}

void append_cpu_spec(std::vector<int>& cpus, const std::string& spec) {
  const auto dash = spec.find('-');
  if (dash == std::string::npos) {
    append_cpu(cpus, cpu_number(spec));
    return;
  }
  if (spec.find('-', dash + 1) != std::string::npos) {
    throw std::runtime_error("invalid cpu range " + spec);
  }
  const int first = cpu_number(spec.substr(0, dash));
  const int last = cpu_number(spec.substr(dash + 1));
  if (last < first) {
    throw std::runtime_error("invalid cpu range " + spec);
  }
  for (int cpu = first; cpu <= last; ++cpu) {
    append_cpu(cpus, cpu);
  }
}

std::vector<int> parse_cpu_specs(const std::string& text) {
  std::vector<int> cpus;
  std::string spec;
  auto flush = [&] {
    if (!spec.empty()) {
      append_cpu_spec(cpus, spec);
      spec.clear();
    }
  };
  for (const char c : text) {
    if (c == ' ' || c == '\t' || c == ',') {
      flush();
    } else {
      spec.push_back(c);
    }
  }
  flush();
  if (cpus.empty()) {
    throw std::runtime_error("cpus needs a list");
  }
  if (cpus.size() < 2) {
    throw std::runtime_error("cpus needs mercury and at least one daemon cpu");
  }
  return cpus;
}

enum class Load { cpu, memory };

const char* load_name(Load load) { return load == Load::memory ? "memory" : "cpu"; }

Load infer_load(const std::string& path) {
  const auto slash = path.rfind('/');
  const auto base = slash == std::string::npos ? path : path.substr(slash + 1);
  static const char* memory[] = {"stream", "vm",     "cache",    "qsort", "malloc", "mmap",
                                 "memrate", "tlb",   "brk",      "bigheap", "madvise", "mremap",
                                 "stack",   "zero",  "msync"};
  for (const char* name : memory) {
    if (base == name) {
      return Load::memory;
    }
  }
  return Load::cpu;
}

std::string workload_name(const std::string& path) {
  const auto slash = path.rfind('/');
  const auto base = slash == std::string::npos ? path : path.substr(slash + 1);
  return base.empty() ? "daemon" : base;
}

struct Workload {
  Load load = Load::cpu;
  std::string path;
  std::string name;
};

struct Job {
  std::vector<int> linux_cpus;
  std::vector<Workload> workloads;
};

Job read_job(const std::string& path) {
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("cannot read " + path);
  }
  const auto dir = dirname_of(path);
  Job job;
  bool saw_cpus = false;
  std::string line;
  while (std::getline(in, line)) {
    line = trim(line);
    if (line.empty() || line.front() == '#') {
      continue;
    }
    if (line == "cpus" || line.rfind("cpus ", 0) == 0 || line.rfind("cpus\t", 0) == 0) {
      if (saw_cpus) {
        throw std::runtime_error("cpus specified twice");
      }
      saw_cpus = true;
      const auto list = line.size() <= 4 ? std::string{} : trim(line.substr(4));
      job.linux_cpus = parse_cpu_specs(list);
      continue;
    }
    Load load = Load::cpu;
    std::string spec = line;
    if (line.rfind("memory ", 0) == 0 || line.rfind("memory\t", 0) == 0) {
      load = Load::memory;
      spec = trim(line.substr(6));
    } else if (line.rfind("cpu ", 0) == 0 || line.rfind("cpu\t", 0) == 0) {
      load = Load::cpu;
      spec = trim(line.substr(3));
    } else {
      load = infer_load(line);
    }
    if (spec.empty()) {
      throw std::runtime_error("missing executable");
    }
    Workload work;
    work.load = load;
    work.path = resolve_executable(spec, dir);
    work.name = workload_name(spec);
    job.workloads.push_back(std::move(work));
  }
  if (!saw_cpus) {
    job.linux_cpus = parse_cpu_specs("0-4");
  }
  if (job.workloads.empty()) {
    throw std::runtime_error(path + " lists no executables");
  }
  return job;
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
  timeout.tv_sec = 2;
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

bool http_ok(const std::string& response) {
  const auto line_end = response.find("\r\n");
  const auto line = response.substr(0, line_end == std::string::npos ? response.size() : line_end);
  return line.find(" 200 ") != std::string::npos;
}

int response_pid(const std::string& response) {
  const auto body = response_body(response);
  const auto key = body.find("\"pid\"");
  if (key == std::string::npos) {
    return -1;
  }
  const auto colon = body.find(':', key);
  if (colon == std::string::npos) {
    return -1;
  }
  try {
    return std::stoi(body.substr(colon + 1));
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

void stop_pid(pid_t& pid) {
  const pid_t child = pid;
  if (child <= 0) {
    return;
  }
  pid = -1;
  ::kill(child, SIGTERM);
  for (int i = 0; i < 20; ++i) {
    const auto got = ::waitpid(child, nullptr, WNOHANG);
    if (got == child || (got < 0 && errno == ECHILD)) {
      return;
    }
    sleep_ms(100);
  }
  ::kill(child, SIGKILL);
  while (::waitpid(child, nullptr, 0) < 0 && errno == EINTR) {
  }
}

void stop_group() {
  stop_pid(g_hellos[0]);
  stop_pid(g_hellos[1]);
  const pid_t group = g_group;
  if (group <= 0) {
    return;
  }
  g_group = -1;
  ::kill(-group, SIGTERM);
  for (int i = 0; i < 20; ++i) {
    const auto got = ::waitpid(group, nullptr, WNOHANG);
    if (got == group || (got < 0 && errno == ECHILD)) {
      return;
    }
    sleep_ms(100);
  }
  ::kill(-group, SIGKILL);
  while (::waitpid(group, nullptr, 0) < 0 && errno == EINTR) {
  }
}

void on_signal(int) {
  if (g_group > 0) {
    ::kill(-g_group, SIGKILL);
  }
  if (g_hellos[0] > 0) {
    ::kill(g_hellos[0], SIGKILL);
  }
  if (g_hellos[1] > 0) {
    ::kill(g_hellos[1], SIGKILL);
  }
  _exit(128);
}

pid_t start_mercury(const std::string& mercury, const std::vector<int>& linux_cpus) {
  std::vector<std::string> args{mercury, std::to_string(kPort)};
  for (const int cpu : linux_cpus) {
    args.push_back(std::to_string(cpu));
  }
  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (auto& arg : args) {
    argv.push_back(arg.data());
  }
  argv.push_back(nullptr);

  const pid_t pid = ::fork();
  if (pid < 0) {
    throw std::runtime_error("fork failed");
  }
  if (pid == 0) {
    if (::setpgid(0, 0) != 0) {
      _exit(127);
    }
    ::execv(mercury.c_str(), argv.data());
    _exit(127);
  }
  ::setpgid(pid, pid);
  g_group = pid;
  return pid;
}

void wait_until_listening(pid_t mercury) {
  const std::string request = "GET /ping HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(kPort) +
                              "\r\nConnection: close\r\n\r\n";
  for (int attempt = 0; attempt < 50; ++attempt) {
    int status = 0;
    const auto got = ::waitpid(mercury, &status, WNOHANG);
    if (got == mercury) {
      g_group = -1;
      throw std::runtime_error("mercury exited before listening");
    }
    try {
      const auto response = http_exchange(kPort, request);
      if (response_body(response).find("pong") != std::string::npos) {
        return;
      }
    } catch (const std::exception&) {
    }
    sleep_ms(100);
  }
  throw std::runtime_error("mercury did not answer on port 8080");
}

pid_t start_hello(const std::string& hello, int index) {
  const pid_t pid = ::fork();
  if (pid < 0) {
    throw std::runtime_error("fork failed");
  }
  if (pid == 0) {
    ::execl(hello.c_str(), "hello_server", std::to_string(kHelloPorts[index]).c_str(), nullptr);
    _exit(127);
  }
  g_hellos[index] = pid;
  return pid;
}

void wait_for_relay(int from, int to) {
  const int from_port = kHelloPorts[from];
  const int to_port = kHelloPorts[to];
  const std::string request = "GET /relay/" + std::to_string(to_port) + " HTTP/1.1\r\nHost: 127.0.0.1:" +
                              std::to_string(from_port) + "\r\nConnection: close\r\n\r\n";
  for (int attempt = 0; attempt < 50; ++attempt) {
    for (pid_t& hello : g_hellos) {
      int status = 0;
      if (hello > 0 && ::waitpid(hello, &status, WNOHANG) == hello) {
        hello = -1;
        throw std::runtime_error("hello_server exited before relay");
      }
    }
    try {
      const auto response = http_exchange(from_port, request);
      if (http_ok(response) && trim(response_body(response)) == "success") {
        std::cout << "relay " << from_port << " -> " << to_port << " success" << std::endl;
        return;
      }
    } catch (const std::exception&) {
    }
    sleep_ms(100);
  }
  throw std::runtime_error("relay " + std::to_string(from_port) + " -> " + std::to_string(to_port) +
                           " did not succeed");
}

struct Task {
  pid_t pid = -1;
  char state = '?';
  std::string name;
  std::vector<int> cpus;
  int last = -1;
  bool alive = false;
};

Task read_task(pid_t pid, pid_t tid) {
  const auto base = "/proc/" + std::to_string(pid) + "/task/" + std::to_string(tid);
  Task task;
  task.pid = tid;
  const auto status = read_file(base + "/status");
  const auto stat = read_file(base + "/stat");
  if (status.empty() || stat.empty()) {
    return task;
  }
  task.alive = true;
  task.name = status_field(status, "Name");
  const auto state = status_field(status, "State");
  task.state = state.empty() ? '?' : state.front();
  task.cpus = parse_cpu_list(status_field(status, "Cpus_allowed_list"));
  task.last = last_cpu(stat);
  return task;
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

std::vector<std::pair<pid_t, pid_t>> process_parents() {
  std::vector<std::pair<pid_t, pid_t>> rows;
  DIR* dir = ::opendir("/proc");
  if (dir == nullptr) {
    throw std::runtime_error("opendir /proc failed");
  }
  while (const auto* entry = ::readdir(dir)) {
    if (entry->d_name[0] < '1' || entry->d_name[0] > '9') {
      continue;
    }
    const auto pid = static_cast<pid_t>(std::atoi(entry->d_name));
    const auto ppid_text = status_field(read_file("/proc/" + std::to_string(pid) + "/status"), "PPid");
    if (ppid_text.empty()) {
      continue;
    }
    try {
      rows.emplace_back(pid, static_cast<pid_t>(std::stoi(ppid_text)));
    } catch (const std::exception&) {
    }
  }
  ::closedir(dir);
  return rows;
}

std::vector<pid_t> descendants(pid_t root) {
  const auto rows = process_parents();
  std::vector<pid_t> pending{root};
  std::set<pid_t> seen{root};
  std::vector<pid_t> out{root};
  for (std::size_t i = 0; i < pending.size(); ++i) {
    for (const auto& [pid, ppid] : rows) {
      if (ppid == pending[i] && seen.insert(pid).second) {
        pending.push_back(pid);
        out.push_back(pid);
      }
    }
  }
  return out;
}

bool running_state(char state) { return state == 'R' || state == 'S' || state == 'D'; }

std::string check_process(pid_t pid, int linux_cpu, bool with_children) {
  const auto processes = with_children ? descendants(pid) : std::vector<pid_t>{pid};
  if (processes.empty()) {
    return "pid " + std::to_string(pid) + " is not running";
  }
  std::ostringstream errors;
  int tasks = 0;
  for (const auto proc : processes) {
    const auto tids = task_ids(proc);
    if (tids.empty()) {
      errors << "pid " << proc << " is not running\n";
      continue;
    }
    for (const auto tid : tids) {
      const auto task = read_task(proc, tid);
      ++tasks;
      if (!task.alive) {
        errors << "pid " << tid << " is not running\n";
        continue;
      }
      if (!running_state(task.state)) {
        errors << "pid " << tid << " (" << task.name << ") state " << task.state << " is not running\n";
      }
      if (task.cpus.size() != 1 || task.cpus.front() != linux_cpu) {
        errors << "pid " << tid << " (" << task.name << ") allowed cpus " << cpu_text(task.cpus)
               << ", expected " << linux_cpu << "\n";
      }
      if (task.last != linux_cpu) {
        errors << "pid " << tid << " (" << task.name << ") last ran on cpu " << task.last << ", expected "
               << linux_cpu << "\n";
      }
    }
  }
  if (tasks == 0) {
    return "pid " + std::to_string(pid) + " is not running";
  }
  return errors.str();
}

std::string fd_target(pid_t pid, int fd) {
  char buf[256];
  const auto path = "/proc/" + std::to_string(pid) + "/fd/" + std::to_string(fd);
  const auto n = ::readlink(path.c_str(), buf, sizeof(buf) - 1);
  if (n < 0) {
    return {};
  }
  buf[n] = '\0';
  return buf;
}

void migrate_hello(const std::string& hello, int from_linux, int to_linux) {
  constexpr int kListenPort = 8083;
  const std::string start_body = std::string("{\"daemon_id\":\"hello\",\"exec_path\":\"") + json_escape(hello) +
                                  "\",\"cpu\":" + std::to_string(from_linux) +
                                  ",\"listen_port\":" + std::to_string(kListenPort) + "}";
  const auto started = http_exchange(
      kPort, "POST /start HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(kPort) +
                 "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(start_body.size()) +
                 "\r\nConnection: close\r\n\r\n" + start_body);
  const pid_t old_pid = response_pid(started);
  if (!http_ok(started) || old_pid <= 0) {
    throw std::runtime_error("start hello failed: " + trim(response_body(started)));
  }

  const std::string ping = "GET /ping HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(kListenPort) +
                           "\r\nConnection: close\r\n\r\n";
  bool up = false;
  std::string socket_id;
  for (int attempt = 0; attempt < 50; ++attempt) {
    try {
      const auto response = http_exchange(kListenPort, ping);
      if (response_body(response).find("pong") != std::string::npos) {
        socket_id = fd_target(old_pid, 3);
        if (!socket_id.empty()) {
          up = true;
          break;
        }
      }
    } catch (const std::exception&) {
    }
    sleep_ms(100);
  }
  if (!up) {
    throw std::runtime_error("hello daemon did not answer on port 8083");
  }

  const std::string migrate_body =
      std::string("{\"daemon_id\":\"hello\",\"cpu\":") + std::to_string(to_linux) + "}";
  const auto moved = http_exchange(
      kPort, "POST /migrate HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(kPort) +
                 "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(migrate_body.size()) +
                 "\r\nConnection: close\r\n\r\n" + migrate_body);
  const pid_t new_pid = response_pid(moved);
  if (!http_ok(moved) || new_pid <= 0) {
    throw std::runtime_error("migrate hello failed: " + trim(response_body(moved)));
  }

  bool old_dead = false;
  std::string placement;
  for (int attempt = 0; attempt < 30; ++attempt) {
    const auto old_id = identity_of(old_pid);
    old_dead = !old_id.present || old_id.starttime.empty();
    const auto same_socket = fd_target(new_pid, 3) == socket_id;
    placement = check_process(new_pid, to_linux, false);
    try {
      const auto response = http_exchange(kListenPort, ping);
      if (old_dead && same_socket && placement.empty() && response_body(response).find("pong") != std::string::npos) {
        break;
      }
    } catch (const std::exception&) {
    }
    old_dead = false;
    sleep_ms(100);
  }
  if (!old_dead || !placement.empty() || fd_target(new_pid, 3) != socket_id) {
    throw std::runtime_error("migrate left pid " + std::to_string(old_pid) + " or socket " + socket_id +
                             " pid " + std::to_string(new_pid) + " fd " + fd_target(new_pid, 3) + "\n" + placement);
  }
  std::cout << "migrated hello pid " << old_pid << " -> " << new_pid << " linux cpu " << to_linux << " socket "
            << socket_id << std::endl;

  const std::string kill_body = R"({"daemon_id":"hello"})";
  const auto killed = http_exchange(
      kPort, "POST /kill HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(kPort) +
                 "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(kill_body.size()) +
                 "\r\nConnection: close\r\n\r\n" + kill_body);
  if (!http_ok(killed)) {
    throw std::runtime_error("kill hello failed: " + trim(response_body(killed)));
  }
  for (int attempt = 0; attempt < 30; ++attempt) {
    const auto identity = identity_of(new_pid);
    if (!identity.present) {
      std::cout << "dead hello pid " << new_pid << std::endl;
      return;
    }
    sleep_ms(100);
  }
  throw std::runtime_error("hello pid " + std::to_string(new_pid) + " still alive after kill");
}

struct Started {
  std::string id;
  std::string path;
  int mercury_cpu = 0;
  int linux_cpu = 0;
  pid_t pid = -1;
};

constexpr int kPlaceMs = 5000;

struct CoreGroup {
  int node = 0;
  int package = 0;
  int core = 0;
  std::vector<int> threads;
};

int read_sysfs_int(const std::string& path) {
  std::ifstream in(path);
  int value = 0;
  in >> value;
  return value;
}

int node_of(int cpu) {
  const auto dir = "/sys/devices/system/cpu/cpu" + std::to_string(cpu);
  DIR* handle = ::opendir(dir.c_str());
  if (handle == nullptr) {
    return 0;
  }
  int node = 0;
  while (const auto* entry = ::readdir(handle)) {
    if (std::strncmp(entry->d_name, "node", 4) == 0 && entry->d_name[4] >= '0' && entry->d_name[4] <= '9') {
      try {
        node = std::stoi(entry->d_name + 4);
      } catch (const std::exception&) {
      }
      break;
    }
  }
  ::closedir(handle);
  return node;
}

std::vector<CoreGroup> cores_of(const std::vector<int>& cpus) {
  std::vector<CoreGroup> cores;
  for (const int cpu : cpus) {
    const auto base = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/topology/";
    const int package = read_sysfs_int(base + "physical_package_id");
    const int core = read_sysfs_int(base + "core_id");
    const int node = node_of(cpu);
    const auto found = std::find_if(cores.begin(), cores.end(), [&](const CoreGroup& group) {
      return group.package == package && group.core == core;
    });
    if (found == cores.end()) {
      cores.push_back(CoreGroup{node, package, core, {cpu}});
    } else {
      found->threads.push_back(cpu);
    }
  }
  for (auto& group : cores) {
    std::sort(group.threads.begin(), group.threads.end());
  }
  return cores;
}

std::vector<CoreGroup> stripe_cores(std::vector<CoreGroup> cores) {
  std::map<int, std::vector<CoreGroup>> by_node;
  for (auto& core : cores) {
    by_node[core.node].push_back(std::move(core));
  }
  std::vector<CoreGroup> ordered;
  bool more = true;
  while (more) {
    more = false;
    for (auto& [node, list] : by_node) {
      (void)node;
      if (!list.empty()) {
        ordered.push_back(std::move(list.front()));
        list.erase(list.begin());
        more = true;
      }
    }
  }
  return ordered;
}

std::vector<int> assign_round_robin(std::size_t count, const std::vector<int>& daemon_cpus) {
  std::vector<int> assigned;
  assigned.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    assigned.push_back(daemon_cpus[i % daemon_cpus.size()]);
  }
  return assigned;
}

std::vector<int> assign_worst(std::size_t count, const std::vector<int>& daemon_cpus) {
  return std::vector<int>(count, daemon_cpus.front());
}

std::vector<int> assign_interleave(const std::vector<Workload>& works, const std::vector<int>& daemon_cpus) {
  const auto cores = stripe_cores(cores_of(daemon_cpus));
  struct Slot {
    int memory = 0;
    int cpu = 0;
  };
  std::vector<Slot> slots;
  for (const auto& core : cores) {
    if (core.threads.empty()) {
      continue;
    }
    Slot slot;
    slot.memory = core.threads.front();
    slot.cpu = core.threads.size() > 1 ? core.threads.back() : core.threads.front();
    slots.push_back(slot);
  }
  if (slots.empty()) {
    throw std::runtime_error("no daemon cpu for interleave");
  }
  std::vector<std::size_t> memory_index;
  std::vector<std::size_t> cpu_index;
  for (std::size_t i = 0; i < works.size(); ++i) {
    if (works[i].load == Load::memory) {
      memory_index.push_back(i);
    } else {
      cpu_index.push_back(i);
    }
  }
  std::vector<int> assigned(works.size(), daemon_cpus.front());
  for (std::size_t i = 0; i < memory_index.size(); ++i) {
    assigned[memory_index[i]] = slots[i % slots.size()].memory;
  }
  for (std::size_t i = 0; i < cpu_index.size(); ++i) {
    assigned[cpu_index[i]] = slots[i % slots.size()].cpu;
  }
  return assigned;
}

Started start_daemon(const Workload& work, int linux_cpu) {
  Started daemon;
  daemon.id = work.name;
  daemon.path = work.path;
  daemon.linux_cpu = linux_cpu;
  daemon.mercury_cpu = linux_cpu;
  const std::string body = std::string("{\"daemon_id\":\"") + daemon.id + "\",\"exec_path\":\"" +
                           json_escape(daemon.path) + "\",\"cpu\":" + std::to_string(daemon.mercury_cpu) + "}";
  const auto response = http_exchange(
      kPort, "POST /start HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(kPort) +
                 "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
                 "\r\nConnection: close\r\n\r\n" +
                 body);
  daemon.pid = response_pid(response);
  if (!http_ok(response) || daemon.pid <= 0) {
    throw std::runtime_error("start failed for " + daemon.path + ": " + trim(response_body(response)));
  }
  return daemon;
}

void kill_daemon(const Started& daemon) {
  const std::string body = std::string("{\"daemon_id\":\"") + json_escape(daemon.id) + "\"}";
  const auto response = http_exchange(
      kPort, "POST /kill HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(kPort) +
                 "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
                 "\r\nConnection: close\r\n\r\n" +
                 body);
  std::cout << "kill " << daemon.id << " pid " << daemon.pid << std::endl;
  if (!http_ok(response)) {
    throw std::runtime_error("kill " + daemon.id + " failed: " + trim(response_body(response)));
  }
}

std::uint32_t load_u32(const unsigned char* bytes) {
  return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
         (static_cast<std::uint32_t>(bytes[2]) << 16) | (static_cast<std::uint32_t>(bytes[3]) << 24);
}

std::uint64_t load_u64(const unsigned char* bytes) {
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(bytes[i]) << (8 * i);
  }
  return value;
}

struct CounterFile {
  std::vector<std::string> names;
  std::uint64_t samples = 0;
  std::uint64_t values[8]{};
};

CounterFile read_counter_file(const std::string& path) {
  CounterFile file;
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return file;
  }
  unsigned char header[40];
  in.read(reinterpret_cast<char*>(header), sizeof(header));
  if (!in || std::memcmp(header, "MRING1", 6) != 0) {
    return file;
  }
  const auto name_count = load_u32(header + 12);
  const auto committed = load_u64(header + 24);
  const auto data_offset = load_u64(header + 32);
  for (std::uint32_t i = 0; i < name_count && i < 8; ++i) {
    unsigned char len_bytes[4];
    in.read(reinterpret_cast<char*>(len_bytes), sizeof(len_bytes));
    if (!in) {
      return file;
    }
    const auto len = load_u32(len_bytes);
    std::string name(len, '\0');
    if (len > 0) {
      in.read(name.data(), static_cast<std::streamsize>(len));
    }
    file.names.push_back(name);
  }
  if (data_offset > 0) {
    in.seekg(static_cast<std::streamoff>(data_offset));
  }
  constexpr std::size_t kRecord = 8 + 8 + 8 + 4 + 4 + 8 * 8;
  for (std::uint64_t i = 0; i < committed; ++i) {
    unsigned char record[kRecord];
    in.read(reinterpret_cast<char*>(record), sizeof(record));
    if (!in) {
      break;
    }
    ++file.samples;
    for (int value = 0; value < 8; ++value) {
      file.values[value] += load_u64(record + 32 + static_cast<std::size_t>(value) * 8);
    }
  }
  return file;
}

int counter_index(const CounterFile& file, const char* name) {
  for (std::size_t i = 0; i < file.names.size(); ++i) {
    if (file.names[i] == name) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

double counter_ratio(const CounterFile& file, const char* num, const char* den) {
  const int n = counter_index(file, num);
  const int d = counter_index(file, den);
  if (n < 0 || d < 0 || file.values[d] == 0) {
    return -1;
  }
  return static_cast<double>(file.values[n]) / static_cast<double>(file.values[d]);
}

void print_counter_line(const std::string& scenario, const Workload& work, int linux_cpu,
                        const std::string& path) {
  const auto file = read_counter_file(path);
  const double ipc = counter_ratio(file, "instructions", "cycles");
  const double misses = counter_ratio(file, "cache-misses", "cache-references");
  std::cout << scenario << ' ' << work.name << ' ' << load_name(work.load) << " linux_cpu " << linux_cpu
            << " samples " << file.samples;
  if (ipc >= 0) {
    std::cout << " ipc " << ipc;
  }
  if (misses >= 0) {
    std::cout << " cache_miss_ratio " << misses;
  }
  const int switches = counter_index(file, "context-switches");
  if (switches >= 0) {
    std::cout << " context_switches " << file.values[switches];
  }
  std::cout << " file " << path << std::endl;
}

std::string scenario_dir(const std::string& bindir, const std::string& scenario) {
  const auto path =
      std::filesystem::weakly_canonical(std::filesystem::path(bindir) / ".." / "rings" / scenario);
  std::filesystem::create_directories(path);
  return path.string();
}

void check_started(pid_t mercury_pid, int mercury_cpu, const std::vector<Started>& started) {
  std::string problems;
  for (int attempt = 0; attempt < 30; ++attempt) {
    problems.clear();
    const auto mercury_problem = check_process(mercury_pid, mercury_cpu, false);
    if (!mercury_problem.empty()) {
      problems += "mercury pid " + std::to_string(mercury_pid) + "\n" + mercury_problem;
    }
    for (const auto& daemon : started) {
      const auto problem = check_process(daemon.pid, daemon.linux_cpu, true);
      if (!problem.empty()) {
        problems += daemon.id + " " + daemon.path + "\n" + problem;
      }
    }
    if (problems.empty()) {
      return;
    }
    sleep_ms(100);
  }
  throw std::runtime_error(problems);
}

void run_together(const std::string& mercury, const Job& job, const std::vector<int>& assigned,
                  const std::string& scenario, const std::string& dir, int mercury_cpu) {
  ::setenv("MERCURY_RINGS", dir.c_str(), 1);
  const pid_t mercury_pid = start_mercury(mercury, job.linux_cpus);
  std::cout << scenario << " mercury pid " << mercury_pid << " rings " << dir << std::endl;
  wait_until_listening(mercury_pid);
  std::vector<Started> started;
  for (std::size_t i = 0; i < job.workloads.size(); ++i) {
    auto daemon = start_daemon(job.workloads[i], assigned[i]);
    started.push_back(daemon);
    std::cout << scenario << ' ' << daemon.id << ' ' << load_name(job.workloads[i].load) << " pid " << daemon.pid
              << " linux cpu " << daemon.linux_cpu << std::endl;
  }
  check_started(mercury_pid, mercury_cpu, started);
  sleep_ms(kPlaceMs);
  for (const auto& daemon : started) {
    kill_daemon(daemon);
  }
  stop_group();
}

void run_individual(const std::string& mercury, const Job& job, int linux_cpu, const std::string& dir,
                    int mercury_cpu) {
  ::setenv("MERCURY_RINGS", dir.c_str(), 1);
  const pid_t mercury_pid = start_mercury(mercury, job.linux_cpus);
  std::cout << "individual mercury pid " << mercury_pid << " rings " << dir << std::endl;
  wait_until_listening(mercury_pid);
  for (const auto& work : job.workloads) {
    const auto daemon = start_daemon(work, linux_cpu);
    std::cout << "individual " << daemon.id << ' ' << load_name(work.load) << " pid " << daemon.pid
              << " linux cpu " << daemon.linux_cpu << std::endl;
    check_started(mercury_pid, mercury_cpu, {daemon});
    sleep_ms(kPlaceMs);
    kill_daemon(daemon);
  }
  stop_group();
}

void run_place(const std::string& mercury, const std::string& bindir, const Job& job,
               const std::vector<int>& daemon_cpus) {
  std::set<std::string> names;
  for (const auto& work : job.workloads) {
    if (!names.insert(work.name).second) {
      throw std::runtime_error("duplicate workload name " + work.name);
    }
  }
  const int mercury_cpu = *std::min_element(job.linux_cpus.begin(), job.linux_cpus.end());
  const auto individual_cpu = daemon_cpus.front();
  const auto round_robin = assign_round_robin(job.workloads.size(), daemon_cpus);
  const auto interleave = assign_interleave(job.workloads, daemon_cpus);
  const auto worst = assign_worst(job.workloads.size(), daemon_cpus);

  const auto individual_dir = scenario_dir(bindir, "individual");
  const auto round_dir = scenario_dir(bindir, "roundrobin");
  const auto interleave_dir = scenario_dir(bindir, "interleave");
  const auto worst_dir = scenario_dir(bindir, "worst");

  run_individual(mercury, job, individual_cpu, individual_dir, mercury_cpu);
  run_together(mercury, job, round_robin, "roundrobin", round_dir, mercury_cpu);
  run_together(mercury, job, interleave, "interleave", interleave_dir, mercury_cpu);
  run_together(mercury, job, worst, "worst", worst_dir, mercury_cpu);

  std::cout << "counter files" << std::endl;
  for (std::size_t i = 0; i < job.workloads.size(); ++i) {
    const auto& work = job.workloads[i];
    print_counter_line("individual", work, individual_cpu, individual_dir + "/" + work.name + ".mring");
    print_counter_line("roundrobin", work, round_robin[i], round_dir + "/" + work.name + ".mring");
    print_counter_line("interleave", work, interleave[i], interleave_dir + "/" + work.name + ".mring");
    print_counter_line("worst", work, worst[i], worst_dir + "/" + work.name + ".mring");
  }
}

}  // namespace

int main(int argc, char** argv) {
  struct sigaction action {};
  action.sa_handler = on_signal;
  ::sigaction(SIGINT, &action, nullptr);
  ::sigaction(SIGTERM, &action, nullptr);

  try {
    bool place = false;
    std::string txt = DAEMONS_TXT;
    if (argc == 2 && std::string(argv[1]) == "--place") {
      place = true;
    } else if (argc == 3 && std::string(argv[1]) == "--place") {
      place = true;
      txt = argv[2];
    } else if (argc == 2) {
      txt = argv[1];
    } else if (argc > 1) {
      throw std::runtime_error("usage: daemon_test [--place] [daemons.txt]");
    }
    const auto job = read_job(txt);
    for (const auto& work : job.workloads) {
      if (::access(work.path.c_str(), X_OK) != 0) {
        throw std::runtime_error("not executable: " + work.path);
      }
    }

    const auto testdir = exe_dir();
    const auto bindir = dirname_of(testdir);
    const auto mercury = bindir + "/mercury";
    const auto hello = testdir + "/hello_server";
    if (::access(mercury.c_str(), X_OK) != 0) {
      throw std::runtime_error("mercury binary not found at " + mercury);
    }
    if (::access(hello.c_str(), X_OK) != 0) {
      throw std::runtime_error("hello_server binary not found at " + hello);
    }

    const auto self = *std::min_element(job.linux_cpus.begin(), job.linux_cpus.end());
    std::vector<int> daemon_cpus;
    for (const int cpu : job.linux_cpus) {
      if (cpu != self) {
        daemon_cpus.push_back(cpu);
      }
    }
    std::sort(daemon_cpus.begin(), daemon_cpus.end());

    if (place) {
      run_place(mercury, bindir, job, daemon_cpus);
      std::cout << "kept counter files under rings/individual and rings/roundrobin" << std::endl;
      return 0;
    }

    start_hello(hello, 0);
    start_hello(hello, 1);
    const pid_t mercury_pid = start_mercury(mercury, job.linux_cpus);
    std::cout << "mercury pid " << mercury_pid << " listening on 127.0.0.1:" << kPort << " linux cpus";
    for (const int cpu : job.linux_cpus) {
      std::cout << ' ' << cpu;
    }
    std::cout << std::endl;
    wait_until_listening(mercury_pid);
    wait_for_relay(0, 1);
    wait_for_relay(1, 0);
    if (daemon_cpus.size() < 2) {
      throw std::runtime_error("migrate needs two daemon cpus");
    }
    migrate_hello(hello, daemon_cpus[0], daemon_cpus[1]);

    std::vector<Started> started;
    for (std::size_t i = 0; i < job.workloads.size(); ++i) {
      Started daemon;
      daemon.id = "d" + std::to_string(i + 1);
      daemon.path = job.workloads[i].path;
      daemon.linux_cpu = daemon_cpus[i % daemon_cpus.size()];
      daemon.mercury_cpu = daemon.linux_cpu;
      const std::string body = std::string("{\"daemon_id\":\"") + daemon.id + "\",\"exec_path\":\"" +
                               json_escape(daemon.path) + "\",\"cpu\":" + std::to_string(daemon.mercury_cpu) +
                               "}";
      const auto response = http_exchange(
          kPort, "POST /start HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(kPort) +
          "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
          "\r\nConnection: close\r\n\r\n" + body);
      daemon.pid = response_pid(response);
      if (!http_ok(response) || daemon.pid <= 0) {
        throw std::runtime_error("start failed for " + daemon.path + ": " + trim(response_body(response)));
      }
      started.push_back(daemon);
      std::cout << "submitted " << daemon.id << " " << daemon.path << " pid " << daemon.pid << " linux cpu "
                << daemon.linux_cpu << std::endl;
    }

    std::string problems;
    for (int attempt = 0; attempt < 30; ++attempt) {
      problems.clear();
      const auto mercury_problem = check_process(mercury_pid, self, false);
      if (!mercury_problem.empty()) {
        problems += "mercury pid " + std::to_string(mercury_pid) + "\n" + mercury_problem;
      }
      for (const auto& daemon : started) {
        const auto problem = check_process(daemon.pid, daemon.linux_cpu, true);
        if (!problem.empty()) {
          problems += daemon.id + " " + daemon.path + "\n" + problem;
        }
      }
      if (problems.empty()) {
        break;
      }
      sleep_ms(100);
    }

    std::cout << "check mercury pid " << mercury_pid << " linux cpu " << self << std::endl;
    for (const auto& daemon : started) {
      const auto tree = descendants(daemon.pid);
      std::cout << "check " << daemon.id << " pid " << daemon.pid << " linux cpu " << daemon.linux_cpu
                << " processes " << tree.size() << std::endl;
    }
    if (!problems.empty()) {
      stop_group();
      std::cerr << problems;
      return 1;
    }
    std::cout << "running on the expected cpus" << std::endl;

    struct Tracked {
      std::string id;
      pid_t pid = -1;
      std::string starttime;
    };
    std::vector<Tracked> tracked;
    for (const auto& daemon : started) {
      for (const auto proc : descendants(daemon.pid)) {
        const auto identity = identity_of(proc);
        tracked.push_back(Tracked{daemon.id, proc, identity.starttime});
      }
      const std::string body = std::string("{\"daemon_id\":\"") + json_escape(daemon.id) + "\"}";
      const auto response = http_exchange(
          kPort, "POST /kill HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(kPort) +
          "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
          "\r\nConnection: close\r\n\r\n" + body);
      std::cout << "kill " << daemon.id << " pid " << daemon.pid << std::endl;
      if (!http_ok(response)) {
        problems += "kill " + daemon.id + " failed: " + trim(response_body(response)) + "\n";
      }
    }

    std::string alive;
    for (int attempt = 0; attempt < 30; ++attempt) {
      alive.clear();
      for (const auto& proc : tracked) {
        const auto identity = identity_of(proc.pid);
        if (identity.present && identity.starttime == proc.starttime) {
          alive += proc.id + " pid " + std::to_string(proc.pid) + " still alive state " + identity.state + "\n";
        }
      }
      if (alive.empty()) {
        break;
      }
      sleep_ms(100);
    }
    problems += alive;
    for (const auto& proc : tracked) {
      const auto identity = identity_of(proc.pid);
      if (!identity.present || identity.starttime != proc.starttime) {
        std::cout << "dead " << proc.id << " pid " << proc.pid << std::endl;
      }
    }

    stop_group();
    if (!problems.empty()) {
      std::cerr << problems;
      return 1;
    }
    std::cout << "daemons are dead" << std::endl;
  } catch (const std::exception& error) {
    stop_group();
    std::cerr << error.what() << std::endl;
    return 1;
  }
}
