#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

constexpr int kBurn = 80000;
constexpr int kBacklog = 256;

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

void serve_connection(int fd) {
  const int nodelay = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
  std::string pending;
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
    pending.append(buffer, static_cast<std::size_t>(n));
    for (;;) {
      const auto nl = pending.find('\n');
      if (nl == std::string::npos) {
        break;
      }
      pending.erase(0, nl + 1);
      burn();
      const char reply[] = "ok\n";
      send_all(fd, reply, sizeof(reply) - 1);
    }
  }
  ::close(fd);
}

void* connection_main(void* arg) {
  const int fd = static_cast<int>(reinterpret_cast<std::uintptr_t>(arg));
  serve_connection(fd);
  return nullptr;
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
  while (true) {
    const int accepted = ::accept(listen_fd, nullptr, nullptr);
    if (accepted < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw std::runtime_error("accept failed");
    }
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) {
      serve_connection(accepted);
      continue;
    }
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    const int created =
        pthread_create(&thread, &attr, connection_main, reinterpret_cast<void*>(static_cast<std::uintptr_t>(accepted)));
    pthread_attr_destroy(&attr);
    if (created != 0) {
      serve_connection(accepted);
    }
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
