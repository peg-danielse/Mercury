#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <chrono>
#include <iostream>
#include <thread>
#include <stdexcept>
#include <string>

namespace {

constexpr int kPort = 18111;

std::string dirname_of(const std::string& path) {
  const auto slash = path.find_last_of('/');
  if (slash == std::string::npos) {
    return ".";
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

std::string recv_all(int fd) {
  std::string response;
  char buffer[1024];
  while (true) {
    const auto n = ::recv(fd, buffer, sizeof(buffer), 0);
    if (n == 0) {
      break;
    }
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    response.append(buffer, static_cast<std::size_t>(n));
  }
  return response;
}

std::string http_exchange(const std::string& request) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    throw std::runtime_error("socket failed");
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(kPort);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
    ::close(fd);
    throw std::runtime_error("connect failed");
  }
  std::size_t sent = 0;
  while (sent < request.size()) {
    const auto n = ::send(fd, request.data() + sent, request.size() - sent, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      ::close(fd);
      throw std::runtime_error("send failed");
    }
    sent += static_cast<std::size_t>(n);
  }
  const auto response = recv_all(fd);
  ::close(fd);
  return response;
}

std::string http_post(const std::string& target, const std::string& body) {
  const std::string request = "POST " + target +
                              " HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: " +
                              std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
  return http_exchange(request);
}

int response_status(const std::string& response) {
  const auto line = response.find("\r\n");
  const auto head = response.substr(0, line == std::string::npos ? response.size() : line);
  const auto space = head.find(' ');
  if (space == std::string::npos) {
    return 0;
  }
  return std::stoi(head.substr(space + 1));
}

int response_pid(const std::string& response) {
  const auto key = response.find("\"pid\"");
  if (key == std::string::npos) {
    return -1;
  }
  return std::stoi(response.substr(response.find(':', key) + 1));
}

std::string status_field(pid_t pid, const std::string& key) {
  const auto path = "/proc/" + std::to_string(pid) + "/status";
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return {};
  }
  std::string text;
  char buffer[4096];
  while (true) {
    const auto n = ::read(fd, buffer, sizeof(buffer));
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if (n == 0) {
      break;
    }
    text.append(buffer, static_cast<std::size_t>(n));
  }
  ::close(fd);
  const std::string prefix = key + ":";
  std::size_t at = 0;
  while (at < text.size()) {
    const auto end = text.find('\n', at);
    const auto line = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
    if (line.compare(0, prefix.size(), prefix) == 0) {
      std::size_t value = prefix.size();
      while (value < line.size() && (line[value] == ' ' || line[value] == '\t')) {
        ++value;
      }
      return line.substr(value);
    }
    if (end == std::string::npos) {
      break;
    }
    at = end + 1;
  }
  return {};
}

std::string wait_for(int fd, const std::string& needle, int timeout_ms) {
  std::string log;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 100000;
    const auto ready = ::select(fd + 1, &set, nullptr, nullptr, &tv);
    if (ready > 0) {
      char buffer[1024];
      const auto n = ::read(fd, buffer, sizeof(buffer));
      if (n > 0) {
        log.append(buffer, static_cast<std::size_t>(n));
        if (log.find(needle) != std::string::npos) {
          return log;
        }
      }
    }
  }
  return log;
}

}  // namespace

int main() {
  pid_t mercury = -1;
  int log_fd = -1;
  try {
    const auto bindir = dirname_of(exe_dir());
    const auto mercury_bin = bindir + "/mercury";
    const auto probe = exe_dir() + "/env_probe";
    int pipes[2] = {-1, -1};
    if (::pipe(pipes) != 0) {
      throw std::runtime_error("pipe failed");
    }
    mercury = ::fork();
    if (mercury < 0) {
      throw std::runtime_error("fork failed");
    }
    if (mercury == 0) {
      ::setpgid(0, 0);
      ::dup2(pipes[1], STDOUT_FILENO);
      ::dup2(pipes[1], STDERR_FILENO);
      ::close(pipes[0]);
      ::close(pipes[1]);
      ::execl(mercury_bin.c_str(), mercury_bin.c_str(), std::to_string(kPort).c_str(), "0", "1", "2", nullptr);
      _exit(127);
    }
    ::setpgid(mercury, mercury);
    ::close(pipes[1]);
    log_fd = pipes[0];
    const auto ready = wait_for(log_fd, "listening", 3000);
    if (ready.find("pinned to cpu 0, daemons on 1 2") == std::string::npos ||
        ready.find("listening") == std::string::npos) {
      throw std::runtime_error("mercury did not start: " + ready);
    }

    const std::string bad = std::string("{\"daemon_id\":\"envprobe\",\"exec_path\":\"") + json_escape(probe) +
                            "\",\"cpu\":1,\"env\":{\"bad-name\":\"x\"}}";
    const auto rejected = http_post("/start", bad);
    if (response_status(rejected) != 400) {
      throw std::runtime_error("bad env name was accepted: " + rejected);
    }

    const std::string body = std::string("{\"daemon_id\":\"envprobe\",\"exec_path\":\"") + json_escape(probe) +
                             "\",\"cpu\":1,\"listen_port\":18112,\"env\":{\"MERCURY_TEST_ENV\":\"from-request\","
                             "\"PATH\":\"/only-this\"}}";
    ::rmdir("/sys/fs/cgroup/mercury/envprobe");
    const auto started = http_post("/start", body);
    const int first = response_pid(started);
    if (response_status(started) != 200 || first <= 0) {
      throw std::runtime_error("start failed: " + started);
    }

    const auto migrated = http_post("/migrate", R"({"daemon_id":"envprobe","cpu":2})");
    const int second = response_pid(migrated);
    if (response_status(migrated) != 200 || second != first) {
      throw std::runtime_error("migrate failed: " + migrated);
    }
    bool on_cpu = false;
    for (int attempt = 0; attempt < 50; ++attempt) {
      const auto state = status_field(first, "State");
      if (status_field(first, "Name") == "env_probe" && !state.empty() && state.front() == 'S' &&
          status_field(first, "Cpus_allowed_list") == "2") {
        on_cpu = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!on_cpu) {
      throw std::runtime_error("migrate left pid " + std::to_string(first) + " on cpus " +
                               status_field(first, "Cpus_allowed_list"));
    }

    const auto killed = http_post("/kill", R"({"daemon_id":"envprobe"})");
    if (response_status(killed) != 200 || killed.find(R"("ok":true)") == std::string::npos) {
      throw std::runtime_error("kill failed: " + killed);
    }
    const auto first_log = wait_for(log_fd, "MERCURY_TEST_ENV=from-request", 3000);
    if (first_log.find("MERCURY_TEST_ENV=from-request") == std::string::npos ||
        first_log.find("PATH=/only-this") == std::string::npos) {
      throw std::runtime_error("started process did not receive env:\n" + first_log);
    }

    ::kill(-mercury, SIGTERM);
    ::waitpid(mercury, nullptr, 0);
    ::close(log_fd);
    std::cout << "env ok" << std::endl;
  } catch (const std::exception& error) {
    if (mercury > 0) {
      ::kill(-mercury, SIGKILL);
      ::waitpid(mercury, nullptr, 0);
    }
    if (log_fd >= 0) {
      ::close(log_fd);
    }
    std::cerr << error.what() << std::endl;
    return 1;
  }
}
