#pragma once
// gru autoscaler (SPEC.md "gru autoscaler"): the C++ forward pass of gnn-example/mercury/slo/seq_models.py GRUBase
// and its daemon-level featurize(), so a model trained by train_seq.py --model gru sizes a daemon every 10 ms.
// Pure: no I/O; GnnController feeds it the same per-thread nodes the gnn policy uses.

#include <array>
#include <cstddef>
#include <deque>
#include <memory>
#include <vector>

inline constexpr int kGruNodes = 8;                      // seq_models.NODES
inline constexpr int kGruDaemonFeatures = 17 + kGruNodes;  // seq_models.DAEMON_F

// model.pt of GRUBase(d, layers), exported by tools/export_gru.py: inp (Linear DAEMON_F -> d), gru (nn.GRU d -> d,
// `layers` layers, gates r, z, n), out (Linear d -> 1).
struct GruWeights {
  int in = 0, d = 0;
  std::vector<float> inp_w, inp_b;  // [d][in], [d]
  struct Layer {
    std::vector<float> w_ih, w_hh, b_ih, b_hh;  // [3d][d], [3d][d], [3d], [3d]
  };
  std::vector<Layer> layers;
  std::vector<float> out_w;  // [d]
  float out_b = 0;
};

// One recurrent step per 10 ms tick: P(t) = softplus(out(GRU(gelu(inp(features))))), the daemon's core demand.
class GruModel {
 public:
  explicit GruModel(std::shared_ptr<const GruWeights> weights);
  float step(const std::array<float, kGruDaemonFeatures>& features);
  void reset();

 private:
  std::shared_ptr<const GruWeights> w_;
  // The weights transposed ([in][out]) at construction, so each product runs as y += x[c] * row c: contiguous,
  // independent outputs that the compiler vectorizes without reordering any sum (the row-by-row dot products
  // took 0.47 ms a tick on dtn-01, with every daemon's controller on Mercury's one CPU).
  std::vector<float> inp_t_;
  std::vector<std::vector<float>> ih_t_, hh_t_;
  std::vector<std::vector<float>> h_;  // hidden state per layer
};

// The cores to request for demand P: ceil(P), 1..16 (train_seq.ceil_req).
int gru_request(float demand);

// One thread node of a tick (train_gnn.case_tensors features 0-4 and its NUMA node).
struct GruThread {
  float task_clock = 0, run = 0, wait = 0, ctx_switch_k = 0, migrations_100 = 0;
  int numa = 0;
};

// seq_models.featurize for one daemon, one tick at a time. alloc is the daemon's cpuset size this tick; the
// decision history (change, ticks since the last change, changes in the last 100 ticks) is kept here.
class GruFeatures {
 public:
  std::array<float, kGruDaemonFeatures> tick(const std::vector<GruThread>& threads, float other_run, float other_wait,
                                             float occupancy, float pressure, int alloc);

 private:
  bool started_ = false;
  int prev_alloc_ = 0;
  long since_ = 10000;       // ticks since the last change; featurize starts as if the last change were long ago
  std::deque<char> recent_;  // change flags of the last 100 ticks, this one included
  int recent_sum_ = 0;
};
