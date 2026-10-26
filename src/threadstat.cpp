#include "threadstat.hpp"

#include "counters.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/resource.h>

#include <cerrno>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <sstream>

namespace {

std::string slurp(const std::string& path) {
  std::ifstream in(path);
  if (!in) {
    return {};
  }
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

std::int64_t mono_ns() {
  timespec now{};
  ::clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<std::int64_t>(now.tv_sec) * 1000000000LL + now.tv_nsec;
}

std::vector<int> parse_list(const std::string& text) {
  std::vector<int> out;
  std::stringstream in(text);
  std::string part;
  while (std::getline(in, part, ',')) {
    while (!part.empty() && (part.back() == '\n' || part.back() == ' ')) part.pop_back();
    if (part.empty()) continue;
    const auto dash = part.find('-');
    try {
      const int a = std::stoi(part.substr(0, dash));
      const int b = dash == std::string::npos ? a : std::stoi(part.substr(dash + 1));
      for (int c = a; c <= b; ++c) out.push_back(c);
    } catch (...) {
    }
  }
  return out;
}


// A kept-open /proc or cgroup file, re-read with pread(0) each tick (no open/close per read).
struct KeptFile {
  int fd = -1;
  KeptFile() = default;
  explicit KeptFile(const std::string& path) : fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC)) {}
  KeptFile(KeptFile&& o) noexcept : fd(o.fd) { o.fd = -1; }
  KeptFile& operator=(KeptFile&& o) noexcept { if (this != &o) { if (fd >= 0) ::close(fd); fd = o.fd; o.fd = -1; } return *this; }
  KeptFile(const KeptFile&) = delete;
  ~KeptFile() { if (fd >= 0) ::close(fd); }
  // Whole file into buf (NUL-terminated). Returns length, or -1.
  long read(char* buf, std::size_t size) const {
    if (fd < 0) return -1;
    std::size_t got = 0;
    while (got + 1 < size) {
      const auto n = ::pread(fd, buf + got, size - 1 - got, static_cast<off_t>(got));
      if (n < 0) { if (errno == EINTR) continue; return -1; }
      if (n == 0) break;
      got += static_cast<std::size_t>(n);
    }
    buf[got] = 0;
    return static_cast<long>(got);
  }
  std::string text() const {
    char stack[4096];
    const auto n = read(stack, sizeof(stack));
    if (n < 0) return {};
    if (static_cast<std::size_t>(n) + 1 < sizeof(stack)) return std::string(stack, static_cast<std::size_t>(n));
    std::string out(65536, '\0');
    for (;;) {
      const auto m = read(out.data(), out.size());
      if (m < 0) return {};
      if (static_cast<std::size_t>(m) + 1 < out.size()) { out.resize(static_cast<std::size_t>(m)); return out; }
      out.resize(out.size() * 2);
    }
  }
};

std::uint64_t d(std::uint64_t now, std::uint64_t before) { return now >= before ? now - before : 0; }

struct Sched {
  std::uint64_t run = 0, wait = 0, slices = 0;
};

std::optional<Sched> read_schedstat(const KeptFile& f) {
  char buf[128];
  if (f.read(buf, sizeof(buf)) <= 0) return std::nullopt;
  char* p = buf;
  Sched s;
  s.run = std::strtoull(p, &p, 10);
  s.wait = std::strtoull(p, &p, 10);
  s.slices = std::strtoull(p, &p, 10);
  return s;
}

// Last CPU (field 39) and comm of /proc/<tid>/stat.
int read_cpu(const KeptFile& f, std::string& comm) {
  char buf[1024];
  if (f.read(buf, sizeof(buf)) <= 0) return -1;
  char* open = std::strchr(buf, '(');
  char* close = std::strrchr(buf, ')');
  if (open == nullptr || close == nullptr) return -1;
  comm.assign(open + 1, close);
  char* p = close + 1;
  for (int field = 3; field <= 39; ++field) {
    while (*p == ' ') ++p;
    if (*p == 0) return -1;
    if (field == 39) return std::atoi(p);
    while (*p != ' ' && *p != 0) ++p;
  }
  return -1;
}

struct CgCounters {
  bool cpu = false;
  std::uint64_t usage = 0, user = 0, system = 0;
  bool psi = false;
  std::uint64_t some = 0, full = 0;
};

CgCounters read_cg(const KeptFile& stat, const KeptFile& pressure) {
  CgCounters c;
  std::istringstream in(stat.text());
  std::string key;
  std::uint64_t v = 0;
  while (in >> key >> v) {
    if (key == "usage_usec") { c.usage = v; c.cpu = true; }
    else if (key == "user_usec") c.user = v;
    else if (key == "system_usec") c.system = v;
  }
  std::istringstream p(pressure.text());
  std::string line;
  while (std::getline(p, line)) {
    const auto at = line.find("total=");
    if (at == std::string::npos) continue;
    try {
      const auto total = std::stoull(line.substr(at + 6));
      if (line.rfind("some", 0) == 0) { c.some = total; c.psi = true; }
      else if (line.rfind("full", 0) == 0) c.full = total;
    } catch (...) {
    }
  }
  return c;
}

}  // namespace

std::vector<int> cpu_node_map() {
  std::vector<int> map;
  DIR* dir = ::opendir("/sys/devices/system/node");
  if (dir == nullptr) return map;
  while (auto* e = ::readdir(dir)) {
    if (std::strncmp(e->d_name, "node", 4) != 0 || e->d_name[4] < '0' || e->d_name[4] > '9') continue;
    const int node = std::atoi(e->d_name + 4);
    for (const int cpu : parse_list(slurp(std::string("/sys/devices/system/node/") + e->d_name + "/cpulist"))) {
      if (cpu >= static_cast<int>(map.size())) map.resize(static_cast<std::size_t>(cpu) + 1, -1);
      map[static_cast<std::size_t>(cpu)] = node;
    }
  }
  ::closedir(dir);
  return map;
}

void raise_nofile_limit() {
  rlimit lim{};
  if (::getrlimit(RLIMIT_NOFILE, &lim) != 0) return;
  lim.rlim_cur = lim.rlim_max == RLIM_INFINITY ? 65536 : lim.rlim_max;
  ::setrlimit(RLIMIT_NOFILE, &lim);
}

struct ThreadStats::Impl {
  struct Thread {
    CounterGroup group;  // empty when perf is off for this tid
    std::vector<std::string> names;
    std::optional<Sched> sched;
    KeptFile stat_file, sched_file;  // /proc/<tid>/stat, /proc/<tid>/schedstat
  };
  std::string dir;
  std::size_t max_perf;
  std::vector<int> nodes = cpu_node_map();
  std::unordered_map<pid_t, Thread> threads;
  std::size_t perf_open = 0;
  CgCounters cg;
  bool have_cg = false;
  std::int64_t last_ns = 0;
  bool warned = false;
  KeptFile threads_file, cpu_stat, cpu_pressure, cpuset_file, type_file;
};

ThreadStats::ThreadStats(std::string cgroup_dir, std::size_t max_perf_threads)
    : impl_(std::make_unique<Impl>()) {
  impl_->dir = std::move(cgroup_dir);
  impl_->max_perf = max_perf_threads;
  auto& m = *impl_;
  m.threads_file = KeptFile(m.dir + "/cgroup.threads");
  m.cpu_stat = KeptFile(m.dir + "/cpu.stat");
  m.cpu_pressure = KeptFile(m.dir + "/cpu.pressure");
  m.cpuset_file = KeptFile(m.dir + "/cpuset.cpus.effective");
}

ThreadStats::~ThreadStats() = default;  // CounterGroup closes its fds

TickResult ThreadStats::tick() {
  auto& m = *impl_;
  TickResult r;
  const auto begin_ns = mono_ns();
  const auto list = m.threads_file.text();
  r.timestamp_ns = mono_ns();
  r.interval_ns = m.last_ns == 0 ? 0 : r.timestamp_ns - m.last_ns;
  m.last_ns = r.timestamp_ns;
  if (list.empty() && ::access(m.dir.c_str(), F_OK) != 0) {
    r.cgroup_gone = true;
    return r;
  }

  const auto cg = read_cg(m.cpu_stat, m.cpu_pressure);
  r.cgroup.cpu_ok = cg.cpu && m.have_cg && m.cg.cpu;
  if (r.cgroup.cpu_ok) {
    r.cgroup.usage_ns = d(cg.usage, m.cg.usage) * 1000;
    r.cgroup.user_ns = d(cg.user, m.cg.user) * 1000;
    r.cgroup.system_ns = d(cg.system, m.cg.system) * 1000;
  }
  r.cgroup.psi = cg.psi && m.have_cg && m.cg.psi;
  if (r.cgroup.psi) {
    r.cgroup.some_ns = d(cg.some, m.cg.some) * 1000;
    r.cgroup.full_ns = d(cg.full, m.cg.full) * 1000;
  }
  m.cg = cg;
  m.have_cg = true;
  auto cpuset = m.cpuset_file.text();
  while (!cpuset.empty() && cpuset.back() == '\n') cpuset.pop_back();
  r.cgroup.cpuset = cpuset;
  r.cgroup.cpuset_cpus = static_cast<int>(parse_list(cpuset).size());

  std::set<pid_t> live;
  std::istringstream in(list);
  pid_t tid = 0;
  while (in >> tid) live.insert(tid);

  for (auto it = m.threads.begin(); it != m.threads.end();) {
    if (live.count(it->first) == 0) {
      if (!it->second.group.empty()) --m.perf_open;
      it = m.threads.erase(it);
      ++r.exited;
    } else {
      ++it;
    }
  }

  for (const pid_t t : live) {
    ThreadDelta out;
    out.tid = t;
    auto found = m.threads.find(t);
    if (found == m.threads.end()) {
      Impl::Thread th;
      const auto proc = "/proc/" + std::to_string(t);
      th.stat_file = KeptFile(proc + "/stat");
      th.sched_file = KeptFile(proc + "/schedstat");
      if (m.perf_open < m.max_perf) {
        try {
          th.group = open_software(t);
          th.group.arm();
          th.names = th.group.names();
          ++m.perf_open;
        } catch (const std::exception& error) {
          th.group = CounterGroup();
          if (!m.warned) {
            std::cerr << "threadstat perf off tid=" << t << ": " << error.what()
                      << (errno == EMFILE ? " (fd limit)" : "") << std::endl;
            m.warned = true;
          }
        }
      }
      th.sched = read_schedstat(th.sched_file);
      out.cpu = read_cpu(th.stat_file, out.comm);
      if (out.cpu >= 0 && out.cpu < static_cast<int>(m.nodes.size())) out.node = m.nodes[static_cast<std::size_t>(out.cpu)];
      out.fresh = true;
      out.perf = !th.group.empty();
      out.schedstat = th.sched.has_value();
      m.threads.emplace(t, std::move(th));
      r.threads.push_back(std::move(out));
      continue;
    }
    auto& th = found->second;
    if (!th.group.empty()) {
      Sample s;
      if (th.group.pull(s)) {
        out.perf = true;
        for (int i = 0; i < s.nvalues && i < static_cast<int>(th.names.size()); ++i) {
          const auto& n = th.names[static_cast<std::size_t>(i)];
          if (n == "task-clock") out.task_clock_ns = s.value[i];
          else if (n == "context-switches") out.context_switches = s.value[i];
          else if (n == "cpu-migrations") out.cpu_migrations = s.value[i];
        }
      }
    }
    const auto now = read_schedstat(th.sched_file);
    if (now && th.sched) {
      out.schedstat = true;
      out.run_ns = d(now->run, th.sched->run);
      out.wait_ns = d(now->wait, th.sched->wait);
      out.timeslices = d(now->slices, th.sched->slices);
    }
    if (now) th.sched = now;
    out.cpu = read_cpu(th.stat_file, out.comm);
    if (out.cpu >= 0 && out.cpu < static_cast<int>(m.nodes.size())) out.node = m.nodes[static_cast<std::size_t>(out.cpu)];
    r.threads.push_back(std::move(out));
  }
  r.perf_open = m.perf_open;
  r.read_ns = mono_ns() - begin_ns;
  return r;
}
