#include "http.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <ctime>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
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
  int release() {
    const int fd = fd_;
    fd_ = -1;
    return fd;
  }

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

bool wait_fd(int fd, short events, int timeout_ms) {
  pollfd ready{};
  ready.fd = fd;
  ready.events = events;
  while (true) {
    const auto n = ::poll(&ready, 1, timeout_ms);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    return n > 0;
  }
}

std::optional<std::string> ping_reply(std::uint16_t port) {
  Socket sock{::socket(AF_INET, SOCK_STREAM, 0)};
  if (sock.get() < 0) {
    return std::nullopt;
  }
  const int flags = ::fcntl(sock.get(), F_GETFL, 0);
  if (flags < 0 || ::fcntl(sock.get(), F_SETFL, flags | O_NONBLOCK) != 0) {
    return std::nullopt;
  }

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  const int connected = ::connect(sock.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address));
  if (connected < 0 && errno != EINPROGRESS) {
    return std::nullopt;
  }
  if (connected < 0 && !wait_fd(sock.get(), POLLOUT, 2000)) {
    return std::nullopt;
  }
  int error = 0;
  socklen_t error_len = sizeof(error);
  if (::getsockopt(sock.get(), SOL_SOCKET, SO_ERROR, &error, &error_len) < 0 || error != 0) {
    return std::nullopt;
  }
  if (::fcntl(sock.get(), F_SETFL, flags) != 0) {
    return std::nullopt;
  }
  timeval timeout{};
  timeout.tv_sec = 2;
  ::setsockopt(sock.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  ::setsockopt(sock.get(), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

  const std::string request = "GET /ping HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
                              "\r\nConnection: close\r\n\r\n";
  try {
    send_all(sock.get(), request);
  } catch (const std::exception&) {
    return std::nullopt;
  }

  std::string response;
  char buffer[1024];
  while (response.size() <= 65536) {
    const auto n = ::recv(sock.get(), buffer, sizeof(buffer), 0);
    if (n == 0) {
      break;
    }
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      if ((errno == EAGAIN || errno == EWOULDBLOCK) && response.find("\r\n\r\n") != std::string::npos) {
        break;
      }
      return std::nullopt;
    }
    response.append(buffer, static_cast<std::size_t>(n));
  }
  const auto body_at = response.find("\r\n\r\n");
  if (body_at == std::string::npos) {
    return std::nullopt;
  }
  return trim(response.substr(body_at + 4));
}

std::optional<std::string> relay_route(const std::string& request) {
  const auto target = http_target(request);
  constexpr std::string_view prefix = "/relay/";
  if (target.rfind(prefix, 0) != 0) {
    return std::nullopt;
  }
  const auto port_text = target.substr(prefix.size());
  int port_number = 0;
  try {
    std::size_t used = 0;
    port_number = std::stoi(port_text, &used);
    if (used != port_text.size() || port_number < 1 || port_number > 65535) {
      return http_response(400, "Bad Request", "failure");
    }
  } catch (const std::exception&) {
    return http_response(400, "Bad Request", "failure");
  }
  const auto reply = ping_reply(static_cast<std::uint16_t>(port_number));
  if (reply && *reply == "pong") {
    return http_response(200, "OK", "success");
  }
  return http_response(502, "Bad Gateway", "failure");
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
  if (auto response = relay_route(request)) {
    return *response;
  }
  return http_response(404, "Not Found", "not found");
}

std::int64_t monotonic_ns() {
  timespec now{};
  if (::clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    return 0;
  }
  return static_cast<std::int64_t>(now.tv_sec) * 1000000000LL + now.tv_nsec;
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

void handle_client(int fd, std::string (*route)(const std::string& request), Arrivals* arrivals) {
  Socket client{fd};
  try {
    const auto request = read_request(client.get());
    if (request_target(request).empty()) {
      return;
    }
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
    const auto response = request_target(request) == "/arrivals"
                              ? http_response(200, "OK", arrivals_body(count, first_ns, last_ns))
                              : route(request);
    send_all(client.get(), response);
  } catch (const std::exception& error) {
    std::cerr << error.what() << std::endl;
  }
}

int listen_loopback(std::uint16_t port) {
  Socket listener = listen_on(port);
  return listener.release();
}

void serve_fd(int fd, std::string_view name, std::string (*route)(const std::string& request)) {
  sockaddr_in bound{};
  socklen_t bound_len = sizeof(bound);
  std::uint16_t port = 0;
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &bound_len) == 0) {
    port = ntohs(bound.sin_port);
  }
  std::cout << name << " listening on 127.0.0.1:" << port << std::endl;

  Arrivals arrivals;
  while (true) {
    const int accepted = ::accept(fd, nullptr, nullptr);
    if (accepted < 0) {
      throw std::runtime_error("accept failed");
    }
    handle_client(accepted, route, &arrivals);
  }
}

void serve(std::uint16_t port, std::string_view name, std::string (*route)(const std::string& request)) {
  Socket listener = listen_on(port);
  serve_fd(listener.get(), name, route);
}
