#include "http.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
  try {
    const auto port = static_cast<std::uint16_t>(argc > 1 ? std::stoi(argv[1]) : 8080);
    serve(port, "hello_server", basic_route);
  } catch (const std::exception& error) {
    std::cerr << error.what() << std::endl;
    return 1;
  }
}
