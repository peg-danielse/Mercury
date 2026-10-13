#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <iostream>

// Prints two variables and, when Mercury passed a listen socket, waits to be migrated or killed.
int main(int argc, char** argv) {
  const char* value = std::getenv("MERCURY_TEST_ENV");
  const char* path = std::getenv("PATH");
  std::cout << "MERCURY_TEST_ENV=" << (value != nullptr ? value : "") << "\n";
  std::cout << "PATH=" << (path != nullptr ? path : "") << "\n";
  std::cout.flush();
  if (argc == 3 && std::strcmp(argv[1], "--listen-fd") == 0) {
    for (;;) {
      ::pause();
    }
  }
  return value != nullptr ? 0 : 2;
}
