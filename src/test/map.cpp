#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

constexpr int kPort = 18121;

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

void send_all(int fd, const std::string& message) {
  std::size_t sent = 0;
  while (sent < message.size()) {
    const auto n = ::send(fd, message.data() + sent, message.size() - sent, MSG_NOSIGNAL);
    if (n < 0) {
      throw std::runtime_error("send failed");
    }
    sent += static_cast<std::size_t>(n);
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

std::string http_exchange(const std::string& request) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    throw std::runtime_error("socket failed");
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(kPort);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    ::close(fd);
    throw std::runtime_error("connect failed");
  }
  send_all(fd, request);
  const auto response = recv_all(fd);
  ::close(fd);
  return response;
}

std::string http_get(const std::string& target) {
  return http_exchange("GET " + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n");
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

std::string response_body(const std::string& response) {
  const auto at = response.find("\r\n\r\n");
  if (at == std::string::npos) {
    return {};
  }
  return response.substr(at + 4);
}

std::size_t count_of(const std::string& text, const std::string& needle) {
  std::size_t count = 0;
  for (std::size_t at = 0; (at = text.find(needle, at)) != std::string::npos; at += needle.size()) {
    ++count;
  }
  return count;
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

void require_status(const std::string& response, int status, const std::string& what) {
  if (response_status(response) != status) {
    throw std::runtime_error(what + ": " + response.substr(0, response.find("\r\n")));
  }
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
    if (ready.find("listening") == std::string::npos) {
      throw std::runtime_error("mercury did not start: " + ready);
    }

    const auto map = http_get("/map");
    require_status(map, 200, "map");
    const auto map_body = response_body(map);
    if (map_body.find("\"mercury_cpu\":0") == std::string::npos ||
        map_body.find("\"allocatable\":[1,2]") == std::string::npos ||
        map_body.find("\"level\":\"socket\"") == std::string::npos ||
        map_body.find("\"level\":\"node\"") == std::string::npos ||
        map_body.find("\"level\":\"l3\"") == std::string::npos ||
        map_body.find("\"level\":\"core\"") == std::string::npos ||
        map_body.find("\"daemons\":[]") == std::string::npos) {
      throw std::runtime_error("map did not describe the tree: " + map_body);
    }

    const auto rings = http_get("/rings?count=1");
    require_status(rings, 200, "rings");
    const auto rings_body = response_body(rings);
    if (rings_body.find("\"sample_ms\":10") == std::string::npos ||
        rings_body.find("\"capacity\":6000") == std::string::npos ||
        rings_body.find("\"busy\"") == std::string::npos ||
        rings_body.find("\"level\":\"core\"") == std::string::npos) {
      throw std::runtime_error("rings did not describe the groups: " + rings_body);
    }
    require_status(http_get("/rings?count=nope"), 400, "bad count");
    require_status(http_get("/rings?count=6001"), 400, "count past capacity");

    const std::string body = std::string("{\"daemon_id\":\"mapprobe\",\"exec_path\":\"") + json_escape(probe) +
                             "\",\"cpu\":1,\"listen_port\":18122}";
    const auto started = http_post("/start", body);
    require_status(started, 200, "start");

    const auto placed = response_body(http_get("/map"));
    if (count_of(placed, "mapprobe") != 5 || count_of(placed, "\"daemons\":[\"mapprobe\"]") != 4 ||
        placed.find("\"daemon_id\":\"mapprobe\"") == std::string::npos ||
        placed.find("\"cpus\":[1]") == std::string::npos) {
      throw std::runtime_error("map did not place mapprobe on cpu 1: " + placed);
    }

    require_status(http_post("/kill", R"({"daemon_id":"mapprobe"})"), 200, "kill");
    ::kill(-mercury, SIGTERM);
    ::waitpid(mercury, nullptr, 0);
    ::close(log_fd);
    std::cout << "map ok" << std::endl;
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
