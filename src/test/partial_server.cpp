#include "http.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

volatile std::uint64_t g_sink = 0;

// A request burns for 33% of the slice and sleeps the rest, so one connection
// takes 33% of a core instead of a whole one. PARTIAL_BURN_PCT (1-100) overrides
// the 33; 100 makes each connection a full core.
constexpr int kWorkers = 48;
constexpr std::int64_t kSliceNs = 2000000;
std::int64_t burn_pct() {
  const char* text = std::getenv("PARTIAL_BURN_PCT");
  const int pct = text != nullptr ? std::atoi(text) : 33;
  return pct >= 1 && pct <= 100 ? pct : 33;
}
const std::int64_t kBurnNs = kSliceNs * burn_pct() / 100;

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

void duty() {
  const auto start = monotonic_ns();
  std::uint64_t x = 0x9E3779B97F4A7C15ULL;
  while (monotonic_ns() - start < kBurnNs) {
    x = x * 6364136223846793005ULL + 1;
  }
  g_sink = x;
  if (kBurnNs >= kSliceNs) {
    return;
  }
  timespec delay{};
  delay.tv_nsec = static_cast<long>(kSliceNs - kBurnNs);
  while (::nanosleep(&delay, &delay) != 0) {
  }
}

std::string read_request(int fd) {
  std::string request;
  char buffer[1024];
  while (request.find("\r\n\r\n") == std::string::npos) {
    const auto n = ::recv(fd, buffer, sizeof(buffer), 0);
    if (n == 0) {
      return {};
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

void serve_connection(int fd) {
  const int nodelay = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
  try {
    while (true) {
      if (read_request(fd).empty()) {
        break;
      }
      duty();
      send_all(fd, http_response(200, "OK", "ok\n"));
    }
  } catch (const std::exception&) {
  }
  ::close(fd);
}

void announce(int fd) {
  sockaddr_in bound{};
  socklen_t bound_len = sizeof(bound);
  std::uint16_t port = 0;
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &bound_len) == 0) {
    port = ntohs(bound.sin_port);
  }
  std::cout << "partial_server listening on 127.0.0.1:" << port << " duty=0.33" << std::endl;
}

void worker_main(Worker* worker) {
  while (true) {
    int fd = -1;
    {
      std::unique_lock lock(worker->mu);
      worker->cv.wait(lock, [&] { return !worker->incoming.empty(); });
      fd = worker->incoming.back();
      worker->incoming.pop_back();
    }
    serve_connection(fd);
  }
}

void accept_loop(int listen_fd) {
  Worker workers[kWorkers];
  for (auto& worker : workers) {
    std::thread(worker_main, &worker).detach();
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
    const auto port = static_cast<std::uint16_t>(argc > 1 ? std::stoi(argv[1]) : 18132);
    const int fd = listen_loopback(port);
    announce(fd);
    accept_loop(fd);
  } catch (const std::exception& error) {
    std::cerr << error.what() << std::endl;
    return 1;
  }
}
