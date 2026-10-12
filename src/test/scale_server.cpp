#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kBurn = 80000;
constexpr int kBacklog = 256;
// The scale test grants up to 32 cores. One long-lived worker can fill one
// core. A thread per connection is a perf group each and exhausts the
// process file limit.
constexpr int kWorkers = 32;

volatile std::uint64_t g_sink = 0;

void burn() {
  std::uint64_t x = 0x9E3779B97F4A7C15ULL;
  for (int i = 0; i < kBurn; ++i) {
    x = x * 6364136223846793005ULL + 1;
  }
  g_sink = x;
}

void send_all(int fd, const char* data, std::size_t size) {
  while (size > 0) {
    const auto n = ::send(fd, data, size, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return;
    }
    data += n;
    size -= static_cast<std::size_t>(n);
  }
}

struct Conn {
  int fd = -1;
  std::string pending;
};

struct Worker {
  std::mutex mu;
  std::condition_variable cv;
  std::vector<int> incoming;
};

Worker g_workers[kWorkers];

void take_new(Worker& worker, std::vector<Conn>& conns) {
  std::vector<int> fresh;
  {
    std::lock_guard lock(worker.mu);
    fresh.swap(worker.incoming);
  }
  for (const int fd : fresh) {
    const int nodelay = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    conns.push_back(Conn{fd, {}});
  }
}

// False when the peer is gone. Reads whatever is already queued.
bool pump(Conn& conn) {
  char buffer[1024];
  while (true) {
    const auto n = ::recv(conn.fd, buffer, sizeof(buffer), MSG_DONTWAIT);
    if (n == 0) {
      return false;
    }
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }
      return false;
    }
    conn.pending.append(buffer, static_cast<std::size_t>(n));
  }
  for (;;) {
    const auto nl = conn.pending.find('\n');
    if (nl == std::string::npos) {
      break;
    }
    conn.pending.erase(0, nl + 1);
    burn();
    const char reply[] = "ok\n";
    send_all(conn.fd, reply, sizeof(reply) - 1);
  }
  return true;
}

void worker_main(Worker* worker) {
  std::vector<Conn> conns;
  while (true) {
    take_new(*worker, conns);
    if (conns.empty()) {
      std::unique_lock lock(worker->mu);
      worker->cv.wait(lock, [&] { return !worker->incoming.empty(); });
      continue;
    }
    std::vector<pollfd> pfds(conns.size());
    for (std::size_t i = 0; i < conns.size(); ++i) {
      pfds[i].fd = conns[i].fd;
      pfds[i].events = POLLIN;
    }
    const int ready = ::poll(pfds.data(), static_cast<nfds_t>(pfds.size()), 50);
    if (ready <= 0) {
      continue;
    }
    std::vector<Conn> kept;
    kept.reserve(conns.size());
    for (std::size_t i = 0; i < conns.size(); ++i) {
      if (pfds[i].revents == 0) {
        kept.push_back(std::move(conns[i]));
        continue;
      }
      if (!pump(conns[i])) {
        ::close(conns[i].fd);
        continue;
      }
      kept.push_back(std::move(conns[i]));
    }
    conns.swap(kept);
  }
}

void assign(int fd) {
  static std::atomic<unsigned> next{0};
  Worker& worker = g_workers[next.fetch_add(1, std::memory_order_relaxed) % static_cast<unsigned>(kWorkers)];
  {
    std::lock_guard lock(worker.mu);
    worker.incoming.push_back(fd);
  }
  worker.cv.notify_one();
}

int listen_on(std::uint16_t port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    throw std::runtime_error("socket failed");
  }
  const int reuse = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 || ::listen(fd, kBacklog) < 0) {
    ::close(fd);
    throw std::runtime_error("listen failed");
  }
  return fd;
}

void accept_loop(int listen_fd) {
  for (auto& worker : g_workers) {
    std::thread(worker_main, &worker).detach();
  }
  while (true) {
    const int accepted = ::accept(listen_fd, nullptr, nullptr);
    if (accepted < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw std::runtime_error("accept failed");
    }
    assign(accepted);
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
    int listen_fd = -1;
    if (argc == 3 && std::strcmp(argv[1], "--listen-fd") == 0) {
      listen_fd = std::stoi(argv[2]);
    } else {
      const auto port = static_cast<std::uint16_t>(argc > 1 ? std::stoi(argv[1]) : 18090);
      listen_fd = listen_on(port);
    }
    std::cout << "scale_server listening" << std::endl;
    accept_loop(listen_fd);
  } catch (const std::exception& error) {
    std::cerr << error.what() << std::endl;
    return 1;
  }
}
