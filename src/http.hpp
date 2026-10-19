#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

std::string http_response(int status, std::string_view reason, std::string_view body,
                          std::string_view content_type = "text/plain");

std::string request_target(const std::string& request);

// Answers "ping" and GET /ping with "pong". Empty when the request is not a ping.
std::optional<std::string> ping_route(const std::string& request);

// Bind 127.0.0.1:port and answer one connection at a time with route, on this thread. Never returns.
void serve(std::uint16_t port, std::string_view name, std::string (*route)(const std::string& request));
