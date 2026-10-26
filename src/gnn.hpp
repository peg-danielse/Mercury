#pragma once
// gnn policy (SPEC.md "gnn policy"): the C++ forward pass of the trained per-thread demand model
// and the per-daemon closed-loop controller (read, graph, infer, bin-pack, pin).

#include "gru.hpp"
#include "threadstat.hpp"

#include <sys/types.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

inline constexpr int kGnnFeatures = 17;  // must match train_gnn.py F_NAMES

// weights.json from gnn-example/mercury/train_gnn.py: g1.W [K][F][H], g1.b [H], g2.W [K][H][H],
// g2.b [H], readout.weight [1][H], readout.bias [1].
// A file with GRU keys (inp.*, gru.*, out.*: train_seq.py --model gru, exported by tools/export_gru.py) loads as the
// gru autoscaler instead: gru is set and the graph weights are empty.
struct GnnWeights {
  int K = 0, F = 0, H = 0;
  std::vector<float> W1, b1, W2, b2, wr;
  float br = 0;
  std::shared_ptr<const GruWeights> gru;
};
std::shared_ptr<const GnnWeights> load_gnn_weights(const std::string& path, std::string& error);

// A JSON object of numeric (nested) arrays, flattened with their shapes: the weight-file format, also used by tests.
struct JsonArray {
  std::vector<int> shape;
  std::vector<float> data;
};
bool load_json_arrays(const std::string& path, std::unordered_map<std::string, JsonArray>& out, std::string& error);

struct GnnNode {
  long key = 0;  // tid, or -1 for the aggregate node
  std::array<float, kGnnFeatures> x{};
  int numa = 0;
};

// Two delayed graph-filter layers (lab 5 LocalGNN_DB): z_0(t) = u(t), z_k(t) = S(t) z_{k-1}(t-1),
// layer(t) = sum_k z_k(t) W_k + b. History is kept per node key; a node without history reads 0.
class GnnModel {
 public:
  GnnModel(std::shared_ptr<const GnnWeights> weights, std::vector<std::vector<double>> distance);
  // One tick. Returns the predicted demand (cores, 0..1) of every node, in input order.
  std::vector<float> step(const std::vector<GnnNode>& nodes);
  void reset();

 private:
  struct Layer {
    int in = 0, out = 0;
    const float* W = nullptr;
    const float* b = nullptr;
    // z_k(t-1) for k = 0..K-2, per node key.
    std::vector<std::unordered_map<long, std::vector<float>>> prev;
  };
  std::vector<float> layer(Layer& L, const std::vector<long>& keys, const std::vector<float>& u,
                           const std::vector<float>& S, std::size_t N);
  std::shared_ptr<const GnnWeights> w_;
  std::vector<std::vector<double>> dist_;
  Layer l1_, l2_;
};

// NUMA distance matrix from /sys/devices/system/node/nodeN/distance.
std::vector<std::vector<double>> numa_distance();

struct GnnConfig {
  // Scale-down hold: required must stay below the cpuset size for hold_ticks consecutive ticks, then one core
  // is removed and the count restarts (at most one removal per hold). Scale-up is immediate.
  int hold_ticks = 20;
  double busy = 0.1;            // a thread predicted at or above this is pinned to its bin's CPU
  double node_min = 0.02;       // run+wait share that makes a thread its own node
  int node_hold_ticks = 100;    // a node stays a node this many ticks after it was last busy
  // Stacking breaker: when the same >=2 busy threads have shared one CPU for stack_k consecutive ticks and
  // each waited >= stack_wait of the interval, they are forced into separate bins (demand >= 0.51 each)
  // for split_ticks, which grants at most one extra CPU per thread beyond the first. It fires once per
  // group; the group re-arms only after it dissolves. Wait is never demand otherwise.
  int stack_k = 5;
  double stack_wait = 0.2;
  int split_ticks = 50;
};

struct GnnTickStats {
  std::int64_t read_ns = 0, infer_ns = 0;
  int nodes = 0, threads = 0, required = 0, target = 0;
  double demand_sum = 0;
  double pred_sum = 0;  // model output alone, before the breaker
  int breaks = 0;       // stacking-breaker firings this tick
};

class GnnController {
 public:
  GnnController(std::string daemon_id, std::shared_ptr<const GnnWeights> weights, GnnConfig config = {});
  ~GnnController();  // restores every thread it touched to the cpuset-wide mask
  GnnController(const GnnController&) = delete;
  GnnController& operator=(const GnnController&) = delete;

  // Read, infer, bin-pack. Returns the core count to hold now (immediate up, hysteresis down),
  // clamped to [min_cores, max_cores]. -1 when the cgroup is gone.
  int observe(int current_cores, int min_cores, int max_cores, GnnTickStats& stats);
  // Pin the busy threads of the last observe() one bin per CPU of cpus; the rest get the wide mask.
  void pin(const std::vector<int>& cpus);
  void restore();

 private:
  std::string daemon_id_;
  GnnConfig cfg_;
  ThreadStats reader_;
  GnnModel model_;
  std::unique_ptr<GruModel> gru_;  // the gru autoscaler, when the weights are a GRU: sizes only, pins nothing
  GruFeatures gru_features_;
  std::unordered_map<pid_t, int> last_busy_tick_;  // node membership with hold
  std::vector<std::vector<pid_t>> bins_;           // busy threads per bin, from the last observe
  std::unordered_map<pid_t, int> pinned_;          // tid -> CPU it is pinned to
  std::unordered_map<pid_t, bool> wide_;           // tid -> wide mask already applied
  int below_ticks_ = 0;                            // consecutive ticks with required < current
  std::string stack_key_;                          // the stacked group being counted (cpu:tids)
  int stack_count_ = 0;
  std::unordered_map<std::string, bool> fired_;    // groups already broken, until they dissolve
  std::unordered_map<pid_t, long> split_until_;    // tid -> tick its forced split ends
  std::vector<pid_t> live_tids_;                   // every tid of the last observe
  long tick_ = 0;
};

// First-fit-decreasing bin packing of items (cores, each <= 1) into unit bins. Returns, per bin,
// the indices of its items.
std::vector<std::vector<int>> ffd_bins(const std::vector<double>& items);

// Async-signal-safe: resets every thread a GnnController pinned to the all-CPU mask (the cpuset
// then decides). Called from Mercury's SIGINT/SIGTERM handler and at exit.
void gnn_restore_all_affinity_raw();

// Model path: $MERCURY_GNN_MODEL, else <dir of /proc/self/exe>/../models/gnn_v1.json.
std::string gnn_model_path();
