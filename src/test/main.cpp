#include <dirent.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr int kPort = 8080;
// Mercury cpu N is pinned to Linux cpu N-1, so these cover Linux cpus 0..4.
constexpr int kLinuxCpus[] = {0, 1, 2, 3, 4};

pid_t g_group = -1;

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

int last_cpu(const std::string& stat) {
  const auto close = stat.rfind(')');
  if (close == std::string::npos) {
    return -1;
  }
  std::istringstream fields(stat.substr(close + 1));
  std::string token;
  for (int field = 3; field <= 39; ++field) {
    if (!(fields >> token)) {
      return -1;
    }
  }
  try {
    return std::stoi(token);
  } catch (const std::exception&) {
    return -1;
  }
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

std::vector<std::string> read_executables(const std::string& path) {
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("cannot read " + path);
  }
  const auto dir = dirname_of(path);
  std::vector<std::string> executables;
  std::string line;
  while (std::getline(in, line)) {
    line = trim(line);
    if (line.empty() || line.front() == '#') {
      continue;
    }
    executables.push_back(resolve_executable(line, dir));
  }
  if (executables.empty()) {
    throw std::runtime_error(path + " lists no executables");
  }
  return executables;
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
  timeout.tv_sec = 2;
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

void stop_group() {
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
  _exit(128);
}

pid_t start_mercury(const std::string& mercury) {
  std::vector<std::string> args{mercury, std::to_string(kPort)};
  for (const int cpu : kLinuxCpus) {
    args.push_back(std::to_string(cpu + 1));
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
      const auto response = http_exchange(request);
      if (response_body(response).find("pong") != std::string::npos) {
        return;
      }
    } catch (const std::exception&) {
    }
    sleep_ms(100);
  }
  throw std::runtime_error("mercury did not answer on port 8080");
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

struct Started {
  std::string id;
  std::string path;
  int mercury_cpu = 0;
  int linux_cpu = 0;
  pid_t pid = -1;
};

}  // namespace

int main(int argc, char** argv) {
  struct sigaction action {};
  action.sa_handler = on_signal;
  ::sigaction(SIGINT, &action, nullptr);
  ::sigaction(SIGTERM, &action, nullptr);

  try {
    if (argc > 2) {
      throw std::runtime_error("usage: daemon_test [daemons.txt]");
    }
    const std::string txt = argc == 2 ? argv[1] : DAEMONS_TXT;
    const auto executables = read_executables(txt);
    for (const auto& path : executables) {
      if (::access(path.c_str(), X_OK) != 0) {
        throw std::runtime_error("not executable: " + path);
      }
    }

    const auto mercury = exe_dir() + "/mercury";
    if (::access(mercury.c_str(), X_OK) != 0) {
      throw std::runtime_error("mercury binary not found at " + mercury);
    }

    const auto self = kLinuxCpus[0];
    std::vector<int> daemon_cpus;
    for (const int cpu : kLinuxCpus) {
      if (cpu != self) {
        daemon_cpus.push_back(cpu);
      }
    }

    const pid_t mercury_pid = start_mercury(mercury);
    std::cout << "mercury pid " << mercury_pid << " listening on 127.0.0.1:" << kPort << " linux cpus";
    for (const int cpu : kLinuxCpus) {
      std::cout << ' ' << cpu;
    }
    std::cout << std::endl;
    wait_until_listening(mercury_pid);

    std::vector<Started> started;
    for (std::size_t i = 0; i < executables.size(); ++i) {
      Started daemon;
      daemon.id = "d" + std::to_string(i + 1);
      daemon.path = executables[i];
      daemon.linux_cpu = daemon_cpus[i % daemon_cpus.size()];
      daemon.mercury_cpu = daemon.linux_cpu + 1;
      const std::string body = std::string("{\"daemon_id\":\"") + daemon.id + "\",\"exec_path\":\"" +
                               json_escape(daemon.path) + "\",\"cpu\":" + std::to_string(daemon.mercury_cpu) +
                               "}";
      const auto response = http_exchange(
          "POST /start HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(kPort) +
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

    stop_group();
    if (!problems.empty()) {
      std::cerr << problems;
      return 1;
    }
    std::cout << "running on the expected cpus" << std::endl;
  } catch (const std::exception& error) {
    stop_group();
    std::cerr << error.what() << std::endl;
    return 1;
  }
}
