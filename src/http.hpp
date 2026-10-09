#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

std::string http_response(int status, std::string_view reason, std::string_view body,
                          std::string_view content_type = "text/plain");

std::string request_target(const std::string& request);

// The ping route shared with hello_server. Empty when the request is not a ping.
std::optional<std::string> ping_route(const std::string& request);

std::string basic_route(const std::string& request);

void serve(std::uint16_t port, std::string_view name, std::string (*route)(const std::string& request));
