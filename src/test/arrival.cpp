#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <limits.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <ctime>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr int kPort = 18081;
constexpr int kPings = 20;

pid_t g_server = -1;

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

std::int64_t monotonic_ns() {
  timespec now{};
  if (::clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    throw std::runtime_error("clock_gettime failed");
  }
  return static_cast<std::int64_t>(now.tv_sec) * 1000000000LL + now.tv_nsec;
}

std::string http_get(const std::string& target) {
  Socket sock{::socket(AF_INET, SOCK_STREAM, 0)};
  if (sock.get() < 0) {
    throw std::runtime_error("socket failed");
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(kPort);
  if (::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1) {
    throw std::runtime_error("invalid ip");
  }
  if (::connect(sock.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
    throw std::runtime_error("connect failed");
  }
  const std::string request = "GET " + target + " HTTP/1.1\r\nHost: 127.0.0.1:" +
                              std::to_string(kPort) + "\r\nConnection: close\r\n\r\n";
  send_all(sock.get(), request);
  const auto response = recv_all(sock.get());
  const auto body_at = response.find("\r\n\r\n");
  if (body_at == std::string::npos) {
    throw std::runtime_error("response is not http");
  }
  return response.substr(body_at + 4);
}

std::string run_capture(const std::string& program, const std::vector<std::string>& args) {
  int pipes[2] = {-1, -1};
  if (::pipe(pipes) != 0) {
    throw std::runtime_error("pipe failed");
  }
  const pid_t pid = ::fork();
  if (pid < 0) {
    ::close(pipes[0]);
    ::close(pipes[1]);
    throw std::runtime_error("fork failed");
  }
  if (pid == 0) {
    ::dup2(pipes[1], STDOUT_FILENO);
    ::close(pipes[0]);
    ::close(pipes[1]);
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(program.c_str()));
    for (const auto& arg : args) {
      argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);
    ::execv(program.c_str(), argv.data());
    _exit(127);
  }
  ::close(pipes[1]);
  std::string output;
  char buffer[256];
  while (true) {
    const auto n = ::read(pipes[0], buffer, sizeof(buffer));
    if (n == 0) {
      break;
    }
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    output.append(buffer, static_cast<std::size_t>(n));
  }
  ::close(pipes[0]);
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    throw std::runtime_error(program + " failed: " + output);
  }
  return output;
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
    timespec delay{};
    delay.tv_nsec = 50 * 1000000L;
    ::nanosleep(&delay, nullptr);
  }
  ::kill(-server, SIGKILL);
  while (::waitpid(server, nullptr, 0) < 0 && errno == EINTR) {
  }
}

void on_signal(int) {
  if (g_server > 0) {
    ::kill(-g_server, SIGKILL);
  }
  _exit(128);
}

pid_t start_server(const std::string& hello_server) {
  const pid_t pid = ::fork();
  if (pid < 0) {
    throw std::runtime_error("fork failed");
  }
  if (pid == 0) {
    if (::setpgid(0, 0) != 0) {
      _exit(127);
    }
    const auto port = std::to_string(kPort);
    ::execl(hello_server.c_str(), hello_server.c_str(), port.c_str(), nullptr);
    _exit(127);
  }
  ::setpgid(pid, pid);
  g_server = pid;
  return pid;
}

void wait_until_listening(pid_t server) {
  for (int attempt = 0; attempt < 50; ++attempt) {
    int status = 0;
    if (::waitpid(server, &status, WNOHANG) == server) {
      g_server = -1;
      throw std::runtime_error("hello_server exited before listening");
    }
    Socket sock{::socket(AF_INET, SOCK_STREAM, 0)};
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(kPort);
    ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (sock.get() >= 0 &&
        ::connect(sock.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
      return;
    }
    timespec delay{};
    delay.tv_nsec = 50 * 1000000L;
    ::nanosleep(&delay, nullptr);
  }
  throw std::runtime_error("hello_server did not listen");
}

std::uint64_t field_u64(const std::string& body, const std::string& key) {
  const auto at = body.find(key + " ");
  if (at == std::string::npos) {
    throw std::runtime_error("missing " + key);
  }
  return static_cast<std::uint64_t>(std::stoull(body.substr(at + key.size() + 1)));
}

double field_double(const std::string& body, const std::string& key) {
  const auto at = body.find(key + " ");
  if (at == std::string::npos) {
    throw std::runtime_error("missing " + key);
  }
  return std::stod(body.substr(at + key.size() + 1));
}

std::string exe_dir() {
  char buf[PATH_MAX];
  const auto n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n < 0) {
    throw std::runtime_error("readlink /proc/self/exe failed");
  }
  buf[n] = '\0';
  const std::string path(buf);
  const auto slash = path.rfind('/');
  if (slash == std::string::npos) {
    return ".";
  }
  if (slash == 0) {
    return "/";
  }
  return path.substr(0, slash);
}

}  // namespace

int main(int argc, char** argv) {
  struct sigaction action {};
  action.sa_handler = on_signal;
  ::sigaction(SIGINT, &action, nullptr);
  ::sigaction(SIGTERM, &action, nullptr);

  try {
    std::string hello_server;
    std::string ping;
    if (argc == 1) {
      const auto dir = exe_dir();
      hello_server = dir + "/hello_server";
      ping = dir + "/ping";
    } else if (argc == 3) {
      hello_server = argv[1];
      ping = argv[2];
    } else {
      throw std::runtime_error("usage: arrival_test [hello_server ping]");
    }
    const pid_t server = start_server(hello_server);
    wait_until_listening(server);

    const auto began = monotonic_ns();
    for (int i = 0; i < kPings; ++i) {
      const auto output = run_capture(ping, {"127.0.0.1", std::to_string(kPort)});
      if (output != "pong\n") {
        throw std::runtime_error("ping response: " + output);
      }
    }
    const auto client_elapsed_ns = monotonic_ns() - began;
    const auto body = http_get("/arrivals");
    const auto arrivals = field_u64(body, "arrivals");
    const auto elapsed_ns = field_u64(body, "elapsed_ns");
    const auto rate_per_s = field_double(body, "rate_per_s");
    const auto expected = static_cast<std::uint64_t>(kPings) + 1;
    if (arrivals != expected) {
      throw std::runtime_error("arrivals " + std::to_string(arrivals) + ", expected " +
                               std::to_string(expected));
    }
    if (elapsed_ns == 0 || rate_per_s <= 0) {
      throw std::runtime_error("arrival rate was not measured");
    }
    const double client_rate = static_cast<double>(kPings - 1) * 1e9 / static_cast<double>(client_elapsed_ns);
    if (rate_per_s < client_rate / 3.0 || rate_per_s > client_rate * 3.0) {
      throw std::runtime_error("server rate " + std::to_string(rate_per_s) + " disagrees with client rate " +
                               std::to_string(client_rate));
    }
    std::cout << "arrivals " << arrivals << " rate_per_s " << rate_per_s << std::endl;
    stop_server();
  } catch (const std::exception& error) {
    stop_server();
    std::cerr << error.what() << std::endl;
    return 1;
  }
}
