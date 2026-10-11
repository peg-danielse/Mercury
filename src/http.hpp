#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

std::string http_response(int status, std::string_view reason, std::string_view body,
                          std::string_view content_type = "text/plain");

std::string request_target(const std::string& request);

// The ping route shared with hello_server. Empty when the request is not a ping.
std::optional<std::string> ping_route(const std::string& request);

std::string basic_route(const std::string& request);

struct Arrivals {
  std::mutex mu;
  std::uint64_t count = 0;
  std::int64_t first_ns = 0;
  std::int64_t last_ns = 0;
};

// Read one connection, run route, and write the response. Closes fd.
void handle_client(int fd, std::string (*route)(const std::string& request), Arrivals* arrivals);

// Bind and listen on 127.0.0.1. The caller owns the returned fd.
int listen_loopback(std::uint16_t port);

void serve(std::uint16_t port, std::string_view name, std::string (*route)(const std::string& request));

// Accept on an already listening socket. The caller keeps the fd open.
void serve_fd(int fd, std::string_view name, std::string (*route)(const std::string& request));
