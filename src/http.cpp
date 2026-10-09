#include "http.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <iostream>
#include <sstream>
#include <stdexcept>
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

std::string trim(std::string value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
    value.pop_back();
  }
  std::size_t start = 0;
  while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
    ++start;
  }
  return value.substr(start);
}

std::string http_target(const std::string& request) {
  const auto line_end = request.find("\r\n");
  const auto line = request.substr(0, line_end == std::string::npos ? request.size() : line_end);
  std::istringstream first(line);
  std::string method;
  std::string target;
  first >> method >> target;
  return target;
}

bool is_ping(const std::string& request) {
  const auto line_end = request.find("\r\n");
  const auto line = request.substr(0, line_end == std::string::npos ? request.size() : line_end);
  if (trim(line) == "ping") {
    return true;
  }

  if (http_target(request) == "/ping") {
    return true;
  }

  const auto header_end = request.find("\r\n\r\n");
  if (header_end == std::string::npos) {
    return false;
  }
  return trim(request.substr(header_end + 4)) == "ping";
}

std::string read_request(int fd) {
  std::string request;
  char buffer[1024];
  while (request.find("\r\n\r\n") == std::string::npos) {
    const auto n = ::recv(fd, buffer, sizeof(buffer), 0);
    if (n == 0) {
      break;
    }
    if (n < 0) {
      throw std::runtime_error("recv failed");
    }
    request.append(buffer, static_cast<std::size_t>(n));
    if (request.size() > 65536) {
      throw std::runtime_error("request too large");
    }
  }

  const auto header_end = request.find("\r\n\r\n");
  if (header_end == std::string::npos) {
    return request;
  }

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
      throw std::runtime_error("send failed");
    }
    message.remove_prefix(static_cast<std::size_t>(n));
  }
}

Socket listen_on(std::uint16_t port) {
  Socket listener{::socket(AF_INET, SOCK_STREAM, 0)};
  if (listener.get() < 0) {
    throw std::runtime_error("socket failed");
  }
  const int reuse = 1;
  ::setsockopt(listener.get(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(listener.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
    throw std::runtime_error("bind failed");
  }
  if (::listen(listener.get(), 16) < 0) {
    throw std::runtime_error("listen failed");
  }
  return listener;
}

}  // namespace

std::string request_target(const std::string& request) { return http_target(request); }

std::string http_response(int status, std::string_view reason, std::string_view body,
                          std::string_view content_type) {
  std::ostringstream out;
  out << "HTTP/1.1 " << status << ' ' << reason << "\r\n"
      << "Content-Type: " << content_type << "\r\n"
      << "Content-Length: " << body.size() << "\r\n"
      << "Connection: close\r\n"
      << "\r\n"
      << body;
  return out.str();
}

std::optional<std::string> ping_route(const std::string& request) {
  if (!is_ping(request)) {
    return std::nullopt;
  }
  return http_response(200, "OK", "pong");
}

std::string basic_route(const std::string& request) {
  if (auto response = ping_route(request)) {
    return *response;
  }
  return http_response(404, "Not Found", "not found");
}

void serve(std::uint16_t port, std::string_view name, std::string (*route)(const std::string& request)) {
  Socket listener = listen_on(port);
  std::cout << name << " listening on 127.0.0.1:" << port << std::endl;

  while (true) {
    const int accepted = ::accept(listener.get(), nullptr, nullptr);
    if (accepted < 0) {
      throw std::runtime_error("accept failed");
    }
    Socket client{accepted};
    try {
      send_all(client.get(), route(read_request(client.get())));
    } catch (const std::exception& error) {
      std::cerr << error.what() << std::endl;
    }
  }
}
