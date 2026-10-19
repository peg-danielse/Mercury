// gnnreplay: run the C++ gnn forward pass over a recorded threadtrace case, with the node set and
// features train_gnn.py uses (labelled gnnload workers as nodes, every other thread in one
// aggregate node), to check it against the Python model.
// gnnreplay <weights.json> <case_dir> > preds.csv   (columns: tick,tid,pred; aggregate tid = -1)

#include "gnn.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <unordered_map>

namespace {

double num(const std::string& s, std::size_t from, const char* key, std::size_t* at = nullptr) {
  const std::string k = std::string("\"") + key + "\":";
  const auto p = s.find(k, from);
  if (p == std::string::npos) return 0;
  if (at) *at = p;
  return std::strtod(s.c_str() + p + k.size(), nullptr);
}

struct T {
  double tc = 0, run = 0, wait = 0, cs = 0, mig = 0;
  int node = -1;
};

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) { std::cerr << "usage: gnnreplay <weights.json> <case_dir>\n"; return 2; }
  std::string err;
  auto w = load_gnn_weights(argv[1], err);
  if (!w) { std::cerr << err << "\n"; return 1; }
  const std::string dir = argv[2];
  std::set<long> workers;
  {
    std::ifstream in(dir + "/labels.csv");
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty() || line[0] == '#' || line[0] == 'p') continue;
      long ts, ph, th, tid;
      if (std::sscanf(line.c_str(), "%ld,%ld,%ld,%ld", &ts, &ph, &th, &tid) == 4 && ph >= 0) workers.insert(tid);
    }
  }
  // dtn-01 distances, as train_gnn.py dtn01_distance()
  std::vector<std::vector<double>> dist(8, std::vector<double>(8, 32.0));
  for (int a = 0; a < 8; ++a)
    for (int b = 0; b < 8; ++b) dist[a][b] = a == b ? 10 : (a / 4 == b / 4 ? 16 : 32);
  GnnModel model(w, dist);
  std::ifstream in(dir + "/trace.jsonl");
  std::string line;
  long tick = 0;
  std::printf("tick,tid,pred\n");
  while (std::getline(in, line)) {
    const double iv = num(line, 0, "interval_ns");
    if (iv <= 0) continue;
    const double usage = num(line, 0, "usage_ns"), some = num(line, 0, "some_ns");
    const int cpus = static_cast<int>(num(line, 0, "cpuset_cpus"));
    std::unordered_map<long, T> th;
    double other_run = 0, other_wait = 0;
    std::size_t pos = line.find("\"threads\":[");
    while ((pos = line.find("{\"tid\":", pos)) != std::string::npos) {
      const long tid = static_cast<long>(num(line, pos, "tid"));
      T t;
      t.tc = num(line, pos, "task_clock_ns") / iv;
      t.cs = num(line, pos, "context_switches") * 1e9 / iv;
      t.mig = num(line, pos, "cpu_migrations") * 1e9 / iv;
      t.run = num(line, pos, "run_ns") / iv;
      t.wait = num(line, pos, "wait_ns") / iv;
      t.node = static_cast<int>(num(line, pos, "node"));
      if (workers.count(tid)) th[tid] = t;
      else { other_run += t.run; other_wait += t.wait; }
      pos += 7;
    }
    std::vector<GnnNode> nodes;
    for (const long tid : workers) {
      GnnNode n;
      n.key = tid;
      const auto it = th.find(tid);
      if (it != th.end()) {
        n.x[0] = static_cast<float>(it->second.tc); n.x[1] = static_cast<float>(it->second.run);
        n.x[2] = static_cast<float>(it->second.wait); n.x[3] = static_cast<float>(it->second.cs / 1000);
        n.x[4] = static_cast<float>(it->second.mig / 100);
        n.numa = std::clamp(it->second.node, 0, 7);
      } else {
        n.numa = 0;
      }
      nodes.push_back(n);
    }
    GnnNode agg;
    agg.key = -1;
    agg.x[0] = static_cast<float>(other_run); agg.x[1] = static_cast<float>(other_run); agg.x[2] = static_cast<float>(other_wait);
    agg.x[8] = 1;
    agg.numa = nodes.empty() ? 0 : nodes.front().numa;
    nodes.push_back(agg);
    for (auto& n : nodes) {
      n.x[5] = static_cast<float>(usage / iv / std::max(1, cpus));
      n.x[6] = static_cast<float>(some / iv);
      n.x[7] = static_cast<float>(cpus / 16.0);
      n.x[9 + n.numa] = 1;
    }
    const auto p = model.step(nodes);
    for (std::size_t i = 0; i < nodes.size(); ++i) std::printf("%ld,%ld,%.7f\n", tick, nodes[i].key, p[i]);
    ++tick;
  }
  return 0;
}
