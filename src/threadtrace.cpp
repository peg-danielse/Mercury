// threadtrace: per-thread trace recorder for one Mercury daemon cgroup (SPEC.md "gnn policy").
// threadtrace <daemon_id | cgroup dir> [--interval-ms 1000] [--duration-s 0] [--out FILE]
//             [--max-perf 1024] [--format jsonl|csv]
// One record per tick (JSONL: one object per tick; CSV: one row per thread per tick).
// Stops at --duration-s, when the cgroup disappears, or on SIGINT/SIGTERM. Needs root (or
// perf_event_paranoid <= 0 and ownership of the threads) for the perf groups.

#include "threadstat.hpp"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <string>

namespace {

volatile std::sig_atomic_t stop = 0;
void on_signal(int) { stop = 1; }

std::string esc(const std::string& s) {
  std::string out;
  for (const char c : s) {
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if (static_cast<unsigned char>(c) < 0x20) out += ' ';
    else out += c;
  }
  return out;
}

void usage() {
  std::cerr << "usage: threadtrace <daemon_id|cgroup_dir> [--interval-ms N] [--duration-s N] [--out FILE]"
               " [--max-perf N] [--format jsonl|csv]\n";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) { usage(); return 2; }
  std::string target = argv[1];
  long interval_ms = 1000;
  double duration_s = 0;
  std::string out_path;
  std::size_t max_perf = 1024;
  std::string format = "jsonl";
  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string { if (i + 1 >= argc) { usage(); std::exit(2); } return argv[++i]; };
    if (a == "--interval-ms") interval_ms = std::atol(next().c_str());
    else if (a == "--duration-s") duration_s = std::atof(next().c_str());
    else if (a == "--out") out_path = next();
    else if (a == "--max-perf") max_perf = static_cast<std::size_t>(std::atol(next().c_str()));
    else if (a == "--format") format = next();
    else { usage(); return 2; }
  }
  if (interval_ms < 10 || (format != "jsonl" && format != "csv")) { usage(); return 2; }
  const std::string dir = target.find('/') == std::string::npos ? "/sys/fs/cgroup/mercury/" + target : target;

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  raise_nofile_limit();

  std::ofstream file;
  if (!out_path.empty()) {
    file.open(out_path, std::ios::out | std::ios::trunc);
    if (!file) { std::cerr << "threadtrace: cannot open " << out_path << "\n"; return 1; }
  }
  std::ostream& out = out_path.empty() ? std::cout : file;
  if (format == "csv") {
    out << "ts_ns,interval_ns,tid,comm,fresh,perf,task_clock_ns,context_switches,cpu_migrations,run_ns,wait_ns,"
           "timeslices,cpu,node,cg_usage_ns,cg_user_ns,cg_system_ns,cg_psi,cg_some_ns,cg_full_ns,cpuset_cpus\n";
  }

  ThreadStats stats(dir, max_perf);
  timespec next{};
  ::clock_gettime(CLOCK_MONOTONIC, &next);
  const auto start_s = static_cast<double>(next.tv_sec) + next.tv_nsec / 1e9;
  long ticks = 0;
  while (!stop) {
    const auto r = stats.tick();
    if (r.cgroup_gone) { std::cerr << "threadtrace: cgroup " << dir << " gone\n"; break; }
    const auto& c = r.cgroup;
    if (format == "jsonl") {
      out << "{\"ts_ns\":" << r.timestamp_ns << ",\"tick\":" << ticks << ",\"interval_ns\":" << r.interval_ns
          << ",\"cgroup\":{\"cpu_ok\":" << (c.cpu_ok ? "true" : "false") << ",\"usage_ns\":" << c.usage_ns
          << ",\"user_ns\":" << c.user_ns << ",\"system_ns\":" << c.system_ns << ",\"psi\":" << (c.psi ? "true" : "false")
          << ",\"some_ns\":" << c.some_ns << ",\"full_ns\":" << c.full_ns << ",\"cpuset_cpus\":" << c.cpuset_cpus
          << ",\"cpuset\":\"" << esc(c.cpuset) << "\"},\"exited\":" << r.exited << ",\"perf_open\":" << r.perf_open << ",\"read_ns\":" << r.read_ns
          << ",\"threads\":[";
      bool first = true;
      for (const auto& t : r.threads) {
        out << (first ? "" : ",") << "{\"tid\":" << t.tid << ",\"comm\":\"" << esc(t.comm) << "\",\"fresh\":"
            << (t.fresh ? "true" : "false") << ",\"perf\":" << (t.perf ? "true" : "false")
            << ",\"task_clock_ns\":" << t.task_clock_ns << ",\"context_switches\":" << t.context_switches
            << ",\"cpu_migrations\":" << t.cpu_migrations << ",\"run_ns\":" << t.run_ns << ",\"wait_ns\":" << t.wait_ns
            << ",\"timeslices\":" << t.timeslices << ",\"cpu\":" << t.cpu << ",\"node\":" << t.node << "}";
        first = false;
      }
      out << "]}\n";
    } else {
      for (const auto& t : r.threads) {
        out << r.timestamp_ns << ',' << r.interval_ns << ',' << t.tid << ',' << esc(t.comm) << ',' << t.fresh << ','
            << t.perf << ',' << t.task_clock_ns << ',' << t.context_switches << ',' << t.cpu_migrations << ','
            << t.run_ns << ',' << t.wait_ns << ',' << t.timeslices << ',' << t.cpu << ',' << t.node << ','
            << c.usage_ns << ',' << c.user_ns << ',' << c.system_ns << ',' << c.psi << ',' << c.some_ns << ','
            << c.full_ns << ',' << c.cpuset_cpus << '\n';
      }
    }
    out.flush();
    ++ticks;
    next.tv_nsec += (interval_ms % 1000) * 1000000L;
    next.tv_sec += interval_ms / 1000 + next.tv_nsec / 1000000000L;
    next.tv_nsec %= 1000000000L;
    const auto next_s = static_cast<double>(next.tv_sec) + next.tv_nsec / 1e9;
    if (duration_s > 0 && next_s - start_s > duration_s + 1e-6) break;
    while (!stop && ::clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, nullptr) == EINTR) {
    }
  }
  std::cerr << "threadtrace: " << ticks << " ticks\n";
  return 0;
}
