// gru autoscaler port check (SPEC.md "gru autoscaler"): the C++ feature step and forward pass against PyTorch on the
// verification file tools/export_gru.py writes (seq_models.featurize and GRUBase over a synthetic sequence).
#include "gnn.hpp"
#include "gru.hpp"

#include <chrono>
#include <cmath>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
  const std::string dir = MERCURY_SOURCE_DIR;
  const std::string model = argc > 1 ? argv[1] : dir + "/models/gru_slo4_mft.json";
  const std::string checkfile = argc > 2 ? argv[2] : dir + "/src/test/gru/gru_slo4_mft.check.json";
  std::string error;
  const auto weights = load_gnn_weights(model, error);
  if (!weights || !weights->gru) {
    std::cerr << "gru_test: " << (weights ? "not a gru model" : error) << std::endl;
    return 1;
  }
  std::unordered_map<std::string, JsonArray> c;
  if (!load_json_arrays(checkfile, c, error)) {
    std::cerr << "gru_test: " << error << std::endl;
    return 1;
  }
  const int T = c["threads"].shape[0], W = c["threads"].shape[1];
  GruFeatures features;
  GruModel gru(weights->gru), gru_python_features(weights->gru);
  double worst_feature = 0, worst_demand = 0, worst_demand_python = 0;
  int request_mismatches = 0;
  for (int t = 0; t < T; ++t) {
    std::vector<GruThread> threads;
    for (int i = 0; i < W; ++i) {
      const float* x = c["threads"].data.data() + (static_cast<std::size_t>(t) * W + i) * 5;
      threads.push_back({x[0], x[1], x[2], x[3], x[4], static_cast<int>(c["numa"].data[static_cast<std::size_t>(i)])});
    }
    const auto f = features.tick(threads, c["other_run"].data[t], c["other_wait"].data[t], c["occupancy"].data[t],
                                 c["pressure"].data[t], static_cast<int>(c["alloc"].data[t]));
    std::array<float, kGruDaemonFeatures> fp{};
    for (int k = 0; k < kGruDaemonFeatures; ++k) {
      fp[k] = c["features"].data[static_cast<std::size_t>(t) * kGruDaemonFeatures + k];
      worst_feature = std::max(worst_feature, std::abs(static_cast<double>(f[k] - fp[k])) / (1.0 + std::abs(fp[k])));
    }
    const float want = c["demand"].data[t];
    const float got = gru.step(f), got_p = gru_python_features.step(fp);
    worst_demand = std::max(worst_demand, std::abs(static_cast<double>(got - want)));
    worst_demand_python = std::max(worst_demand_python, std::abs(static_cast<double>(got_p - want)));
    if (gru_request(got) != gru_request(want)) ++request_mismatches;
  }
  // Cost of one step (the controller runs it every 10 ms for every daemon, all on Mercury's CPU).
  std::array<float, kGruDaemonFeatures> probe{};
  probe.fill(0.3f);
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < 2000; ++i) gru.step(probe);
  const double step_us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / 2000;
  std::cout << "gru_test: one step " << step_us << " us" << std::endl;
  std::cout << "gru_test: " << T << " ticks, worst feature error " << worst_feature << ", worst demand error "
            << worst_demand << " (on PyTorch's features " << worst_demand_python << "), request mismatches "
            << request_mismatches << std::endl;
  const bool ok = worst_feature < 1e-4 && worst_demand < 1e-3 && worst_demand_python < 1e-3 && request_mismatches == 0;
  std::cout << (ok ? "gru_test ok" : "gru_test FAILED") << std::endl;
  return ok ? 0 : 1;
}
