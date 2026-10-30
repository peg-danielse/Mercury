#define _GNU_SOURCE

#include <dlfcn.h>
#include <sys/socket.h>

/* Sets SO_REUSEPORT before every IPv4 and IPv6 bind, so several processes of one service can listen on
 * its port and the kernel balances new connections. Mercury puts this library in LD_PRELOAD when
 * MERCURY_REUSEPORT=1 (SPEC.md "LLM interface"). Go services call the bind syscall directly and are not covered. */

int bind(int sockfd, const struct sockaddr* addr, socklen_t addrlen) {
  static int (*real_bind)(int, const struct sockaddr*, socklen_t) = 0;
  if (real_bind == 0) {
    real_bind = (int (*)(int, const struct sockaddr*, socklen_t))dlsym(RTLD_NEXT, "bind");
  }
  if (addr != 0 && (addr->sa_family == AF_INET || addr->sa_family == AF_INET6)) {
    const int one = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
  }
  return real_bind(sockfd, addr, addrlen);
}
