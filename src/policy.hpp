#pragma once

#include "ring.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

inline constexpr int kScaleWindow = 10;
inline constexpr int kDefaultMaxCores = 16;

enum class Decision { Hold, AddCore, RemoveCore };

// One thread's recent samples, oldest first. The ring belongs to the daemon.
struct ThreadSamples {
  std::vector<std::string> names;
  std::vector<Sample> samples;
};

// cores is the cpuset the daemon has now. min_cores and max_cores bound the policy.
struct OccupancyWindow {
  int cores = 0;
  int min_cores = 1;
  int max_cores = kDefaultMaxCores;
  std::vector<ThreadSamples> threads;
};

class Policy {
 public:
  virtual ~Policy() = default;
  virtual Decision decide(const OccupancyWindow& window) = 0;
};

// Add a core when more threads are busy than the cpuset has and those cores stay full.
// Remove a core as soon as fewer threads are busy, and keep removing until the
// cpuset matches that demand.
class SaturatePolicy : public Policy {
 public:
  Decision decide(const OccupancyWindow& window) override;
};

std::shared_ptr<Policy> make_policy(const std::string& name);

// ---------------------------------------------------------------------------
// Resource and application policies (policies.md). Pure decisions only: the
// loop that feeds them lives with the daemon table in src/daemons.cpp.
//
// These are the Mercury forms of the infrabench rca/controller baselines:
//   reactive_move    -> shift, hotspot trigger (3 of the last 5 intervals > 0.75)
//   scale_nodes_ind  -> shift, mean trigger (mean of the last 5 intervals > 0.80)
//   degrade_optional -> degrade (brownout on operator-listed optional components)
//   scale_out        -> saturate with min = max = 2 inside the current NUMA node
//   scale_out_spread -> saturate with min = max = 2 anywhere in the pool
//   combined         -> one daemon with shift + degrade at staggered thresholds
// ---------------------------------------------------------------------------

// Five 1 s intervals stand in for the 75 s Prometheus window; the group ring holds 60 s.
inline constexpr int kPolicyIntervals = 5;
inline constexpr std::int64_t kIntervalNs = 1000000000LL;
inline constexpr double kHotspotUtil = 0.75;  // reactive_move HOTSPOT_UTIL
inline constexpr int kHotspotHits = 3;        // reactive_move HOTSPOT_HITS
inline constexpr double kNodeTheta = 0.80;    // scale_nodes_ind THETA
inline constexpr double kDegradeLimit = 0.80; // degrade_optional UTIL_LIMIT

// One load per 1 s interval, oldest first. nullopt is an interval with no samples.
using IntervalLoads = std::vector<std::optional<double>>;

// Bucket ring samples into count 1 s intervals ending at end_ns (exclusive of older data).
// Load is busy / (busy + idle + iowait) over the samples in that interval, the same ratio as load_of.
IntervalLoads interval_loads(const std::vector<std::string>& names, const std::vector<Sample>& samples,
                             std::int64_t end_ns, int count = kPolicyIntervals);

// Sandpiper hotspot test: at least hits of the last kPolicyIntervals loads above util.
// Fewer than kPolicyIntervals present intervals is cold.
bool node_hot(const IntervalLoads& loads, double util = kHotspotUtil, int hits = kHotspotHits);

// scale_nodes_ind test: the mean of all kPolicyIntervals loads is above theta. A missing interval is cold.
bool node_overloaded(const IntervalLoads& loads, double theta = kNodeTheta);

// scale_nodes_agg (Baselines.pdf eq. 2, right): the mean over groups of each group's window mean is
// above theta. A group with a missing interval makes the test cold. Pure decision only (not wired).
bool cluster_overloaded(const std::vector<IntervalLoads>& groups, double theta = kNodeTheta);

// Horizontal scaling as written in Baselines.pdf eq. 6 (native OpenFaaS):
// r(t+1) = ceil(r(t) * mean_load / (target * proportion)), clamped to [min, max]. infrabench's
// scale_out module instead pins 2 replicas, which is what the scale_out arm reproduces; this is the
// paper's rule for when the arm is switched to it. Pure decision only (not wired).
int proportional_replicas(int replicas, double mean_load, double target, double proportion, int min_replicas,
                          int max_replicas);

enum class ShiftTrigger { Hotspot, Mean, Either };

struct ShiftRule {
  ShiftTrigger trigger = ShiftTrigger::Hotspot;
  double hot_util = kHotspotUtil;
  double mean_theta = kNodeTheta;
};

bool shift_triggered(const IntervalLoads& loads, const ShiftRule& rule);

// A NUMA node a shift may land on.
struct NodeCandidate {
  std::string name;
  double load = 0;      // latest interval load
  int lowest_cpu = 0;   // tie break, as in least_loaded
  bool hot = false;
  bool inside_ceiling = true;
  int allocatable = 0;  // allocatable CPUs on that node
};

// Lowest-load legal node other than source that can hold need_cpus. Equal load keeps the lower CPU.
std::optional<std::string> shift_destination(const std::string& source, const std::vector<NodeCandidate>& nodes,
                                             int need_cpus);

// least_allocated (planned in infrabench, specified in policies.md). The group with the highest share of
// allocatable CPUs that no running daemon holds; ties keep lower load, then lower CPU.
struct GroupCandidate {
  std::string name;
  int allocatable = 0;
  int unallocated = 0;
  double load = 0;
  int lowest_cpu = 0;
};

std::optional<std::string> least_allocated(const std::vector<GroupCandidate>& groups);

struct DegradeComponent {
  std::string name;
  double cost = 0;  // operator's fraction in (0, 1]
  bool enabled = true;
};

// One brownout step. Disables the costliest enabled component while U - cost(disabled) > limit,
// using the latest interval. Re-enables the cheapest disabled component when all five intervals
// stay at or below limit with its cost added back. Returns the index that changed.
std::optional<std::size_t> degrade_step(std::vector<DegradeComponent>& components, const IntervalLoads& loads,
                                        double limit = kDegradeLimit);

struct TuneParameter {
  std::string name;
  std::vector<std::string> values;  // smallest footprint first, at least two
  std::size_t index = 0;            // starts at values.size() - 1
};

// One ladder step. Toward the front while the latest U > limit or anything is degraded; toward the
// end while all five intervals are at or below limit and nothing is degraded. Returns the index moved.
std::optional<std::size_t> tune_step(std::vector<TuneParameter>& parameters, const IntervalLoads& loads,
                                     bool degraded, double limit = kDegradeLimit);

// zeroctl (infrabench "ours"): the model proposes one action per call. Mercury validates it
// against the live state before anything is applied; an illegal action is a no-op.
struct ZeroctlAction {
  std::string action;     // move | scale_out | degrade | restore | none | stop
  std::string workload;   // daemon_id
  std::string node;       // move: destination group name
  std::string component;  // degrade, restore
  int cpus = 0;           // scale_out
  std::string reason;
};

struct ZeroctlGroup {
  std::string level;
  double load = 0;   // latest interval
  int allocatable = 0;
};

struct ZeroctlDaemon {
  std::vector<std::string> groups;          // groups its cpuset touches (any level)
  int cpus = 0;
  std::map<std::string, bool> components;   // name -> enabled
};

struct ZeroctlView {
  std::map<std::string, ZeroctlDaemon> daemons;  // zeroctl-attached, running
  std::map<std::string, ZeroctlGroup> groups;
  std::string move_level = "node";
  int max_cpus = kDefaultMaxCores;
};

// Empty string when the action may be applied; otherwise why it is rejected.
std::string validate_zeroctl(const ZeroctlAction& action, const ZeroctlView& view);
