#pragma once
// RCA step for zeroctl: Paul's per-thread demand GNN (src/gnn.cpp, model models/gnn_v5.json) run in
// observe-only mode next to each zeroctl daemon. Nothing is pinned and no cpuset is written here; the
// sampler only reads threadstat signals every tick_ms, runs the forward pass, and FFD-packs the
// predicted per-thread demand into the cores the daemon needs. zeroctl puts the windowed summary into
// the prompt's RCA section ("which daemon is CPU-starved, by how much").

#include <set>
#include <string>

struct GnnRcaConfig {
  bool enabled = true;
  int tick_ms = 10;     // the model was trained on 10 ms ticks (results/v5/results.json tick_ms)
  int window_s = 5;     // summary window handed to zeroctl
  int max_cpus = 16;    // clamp for the FFD required-core count
};

// Observe exactly these daemons from now on (empty set: stop and free every reader).
void gnn_rca_track(const std::set<std::string>& daemon_ids, const GnnRcaConfig& config);

// JSON object for the zeroctl state "rca.gnn" field:
// {"enabled":bool,"available":bool,"model":"...","error":"...","tick_ms":10,"window_s":5,
//  "daemons":[{"daemon_id","cpus","ticks","required_mean","required_max","required_p90","under_share",
//              "pred_demand_mean","threads_mean"}]}
std::string gnn_rca_json();
