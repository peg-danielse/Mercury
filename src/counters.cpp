#include "counters.hpp"

#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <optional>
#include <stdexcept>
#include <utility>

namespace {

struct Spec {
  const char* name;
  std::uint32_t type;
  std::uint64_t config;
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

std::int64_t monotonic_ns() {
  timespec now{};
  if (::clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    throw std::runtime_error("clock_gettime failed");
  }
  return static_cast<std::int64_t>(now.tv_sec) * 1000000000LL + now.tv_nsec;
}

int open_event(const Spec& spec, pid_t pid, int cpu, int group_fd, int exclude_kernel) {
  perf_event_attr attr{};
  attr.type = spec.type;
  attr.size = sizeof(attr);
  attr.config = spec.config;
  attr.disabled = group_fd == -1 ? 1 : 0;
  attr.exclude_kernel = exclude_kernel;
  attr.exclude_hv = 1;
  attr.read_format = PERF_FORMAT_GROUP | PERF_FORMAT_TOTAL_TIME_ENABLED |
                     PERF_FORMAT_TOTAL_TIME_RUNNING | PERF_FORMAT_ID;

  const auto fd = ::syscall(__NR_perf_event_open, &attr, pid, cpu, group_fd, PERF_FLAG_FD_CLOEXEC);
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

// cpu -1 counts the task on any CPU; pid -1 with a cpu counts everything on that CPU.
std::vector<Opened> open_group(pid_t pid, int cpu, const Spec* specs, std::size_t count) {
  std::vector<Opened> opened;
  opened.reserve(count);
  int leader = -1;
  int exclude_kernel = 0;
  int saved = ENODEV;

  for (std::size_t i = 0; i < count; ++i) {
    int fd = open_event(specs[i], pid, cpu, leader, exclude_kernel);
    if (fd < 0 && leader < 0 && errno == EACCES && exclude_kernel == 0) {
      exclude_kernel = 1;
      fd = open_event(specs[i], pid, cpu, -1, exclude_kernel);
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

std::optional<std::uint64_t> group_value(const GroupData& data, std::uint64_t id) {
  for (std::uint64_t i = 0; i < data.nr; ++i) {
    if (data.values[i].id == id) {
      return data.values[i].value;
    }
  }
  return std::nullopt;
}

Sample make_delta(const std::vector<Opened>& counters, const GroupData& previous, const GroupData& now) {
  Sample sample;
  sample.timestamp_ns = monotonic_ns();
  sample.time_enabled = now.time_enabled - previous.time_enabled;
  sample.time_running = now.time_running - previous.time_running;
  const int nvalues = std::min(static_cast<int>(counters.size()), kMaxCounters);
  sample.nvalues = nvalues;
  for (int i = 0; i < nvalues; ++i) {
    const auto after = group_value(now, counters[static_cast<std::size_t>(i)].id);
    const auto before = group_value(previous, counters[static_cast<std::size_t>(i)].id);
    sample.value[i] = (after ? *after : 0) - (before ? *before : 0);
  }
  return sample;
}

}  // namespace

struct CounterGroup::Impl {
  std::vector<Opened> events;
  bool armed = false;
  GroupData previous{};
};

CounterGroup::CounterGroup() = default;
CounterGroup::~CounterGroup() = default;
CounterGroup::CounterGroup(CounterGroup&&) noexcept = default;
CounterGroup& CounterGroup::operator=(CounterGroup&&) noexcept = default;

CounterGroup::CounterGroup(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

bool CounterGroup::empty() const { return !impl_ || impl_->events.empty(); }

std::vector<std::string> CounterGroup::names() const {
  std::vector<std::string> names;
  if (!impl_) {
    return names;
  }
  names.reserve(impl_->events.size());
  for (const auto& event : impl_->events) {
    names.emplace_back(event.name);
  }
  return names;
}

void CounterGroup::arm() {
  if (!impl_ || impl_->events.empty()) {
    throw std::runtime_error("no counters");
  }
  const int leader = impl_->events.front().fd.get();
  ioctl_group(leader, PERF_EVENT_IOC_RESET, "reset failed");
  ioctl_group(leader, PERF_EVENT_IOC_ENABLE, "enable failed");
  impl_->previous = read_group(leader);
  impl_->armed = true;
}

bool CounterGroup::pull(Sample& sample) {
  if (!impl_ || !impl_->armed || impl_->events.empty()) {
    return false;
  }
  try {
    const int leader = impl_->events.front().fd.get();
    const auto now = read_group(leader);
    sample = make_delta(impl_->events, impl_->previous, now);
    impl_->previous = now;
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

namespace {
constexpr Spec kSoftwareTrio[] = {
    {"task-clock", PERF_TYPE_SOFTWARE, PERF_COUNT_SW_TASK_CLOCK},
    {"context-switches", PERF_TYPE_SOFTWARE, PERF_COUNT_SW_CONTEXT_SWITCHES},
    {"cpu-migrations", PERF_TYPE_SOFTWARE, PERF_COUNT_SW_CPU_MIGRATIONS},
};

constexpr Spec kCpuHardware[] = {
    {"cycles", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES},
    {"instructions", PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS},
    {"llc_misses", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES},
};
}  // namespace

CounterGroup open_software(pid_t pid) {
  auto impl = std::make_unique<CounterGroup::Impl>();
  impl->events = open_group(pid, -1, kSoftwareTrio, std::size(kSoftwareTrio));
  return CounterGroup(std::move(impl));
}

CounterGroup open_cpu_hardware(int cpu) {
  auto impl = std::make_unique<CounterGroup::Impl>();
  impl->events = open_group(-1, cpu, kCpuHardware, std::size(kCpuHardware));
  return CounterGroup(std::move(impl));
}
