#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

// One listening process for the replica test. With --listen-fd 3 it accepts on the socket Mercury
// passed. Otherwise it binds 127.0.0.1:$BIND_PORT itself, so the test can see SO_REUSEPORT from
// LD_PRELOAD. Each accepted connection is answered with MERCURY_DAEMON_ID and a newline.

int main(int argc, char** argv) {
  int fd = -1;
  if (argc == 3 && std::strcmp(argv[1], "--listen-fd") == 0) {
    fd = 3;
  } else {
    const char* port_text = std::getenv("BIND_PORT");
    if (port_text == nullptr) {
      return 2;
    }
    fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
      return 3;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(std::atoi(port_text)));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 || ::listen(fd, 64) < 0) {
      return 3;
    }
  }
  const char* name = std::getenv("MERCURY_DAEMON_ID");
  const std::string line = std::string(name != nullptr ? name : "?") + "\n";
  for (;;) {
    const int client = ::accept(fd, nullptr, nullptr);
    if (client < 0) {
      continue;
    }
    const auto wrote = ::write(client, line.data(), line.size());
    (void)wrote;
    ::close(client);
  }
}
