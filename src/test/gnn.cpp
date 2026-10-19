// gnn_test: the committed model (models/gnn_v5.json) loads with the 17-feature layout and the
// forward pass behaves as the per-thread demand model the zeroctl RCA step relies on.
#include "gnn.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {
int failures = 0;
void check(bool ok, const char* what) {
  if (!ok) {
    std::cerr << "FAIL " << what << std::endl;
    ++failures;
  }
}

GnnNode thread_node(long tid, float run, float wait) {
  GnnNode n;
  n.key = tid;
  n.x[0] = run;  // task-clock share
  n.x[1] = run;
  n.x[2] = wait;
  n.x[5] = 0.9f;  // cgroup occupancy
  n.x[6] = wait;  // pressure
  n.x[9] = 1.f;   // NUMA node 0
  return n;
}
}  // namespace

int main() {
  std::string error;
  auto weights = load_gnn_weights(GNN_MODEL, error);
  check(weights != nullptr, "load models/gnn_v5.json");
  if (!weights) {
    std::cerr << error << std::endl;
    return 1;
  }
  check(weights->K == 3 && weights->F == kGnnFeatures && weights->H == 16, "v5 shape K=3 F=17 H=16");
  check(load_gnn_weights("/nonexistent.json", error) == nullptr && !error.empty(), "missing model is an error");

  GnnModel model(weights, {{10.0}});
  GnnNode agg;
  agg.key = -1;
  agg.x[8] = 1.f;
  agg.x[9] = 1.f;
  std::vector<float> busy, idle;
  for (int t = 0; t < 10; ++t) {
    // two saturated, stacked threads (run 0.5, wait 0.5 each on one CPU) and one idle thread
    const auto out = model.step({thread_node(1, 0.5f, 0.5f), thread_node(2, 0.5f, 0.5f), thread_node(3, 0.0f, 0.0f), agg});
    check(out.size() == 4, "one prediction per node");
    for (const float v : out) check(v >= 0.f && v <= 1.f && std::isfinite(v), "prediction in [0,1]");
    busy.push_back(out[0]);
    idle.push_back(out[2]);
  }
  std::cout << "busy thread demand " << busy.back() << ", idle thread demand " << idle.back() << std::endl;
  check(busy.back() > idle.back() + 0.2f, "a contended thread is predicted to need more than an idle one");
  check(busy.back() > 0.5f, "a thread waiting half the interval needs more than its run share");

  GnnModel again(weights, {{10.0}});
  for (int t = 0; t < 10; ++t) {
    const auto out = again.step({thread_node(1, 0.5f, 0.5f), thread_node(2, 0.5f, 0.5f), thread_node(3, 0.0f, 0.0f), agg});
    if (t == 9) check(std::fabs(out[0] - busy.back()) < 1e-6f, "deterministic");
  }

  const auto bins = ffd_bins({0.9, 0.8, 0.3, 0.2, 0.0});
  check(bins.size() == 3, "FFD packs 0.9, 0.8, 0.3, 0.2 into three cores");

  if (failures == 0) std::cout << "gnn_test ok" << std::endl;
  return failures == 0 ? 0 : 1;
}
