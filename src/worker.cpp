#include "http.hpp"

#include <poll.h>
#include <sched.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr std::string_view kJson = "application/json";

std::vector<int> job_cpus;

struct Daemon {
  std::string job_id;
  std::string exec_path;
  pid_t pid = -1;
  bool running = false;
  int exit_code = -1;
  std::string log;
  std::optional<std::int64_t> finished;
};

std::mutex daemons_mu;
std::condition_variable daemons_cv;
std::vector<Daemon> daemons;

std::int64_t now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

Daemon* find_daemon(const std::string& job_id) {
  for (auto& daemon : daemons) {
    if (daemon.job_id == job_id) {
      return &daemon;
    }
  }
  return nullptr;
}

std::string ok_json(bool ok) { return ok ? R"({"ok":true})" : R"({"ok":false})"; }

std::string kill_json(std::int64_t finished) {
  return "{\"ok\":true,\"finished\":" + std::to_string(finished) + "}";
}

void pin_to(int cpu) {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu - 1, &set);
  if (::sched_setaffinity(0, sizeof(set), &set) != 0) {
    throw std::runtime_error("sched_setaffinity failed");
  }
}

std::string request_body(const std::string& request);
std::optional<std::string> json_value(const std::string& json, std::string_view key);

// stat — {exec_path} returns {ok} for a daemon currently in the cache
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

std::string request_body(const std::string& request) {
  const auto header_end = request.find("\r\n\r\n");
  if (header_end == std::string::npos) {
    return {};
  }
  return request.substr(header_end + 4);
}

std::optional<std::string> json_value(const std::string& json, std::string_view key) {
  const std::string pattern = std::string("\"") + std::string(key) + "\"";
  const auto key_at = json.find(pattern);
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

void watch_job(std::string job_id, pid_t pid, int out_fd, int err_fd) {
  const auto log = read_pipes(out_fd, err_fd);
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  const int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  const auto finished = now_ms();
  {
    std::lock_guard lock(daemons_mu);
    if (auto* daemon = find_daemon(job_id); daemon != nullptr && daemon->pid == pid) {
      daemon->running = false;
      daemon->exit_code = exit_code;
      daemon->log = log;
      daemon->finished = finished;
    }
  }
  daemons_cv.notify_all();
  std::cout << "exited job_id=" << job_id << " exit_code=" << exit_code << " finished=" << finished
            << " log=" << log << std::flush;
}

std::optional<std::string> start_route(const std::string& request) {
  if (request_target(request) != "/start") {
    return std::nullopt;
  }

  const auto body = request_body(request);
  const auto job_id = json_value(body, "job_id");
  const auto exec_path = json_value(body, "exec_path");
  const auto cpu_text = json_value(body, "cpu");
  if (!job_id || !exec_path || exec_path->empty() || !cpu_text) {
    return http_response(400, "Bad Request", R"({"pid":0})", kJson);
  }

  int cpu = 0;
  try {
    cpu = std::stoi(*cpu_text);
  } catch (const std::exception&) {
    return http_response(400, "Bad Request", R"({"pid":0})", kJson);
  }
  if (std::find(job_cpus.begin(), job_cpus.end(), cpu) == job_cpus.end() ||
      ::access(exec_path->c_str(), X_OK) != 0) {
    return http_response(400, "Bad Request", R"({"pid":0})", kJson);
  }
  {
    std::lock_guard lock(daemons_mu);
    if (const auto* daemon = find_daemon(*job_id); daemon != nullptr && daemon->running) {
      return http_response(400, "Bad Request", R"({"pid":0})", kJson);
    }
  }

  int out_pipe[2] = {-1, -1};
  int err_pipe[2] = {-1, -1};
  if (::pipe(out_pipe) != 0) {
    return http_response(500, "Internal Server Error", R"({"pid":0})", kJson);
  }
  if (::pipe(err_pipe) != 0) {
    ::close(out_pipe[0]);
    ::close(out_pipe[1]);
    return http_response(500, "Internal Server Error", R"({"pid":0})", kJson);
  }

  const pid_t pid = ::fork();
  if (pid < 0) {
    ::close(out_pipe[0]);
    ::close(out_pipe[1]);
    ::close(err_pipe[0]);
    ::close(err_pipe[1]);
    return http_response(500, "Internal Server Error", R"({"pid":0})", kJson);
  }
  if (pid == 0) {
    ::dup2(out_pipe[1], STDOUT_FILENO);
    ::dup2(err_pipe[1], STDERR_FILENO);
    ::close(out_pipe[0]);
    ::close(out_pipe[1]);
    ::close(err_pipe[0]);
    ::close(err_pipe[1]);

    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu - 1, &set);
    if (::sched_setaffinity(0, sizeof(set), &set) != 0) {
      _exit(127);
    }
    std::vector<char> arg0(exec_path->begin(), exec_path->end());
    arg0.push_back('\0');
    char* argv[] = {arg0.data(), nullptr};
    ::execv(exec_path->c_str(), argv);
    _exit(127);
  }

  ::close(out_pipe[1]);
  ::close(err_pipe[1]);
  {
    std::lock_guard lock(daemons_mu);
    if (auto* daemon = find_daemon(*job_id)) {
      *daemon = Daemon{*job_id, *exec_path, pid, true, -1, {}, std::nullopt};
    } else {
      daemons.push_back(Daemon{*job_id, *exec_path, pid, true, -1, {}, std::nullopt});
    }
  }
  std::thread(watch_job, *job_id, pid, out_pipe[0], err_pipe[0]).detach();
  return http_response(200, "OK", "{\"pid\":" + std::to_string(pid) + "}", kJson);
}

// kill — {job_id} returns {ok, finished}
std::optional<std::string> kill_route(const std::string& request) {
  if (request_target(request) != "/kill") {
    return std::nullopt;
  }
  const auto job_id = json_value(request_body(request), "job_id");
  if (!job_id || job_id->empty()) {
    return http_response(400, "Bad Request", ok_json(false), kJson);
  }

  std::unique_lock lock(daemons_mu);
  auto* daemon = find_daemon(*job_id);
  if (daemon == nullptr) {
    return http_response(200, "OK", ok_json(false), kJson);
  }
  if (!daemon->finished) {
    const auto pid = daemon->pid;
    ::kill(pid, SIGTERM);
    if (!daemons_cv.wait_for(lock, std::chrono::seconds(2), [&] {
          daemon = find_daemon(*job_id);
          return daemon != nullptr && daemon->finished.has_value();
        })) {
      ::kill(pid, SIGKILL);
      daemons_cv.wait(lock, [&] {
        daemon = find_daemon(*job_id);
        return daemon != nullptr && daemon->finished.has_value();
      });
    }
  }
  return http_response(200, "OK", kill_json(*daemon->finished), kJson);
}

std::string route(const std::string& request) {
  if (auto response = ping_route(request)) {
    return *response;
  }
  if (auto response = stat_route(request)) {
    return *response;
  }
  if (auto response = start_route(request)) {
    return *response;
  }
  if (auto response = kill_route(request)) {
    return *response;
  }
  return http_response(404, "Not Found", "not found");
}

}  // namespace

int parse_cpu(const char* text) {
  try {
    std::size_t used = 0;
    const int cpu = std::stoi(text, &used);
    if (text[used] != '\0' || cpu < 1 || cpu > CPU_SETSIZE) {
      throw std::runtime_error("invalid cpu");
    }
    return cpu;
  } catch (const std::runtime_error&) {
    throw;
  } catch (const std::exception&) {
    throw std::runtime_error("invalid cpu");
  }
}

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: worker <port> <cpu> [cpu...]\n";
    return 1;
  }
  try {
    const int port_number = std::stoi(argv[1]);
    if (port_number < 1 || port_number > 65535) {
      throw std::runtime_error("invalid port");
    }
    std::vector<int> cpus;
    for (int i = 2; i < argc; ++i) {
      const int cpu = parse_cpu(argv[i]);
      if (std::find(cpus.begin(), cpus.end(), cpu) != cpus.end()) {
        throw std::runtime_error("duplicate cpu");
      }
      cpus.push_back(cpu);
    }
    const int self = *std::min_element(cpus.begin(), cpus.end());
    pin_to(self);
    job_cpus.clear();
    for (const int cpu : cpus) {
      if (cpu != self) {
        job_cpus.push_back(cpu);
      }
    }
    std::sort(job_cpus.begin(), job_cpus.end());
    std::cout << "pinned to cpu " << self << ", jobs on";
    for (const int cpu : job_cpus) {
      std::cout << ' ' << cpu;
    }
    std::cout << std::endl;
    serve(static_cast<std::uint16_t>(port_number), "worker", route);
  } catch (const std::exception& error) {
    std::cerr << error.what() << std::endl;
    return 1;
  }
}
