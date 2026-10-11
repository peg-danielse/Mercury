#include "http.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

struct Request {
  int fd = -1;
  std::string (*route)(const std::string&) = nullptr;
  Arrivals* arrivals = nullptr;
};

void* run_request(void* arg) {
  const std::unique_ptr<Request> request(static_cast<Request*>(arg));
  handle_client(request->fd, request->route, request->arrivals);
  return nullptr;
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

// Each accepted connection is a goroutine: a new pthread runs it, then exits.
void accept_loop(int listen_fd) {
  Arrivals arrivals;
  while (true) {
    const int accepted = ::accept(listen_fd, nullptr, nullptr);
    if (accepted < 0) {
      throw std::runtime_error("accept failed");
    }
    auto request = std::make_unique<Request>();
    request->fd = accepted;
    request->route = basic_route;
    request->arrivals = &arrivals;

    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) {
      handle_client(accepted, basic_route, &arrivals);
      continue;
    }
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    const int created = pthread_create(&thread, &attr, run_request, request.get());
    pthread_attr_destroy(&attr);
    if (created != 0) {
      handle_client(accepted, basic_route, &arrivals);
      continue;
    }
    request.release();
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
