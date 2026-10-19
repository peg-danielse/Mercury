#include "http.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <ctime>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

volatile std::uint64_t g_sink = 0;

// A thread per connection is a perf event each, and that exhausts the process
// file limit. A fixed pool keeps one long-lived thread per worker.
constexpr int kWorkers = 32;

struct Worker {
  std::mutex mu;
  std::condition_variable cv;
  std::vector<int> incoming;
};

std::int64_t monotonic_ns() {
  timespec now{};
  if (::clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    return 0;
  }
  return static_cast<std::int64_t>(now.tv_sec) * 1000000000LL + now.tv_nsec;
}

void burn_cpu() {
  std::uint64_t x = 0x9E3779B97F4A7C15ULL;
  for (int i = 0; i < 20000000; ++i) {
    x = x * 6364136223846793005ULL + 1;
  }
  g_sink = x;
}

// Optional component for the degrade policy (policies.md "Application"). When MERCURY_POLICY names a
// policy file, /cpu does an extra half burn while component "extra" is enabled. Without the variable
// /cpu is unchanged. The file is re-read at most every 200 ms.
bool extra_enabled() {
  static const char* path = std::getenv("MERCURY_POLICY");
  if (path == nullptr || path[0] == '\0') {
    return false;
  }
  static std::mutex mu;
  static std::int64_t read_ns = 0;
  static bool enabled = true;
  std::lock_guard lock(mu);
  const auto now = monotonic_ns();
  if (now - read_ns > 200000000LL) {
    read_ns = now;
    std::ifstream file(path);
    std::stringstream text;
    text << file.rdbuf();
    const auto body = text.str();
    const auto at = body.find("\"name\":\"extra\"");
    if (at != std::string::npos) {
      constexpr std::string_view off = "\"name\":\"extra\",\"enabled\":false";
      enabled = body.compare(at, off.size(), off) != 0;
    } else {
      enabled = true;
    }
  }
  return enabled;
}

void burn_extra() {
  std::uint64_t x = 0x9E3779B97F4A7C15ULL;
  for (int i = 0; i < 10000000; ++i) {
    x = x * 6364136223846793005ULL + 1;
  }
  g_sink = x;
}

std::uint32_t random_ns() {
  static thread_local std::uint32_t state = static_cast<std::uint32_t>(monotonic_ns()) ^
                                             static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(pthread_self()));
  if (state == 0) {
    state = 0xA5A5A5A5u;
  }
  state = state * 1664525u + 1013904223u;
  return (state % 10000000u) + 1;
}

std::string keep_alive(std::string_view body) {
  std::ostringstream out;
  out << "HTTP/1.1 200 OK\r\n"
      << "Content-Type: text/plain\r\n"
      << "Content-Length: " << body.size() << "\r\n"
      << "Connection: keep-alive\r\n"
      << "\r\n"
      << body;
  return out.str();
}

std::string arrivals_body(std::uint64_t arrivals, std::int64_t first_ns, std::int64_t last_ns) {
  const auto elapsed_ns = arrivals > 1 ? last_ns - first_ns : 0;
  double rate_per_s = 0;
  if (elapsed_ns > 0) {
    rate_per_s = static_cast<double>(arrivals - 1) * 1e9 / static_cast<double>(elapsed_ns);
  }
  std::ostringstream out;
  out << "arrivals " << arrivals << "\n"
      << "elapsed_ns " << elapsed_ns << "\n"
      << "rate_per_s " << rate_per_s << "\n";
  return out.str();
}

bool connection_close(const std::string& request) {
  const auto header_end = request.find("\r\n\r\n");
  const auto headers = request.substr(0, header_end == std::string::npos ? request.size() : header_end);
  std::string lower = headers;
  for (char& c : lower) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return lower.find("connection: close") != std::string::npos;
}

std::string read_http(int fd) {
  std::string request;
  char buffer[1024];
  while (request.find("\r\n\r\n") == std::string::npos) {
    const auto n = ::recv(fd, buffer, sizeof(buffer), 0);
    if (n == 0) {
      return request;
    }
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw std::runtime_error("recv failed");
    }
    request.append(buffer, static_cast<std::size_t>(n));
    if (request.size() > 65536) {
      throw std::runtime_error("request too large");
    }
  }
  const auto header_end = request.find("\r\n\r\n");
  std::size_t content_length = 0;
  const auto headers = request.substr(0, header_end);
  const auto key = headers.find("Content-Length:");
  if (key != std::string::npos) {
    content_length = static_cast<std::size_t>(std::stoul(headers.substr(key + 15)));
  }
  const auto body_at = header_end + 4;
  while (request.size() - body_at < content_length) {
    const auto n = ::recv(fd, buffer, sizeof(buffer), 0);
    if (n <= 0) {
      break;
    }
    request.append(buffer, static_cast<std::size_t>(n));
  }
  return request;
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

std::string dispatch(const std::string& request, Arrivals* arrivals) {
  std::uint64_t count = 0;
  std::int64_t first_ns = 0;
  std::int64_t last_ns = 0;
  {
    std::lock_guard lock(arrivals->mu);
    const auto now = monotonic_ns();
    if (arrivals->count == 0) {
      arrivals->first_ns = now;
    }
    arrivals->last_ns = now;
    count = ++arrivals->count;
    first_ns = arrivals->first_ns;
    last_ns = arrivals->last_ns;
  }
  const auto target = request_target(request);
  if (target == "/arrivals") {
    return http_response(200, "OK", arrivals_body(count, first_ns, last_ns));
  }
  if (target == "/cpu") {
    burn_cpu();
    if (extra_enabled()) {
      burn_extra();
      return keep_alive("cpu extra\n");
    }
    return keep_alive("cpu\n");
  }
  if (target == "/sleep") {
    const auto ns = random_ns();
    timespec delay{};
    delay.tv_nsec = static_cast<long>(ns);
    while (::nanosleep(&delay, &delay) != 0) {
    }
    return keep_alive("slept " + std::to_string(ns) + "\n");
  }
  return basic_route(request);
}

void serve_connection(int fd, Arrivals* arrivals) {
  const int nodelay = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
  try {
    while (true) {
      const auto request = read_http(fd);
      if (request_target(request).empty()) {
        return;
      }
      const bool close_after = connection_close(request);
      const auto response = dispatch(request, arrivals);
      send_all(fd, response);
      if (close_after) {
        return;
      }
    }
  } catch (const std::exception& error) {
    std::cerr << error.what() << std::endl;
  }
}

void worker_main(Worker* worker, Arrivals* arrivals) {
  while (true) {
    int fd = -1;
    {
      std::unique_lock lock(worker->mu);
      worker->cv.wait(lock, [&] { return !worker->incoming.empty(); });
      fd = worker->incoming.back();
      worker->incoming.pop_back();
    }
    serve_connection(fd, arrivals);
    ::close(fd);
  }
}

void announce(int fd) {
  sockaddr_in bound{};
  socklen_t bound_len = sizeof(bound);
  std::uint16_t port = 0;
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &bound_len) == 0) {
    port = ntohs(bound.sin_port);
  }
  std::cout << "hello_server listening on 127.0.0.1:" << port << std::endl;
}

// /cpu burns on the worker that owns the connection, and /sleep pauses that same worker.
void accept_loop(int listen_fd) {
  Arrivals arrivals;
  Worker workers[kWorkers];
  for (auto& worker : workers) {
    std::thread(worker_main, &worker, &arrivals).detach();
  }
  unsigned next = 0;
  while (true) {
    const int accepted = ::accept(listen_fd, nullptr, nullptr);
    if (accepted < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw std::runtime_error("accept failed");
    }
    Worker& worker = workers[next % static_cast<unsigned>(kWorkers)];
    ++next;
    {
      std::lock_guard lock(worker.mu);
      worker.incoming.push_back(accepted);
    }
    worker.cv.notify_one();
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc == 3 && std::strcmp(argv[1], "--listen-fd") == 0) {
      const int fd = std::stoi(argv[2]);
      announce(fd);
      accept_loop(fd);
      return 0;
    }
    const auto port = static_cast<std::uint16_t>(argc > 1 ? std::stoi(argv[1]) : 8080);
    const int fd = listen_loopback(port);
    announce(fd);
    accept_loop(fd);
  } catch (const std::exception& error) {
    std::cerr << error.what() << std::endl;
    return 1;
  }
}
