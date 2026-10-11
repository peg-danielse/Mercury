#include "scale_stats.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kServerPort = 18090;
constexpr char kRequest[] = "w\n";

ScaleStats* g_stats = nullptr;

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

ScaleStats* map_stats(const char* path) {
  const int fd = ::open(path, O_RDWR);
  if (fd < 0) {
    throw std::runtime_error(std::string("open ") + path + " failed");
  }
  void* mapped = ::mmap(nullptr, sizeof(ScaleStats), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  ::close(fd);
  if (mapped == MAP_FAILED) {
    throw std::runtime_error("mmap failed");
  }
  return static_cast<ScaleStats*>(mapped);
}

std::atomic_ref<std::uint64_t> stat_ref(std::uint64_t& slot) { return std::atomic_ref<std::uint64_t>(slot); }

void wait_until(const char* path) {
  for (int i = 0; i < 2000; ++i) {
    if (::access(path, F_OK) == 0) {
      return;
    }
    sleep_ms(10);
  }
  throw std::runtime_error(std::string("timed out waiting for ") + path);
}

int connect_server() {
  for (int attempt = 0; attempt < 200; ++attempt) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
      sleep_ms(10);
      continue;
    }
    const int nodelay = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(kServerPort);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
      return fd;
    }
    ::close(fd);
    sleep_ms(10);
  }
  return -1;
}

bool send_request(int fd) {
  std::size_t sent = 0;
  while (sent < sizeof(kRequest) - 1) {
    const auto n = ::send(fd, kRequest + sent, sizeof(kRequest) - 1 - sent, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    sent += static_cast<std::size_t>(n);
  }
  return true;
}

bool recv_reply(int fd) {
  char buffer[64];
  std::string pending;
  while (pending.find('\n') == std::string::npos) {
    const auto n = ::recv(fd, buffer, sizeof(buffer), 0);
    if (n == 0) {
      return false;
    }
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    pending.append(buffer, static_cast<std::size_t>(n));
  }
  return true;
}

bool exchange(WorkerStats& slot, int fd) {
  if (!send_request(fd)) {
    stat_ref(slot.failed).fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  stat_ref(slot.sent).fetch_add(1, std::memory_order_relaxed);
  const auto began = monotonic_ns();
  const bool ok = recv_reply(fd);
  if (!ok) {
    stat_ref(slot.failed).fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  const auto service_ns = static_cast<std::uint64_t>(monotonic_ns() - began);
  stat_ref(slot.service_ns).fetch_add(service_ns, std::memory_order_relaxed);
  stat_ref(slot.completed).fetch_add(1, std::memory_order_relaxed);
  if (service_ns > static_cast<std::uint64_t>(kResponseDeadlineNs)) {
    stat_ref(slot.late).fetch_add(1, std::memory_order_relaxed);
  }
  return true;
}

void worker(int index) {
  WorkerStats& slot = g_stats->workers[index];
  while (true) {
    const int fd = connect_server();
    if (fd < 0) {
      return;
    }
    while (exchange(slot, fd)) {
    }
    ::close(fd);
  }
}

}  // namespace

int main() {
  try {
    const char* stats = std::getenv("SCALE_STATS");
    const char* go = std::getenv("SCALE_GO");
    if (stats == nullptr || go == nullptr || stats[0] == '\0' || go[0] == '\0') {
      throw std::runtime_error("SCALE_STATS and SCALE_GO are required");
    }
    g_stats = map_stats(stats);
    wait_until(go);
    std::vector<std::thread> threads;
    threads.reserve(kScaleWorkers);
    for (int i = 0; i < kScaleWorkers; ++i) {
      threads.emplace_back(worker, i);
    }
    for (auto& thread : threads) {
      thread.join();
    }
  } catch (const std::exception& error) {
    std::cerr << error.what() << std::endl;
    return 1;
  }
}
