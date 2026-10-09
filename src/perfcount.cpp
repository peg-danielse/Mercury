#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr int kMaxCounters = 8;

struct Spec {
  const char* name;
  std::uint32_t type;
  std::uint64_t config;
};

constexpr Spec kHardware[] = {
    {"cycles", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES},
    {"instructions", PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS},
    {"cache-references", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_REFERENCES},
    {"cache-misses", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES},
    {"branches", PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_INSTRUCTIONS},
    {"branch-misses", PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES},
};

constexpr Spec kSoftware[] = {
    {"task-clock", PERF_TYPE_SOFTWARE, PERF_COUNT_SW_TASK_CLOCK},
    {"page-faults", PERF_TYPE_SOFTWARE, PERF_COUNT_SW_PAGE_FAULTS},
    {"context-switches", PERF_TYPE_SOFTWARE, PERF_COUNT_SW_CONTEXT_SWITCHES},
    {"cpu-migrations", PERF_TYPE_SOFTWARE, PERF_COUNT_SW_CPU_MIGRATIONS},
};

class Fd {
 public:
  explicit Fd(int fd) : fd_(fd) {}
  ~Fd() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  Fd(Fd&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
  Fd& operator=(Fd&&) = delete;

  int get() const { return fd_; }

 private:
  int fd_;
};

struct Opened {
  Fd fd;
  const char* name;
  std::uint64_t id;
};

struct GroupValue {
  std::uint64_t value;
  std::uint64_t id;
};

struct GroupData {
  std::uint64_t nr;
  std::uint64_t time_enabled;
  std::uint64_t time_running;
  GroupValue values[kMaxCounters];
};

int parse_positive(const char* text, const char* what) {
  try {
    std::size_t used = 0;
    const int value = std::stoi(text, &used);
    if (text[used] != '\0' || value < 1) {
      throw std::runtime_error(what);
    }
    return value;
  } catch (const std::runtime_error&) {
    throw;
  } catch (const std::exception&) {
    throw std::runtime_error(what);
  }
}

int parse_milliseconds(const char* text) {
  try {
    std::size_t used = 0;
    const int value = std::stoi(text, &used);
    if (text[used] != '\0' || value < 0) {
      throw std::runtime_error("invalid milliseconds");
    }
    return value;
  } catch (const std::runtime_error&) {
    throw;
  } catch (const std::exception&) {
    throw std::runtime_error("invalid milliseconds");
  }
}

void sleep_ms(int milliseconds) {
  timespec remaining{};
  remaining.tv_sec = milliseconds / 1000;
  remaining.tv_nsec = static_cast<long>(milliseconds % 1000) * 1000000L;
  while (::nanosleep(&remaining, &remaining) != 0) {
    if (errno != EINTR) {
      throw std::runtime_error("nanosleep failed");
    }
  }
}

int open_event(const Spec& spec, int pid, int group_fd, int exclude_kernel) {
  perf_event_attr attr{};
  attr.type = spec.type;
  attr.size = sizeof(attr);
  attr.config = spec.config;
  attr.disabled = group_fd == -1 ? 1 : 0;
  attr.exclude_kernel = exclude_kernel;
  attr.exclude_hv = 1;
  attr.read_format = PERF_FORMAT_GROUP | PERF_FORMAT_TOTAL_TIME_ENABLED |
                     PERF_FORMAT_TOTAL_TIME_RUNNING | PERF_FORMAT_ID;

  const auto fd = ::syscall(__NR_perf_event_open, &attr, pid, -1, group_fd, PERF_FLAG_FD_CLOEXEC);
  if (fd < 0) {
    return -1;
  }
  return static_cast<int>(fd);
}

std::uint64_t event_id(int fd) {
  std::uint64_t id = 0;
  if (::ioctl(fd, PERF_EVENT_IOC_ID, &id) < 0) {
    throw std::runtime_error(std::string("perf event id failed: ") + std::strerror(errno));
  }
  return id;
}

std::vector<Opened> open_group(int pid, const Spec* specs, std::size_t count) {
  std::vector<Opened> opened;
  opened.reserve(count);
  int leader = -1;
  int exclude_kernel = 0;
  int saved = ENODEV;

  for (std::size_t i = 0; i < count; ++i) {
    int fd = open_event(specs[i], pid, leader, exclude_kernel);
    if (fd < 0 && leader < 0 && errno == EACCES && exclude_kernel == 0) {
      exclude_kernel = 1;
      fd = open_event(specs[i], pid, -1, exclude_kernel);
    }
    if (fd < 0) {
      saved = errno;
      continue;
    }

    Fd owned{fd};
    const auto id = event_id(owned.get());
    if (leader < 0) {
      leader = owned.get();
    }
    opened.push_back(Opened{std::move(owned), specs[i].name, id});
  }

  if (opened.empty()) {
    throw std::runtime_error(std::string("perf_event_open failed: ") + std::strerror(saved));
  }
  return opened;
}

void ioctl_group(int leader, unsigned long request, const char* what) {
  if (::ioctl(leader, request, PERF_IOC_FLAG_GROUP) < 0) {
    throw std::runtime_error(std::string(what) + ": " + std::strerror(errno));
  }
}

GroupData read_group(int leader) {
  GroupData data{};
  ssize_t n = 0;
  do {
    n = ::read(leader, &data, sizeof(data));
  } while (n < 0 && errno == EINTR);
  if (n < 0) {
    throw std::runtime_error(std::string("read failed: ") + std::strerror(errno));
  }
  const auto need = sizeof(std::uint64_t) * 3 + data.nr * sizeof(GroupValue);
  if (data.nr > kMaxCounters || static_cast<std::size_t>(n) < need) {
    throw std::runtime_error("short perf read");
  }
  return data;
}

void print_counters(std::vector<Opened>& counters, int milliseconds) {
  const int leader = counters.front().fd.get();
  ioctl_group(leader, PERF_EVENT_IOC_RESET, "reset failed");
  ioctl_group(leader, PERF_EVENT_IOC_ENABLE, "enable failed");
  if (milliseconds > 0) {
    sleep_ms(milliseconds);
  }
  ioctl_group(leader, PERF_EVENT_IOC_DISABLE, "disable failed");

  const GroupData data = read_group(leader);
  for (const auto& counter : counters) {
    for (std::uint64_t i = 0; i < data.nr; ++i) {
      if (data.values[i].id == counter.id) {
        std::cout << counter.name << ' ' << data.values[i].value << '\n';
        break;
      }
    }
  }
  std::cout << "time_enabled_ns " << data.time_enabled << '\n'
            << "time_running_ns " << data.time_running << '\n';
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2 || argc > 3) {
    std::cerr << "usage: perfcount <pid> [milliseconds]\n";
    return 1;
  }
  try {
    const int pid = parse_positive(argv[1], "invalid pid");
    const int milliseconds = argc == 3 ? parse_milliseconds(argv[2]) : 0;
    std::vector<Opened> counters;
    try {
      counters = open_group(pid, kHardware, std::size(kHardware));
    } catch (const std::runtime_error&) {
      counters = open_group(pid, kSoftware, std::size(kSoftware));
    }
    print_counters(counters, milliseconds);
  } catch (const std::exception& error) {
    std::cerr << error.what() << std::endl;
    return 1;
  }
}
