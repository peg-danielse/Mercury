#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

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
  Socket(Socket&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }

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
      throw std::runtime_error("recv failed");
    }
    response.append(buffer, static_cast<std::size_t>(n));
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: ping <ip> <port>\n";
    return 1;
  }

  try {
    const std::string ip = argv[1];
    const int port_number = std::stoi(argv[2]);
    if (port_number < 1 || port_number > 65535) {
      throw std::runtime_error("invalid port");
    }
    const auto port = static_cast<std::uint16_t>(port_number);

    Socket sock{::socket(AF_INET, SOCK_STREAM, 0)};
    if (sock.get() < 0) {
      throw std::runtime_error("socket failed");
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (::inet_pton(AF_INET, ip.c_str(), &address.sin_addr) != 1) {
      throw std::runtime_error("invalid ip");
    }
    if (::connect(sock.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
      throw std::runtime_error("connect failed");
    }

    const std::string request = "GET /ping HTTP/1.1\r\nHost: " + ip + ":" + std::to_string(port) +
                                "\r\nConnection: close\r\n\r\n";
    send_all(sock.get(), request);

    const auto response = recv_all(sock.get());
    const auto body_at = response.find("\r\n\r\n");
    if (body_at == std::string::npos) {
      throw std::runtime_error("response is not http");
    }
    std::cout << response.substr(body_at + 4) << '\n';
  } catch (const std::exception& error) {
    std::cerr << error.what() << std::endl;
    return 1;
  }
}
