#include "cpustat.hpp"

#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool ok, const std::string& message) {
  if (!ok) {
    throw std::runtime_error(message);
  }
}

std::string read_file(const std::string& path) {
  std::ifstream in(path);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

void parsers() {
  require(parse_cpu_ranges("0-3,32-35\n") == std::vector<int>{0, 1, 2, 3, 32, 33, 34, 35}, "cpu ranges");
  require(parse_cpu_ranges("5") == std::vector<int>{5}, "single cpu");
  require(parse_cpu_ranges("3-1").empty() && parse_cpu_ranges("a").empty(), "malformed ranges");

  const auto stat = parse_proc_stat("cpu  9 9 9 9 9 9 9 0 0 0\ncpu0 10 1 5 100 2 3 4 0 0 0\ncpu7 1 0 0 0 0 0 0\n");
  require(stat.size() == 2 && !stat.count(-1), "proc stat rows (the cpu total line is skipped)");
  require(stat.at(0).busy == 10 + 1 + 5 + 3 + 4 && stat.at(0).irq == 3 + 4, "proc stat busy and irq");

  const auto sched =
      parse_schedstat("version 17\ntimestamp 1\ncpu0 0 0 0 0 0 0 600 1200 58\ndomain0 ff 1 2\ncpu1 0 0 0\n");
  require(sched.size() == 1 && sched.at(0).run_ns == 600 && sched.at(0).run_delay_ns == 1200 && sched.at(0).slices == 58,
          "schedstat run time, run_delay and slices (short lines skipped)");
  require(!parse_schedstat(read_file("/proc/schedstat")).empty(), "this host's /proc/schedstat parses");
}

void placement_log() {
  char dir[] = "/tmp/cpustat_test.XXXXXX";
  require(::mkdtemp(dir) != nullptr, "mkdtemp");
  require(!cpustat_enabled(), "off before init");
  note_cpuset("a", {1});  // ignored while off
  init_cpustat(dir);
  require(cpustat_enabled(), "on after init");
  note_cpuset("a", {3, 1});
  note_cpuset("a", {1, 3});  // unchanged: no line
  note_cpuset("b", {1});
  note_cpuset("a", {});
  note_cpuset("c", {});  // never had a set: no line
  std::istringstream lines(read_file(std::string(dir) + "/placement.jsonl"));
  std::vector<std::string> got;
  for (std::string line; std::getline(lines, line);) {
    got.push_back(line.substr(line.find(",\"daemon\"")));
  }
  require(got == std::vector<std::string>{",\"daemon\":\"a\",\"cpus\":[1,3]}", ",\"daemon\":\"b\",\"cpus\":[1]}",
                                          ",\"daemon\":\"a\",\"cpus\":[]}"},
          "placement lines");
  std::system(("rm -rf " + std::string(dir)).c_str());
}

}  // namespace

int main() {
  try {
    parsers();
    placement_log();
  } catch (const std::exception& error) {
    std::cerr << "cpustat_test: " << error.what() << std::endl;
    return 1;
  }
  std::cout << "cpustat_test ok" << std::endl;
  return 0;
}
