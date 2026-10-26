#pragma once

#include "ring.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// saturate decides on the mean of the last kScaleWindow cgroup samples (640 ms at 10 ms).
inline constexpr int kScaleWindow = 64;
inline constexpr int kDefaultMaxCores = 16;
inline constexpr double kAddOccupancy = 0.85;
inline constexpr double kAddPressure = 0.10;
inline constexpr double kRemoveMargin = 0.05;
inline constexpr double kRemovePressure = 0.02;
inline constexpr double kRunnableAddMargin = 0.5;
// Cgroup ring layout: task-clock, user, system, then pressure (or runnable in the fallback), then
// cores (the cpuset size at that sample).
inline constexpr int kCgroupTaskClock = 0;
inline constexpr int kCgroupPressure = 3;
inline constexpr int kCgroupCores = 4;

enum class Decision { Hold, AddCore, RemoveCore };

// cores is the cpuset the daemon has now. min_cores and max_cores bound the policy.
// cgroup is the daemon cgroup ring, oldest first, only samples newer than the last cpuset change.
// value[0] is task-clock in nanoseconds for that gap. value[3] is the cpu.pressure "some" gap in
// nanoseconds, or, when runnable is true, the count of threads in state R at that tick.
struct OccupancyWindow {
  int cores = 0;
  int min_cores = 1;
  int max_cores = kDefaultMaxCores;
  bool runnable = false;
  std::vector<Sample> cgroup;
};

// Means over the newest count gaps of a cgroup series (count + 1 samples). occupancy is
// task-clock over the summed gaps and cores; pressure is the pressure gap over the summed gaps,
// or the mean runnable count in the fallback. Empty when there are not enough samples, a gap is
// not positive, or a sample lacks the pressure value.
struct WindowMeans {
  double occupancy = 0;
  double pressure = 0;
};
std::optional<WindowMeans> window_means(const std::vector<Sample>& cgroup, int cores, std::size_t count,
                                        bool runnable);

// The add and remove tests of saturate, without the min/max limits.
bool saturate_add_test(const WindowMeans& means, int cores, bool runnable);
bool saturate_remove_test(const WindowMeans& means, int cores, bool runnable);

class Policy {
 public:
  virtual ~Policy() = default;
  virtual Decision decide(const OccupancyWindow& window) = 0;
};

// Add a core when the window mean shows the cpuset full (occupancy >= 0.85) and tasks waiting for a
// CPU (pressure >= 0.10). Remove one when a CPU is clearly spare and nothing waits.
class SaturatePolicy : public Policy {
 public:
  Decision decide(const OccupancyWindow& window) override;
};

// gnn (SPEC.md "gnn policy"): the per-thread controller in src/gnn.cpp runs in monitor_daemon instead of
// decide(); decide() only holds. make_policy returns it for "gnn".
class GnnPolicy : public Policy {
 public:
  Decision decide(const OccupancyWindow&) override { return Decision::Hold; }
};

// l3 pool (SPEC.md "l3 pool"): node scaling (Baselines.pdf eq. 2, scale nodes ind) with an L3 domain
// as the node. The daemons' autoscaling (saturate, gnn) hands out CPUs only from an active pool of whole
// L3 domains, kPoolStartCpus to begin with; when the pool's CPUs are above kPoolAddLoad busy over a full
// window, the pool gains one more L3 domain; when it is nearly idle over a long window, the last domain added
// leaves again.
inline constexpr int kPoolStartCpus = 8;
inline constexpr double kPoolAddLoad = 0.90;
inline constexpr int kPoolWindowMs = 1000;
// Scale-in: at or below kPoolRemoveLoad busy over a kPoolRemoveWindowMs window, the last domain added leaves
// the pool (never below the start pool) and the daemons on it move to the CPUs left.
inline constexpr double kPoolRemoveLoad = 0.10;
inline constexpr int kPoolRemoveWindowMs = 30000;

// One L3 domain. cpus are its allocatable Linux CPUs. For shift and the LLM interface, free are those in
// no running daemon's cpuset and held counts those in the judged daemon's cpuset; for the pool, free are
// those outside the pool and held counts those in it.
struct L3Block {
  std::string name;
  std::string node;
  std::string socket;
  std::vector<int> cpus;
  std::vector<int> free;
  int held = 0;
  double load = 0;
  // The domain has CPUs that are not allocatable (Mercury's own).
  bool partial = false;
};

// The block the next step takes CPUs from, or empty when nothing is available. A block already partly
// held comes first, then a wholly free block on the node of a held block, then on its socket, then
// anywhere. A block partly free but not held is never chosen. Ties keep the lower load, then the
// lower Linux CPU.
std::optional<std::size_t> pick_l3_block(const std::vector<L3Block>& blocks);

// Appends up to room free CPUs of pick_l3_block's choice to have (kept sorted) and marks them held
// in blocks. False when no block is available or room is 0.
bool l3_take_block(std::vector<L3Block>& blocks, std::vector<int>& have, std::size_t room);

// The pool's first L3 domains: whole wholly free domains, nearest first from the one holding seed_cpu
// (or the first wholly allocatable domain when seed_cpu is in none), until at least start CPUs. A domain
// with a CPU that is not allocatable (Mercury's own) is taken only when nothing else is left.
std::vector<int> pool_start(std::vector<L3Block> blocks, std::size_t start, int seed_cpu);

// Pool growth on unmet demand: the services' mean CPU pressure (share of time a thread waited) at or above this
// grows the pool even when its CPUs read under kPoolAddLoad busy (services sharing too few CPUs wait while
// each CPU still has idle gaps; on hotel the 8-CPU pool stayed near 50% busy with every service waiting).
inline constexpr double kPoolAddPressure = 0.25;
// Pool swap (the shift system with a pool): a pool domain whose load from outside Mercury's daemons is at least
// this share of its pooled CPUs is swapped for a clean free domain.
inline constexpr double kPoolSwapForeign = 0.25;
inline constexpr double kPoolSwapCleanLoad = 0.25;

// One swap: blocks[from] (pooled) leaves the pool, blocks[to] (wholly free and allocatable) takes its place.
struct PoolSwap {
  std::size_t from = 0;
  std::size_t to = 0;
};

// blocks from the pool's view (held = pooled CPUs, free = the rest) with window loads; foreign[i] is the CPUs'
// worth of blocks[i]'s load that Mercury's daemons do not account for. The source is the pooled domain with the
// most foreign load at or above share of its pooled CPUs; the destination a domain with no pooled CPU, no CPU
// that is not allocatable, at least as many CPUs as the source pools, and load at or below clean; the lowest
// load wins, then the source's node, then its socket, then the lower Linux CPU. Empty when nothing qualifies.
std::optional<PoolSwap> plan_pool_swap(const std::vector<L3Block>& blocks, const std::vector<double>& foreign,
                                       double share = kPoolSwapForeign, double clean = kPoolSwapCleanLoad);

// throttle (SPEC.md "throttle"): when the whole host is above kThrottleAbove over a full window, the
// daemon's CPU time is cut by kThrottleCut through cpu.max (quota = (1 - cut) of its cpuset). It is
// released when the host, with the throttled daemon's time scaled back up to unthrottled, would
// stay at or below the same line. Independent of the /scale policy; the cpuset is held while throttled.
inline constexpr double kThrottleAbove = 0.80;
inline constexpr double kThrottleCut = 0.75;
inline constexpr int kThrottleWindowMs = 1000;
inline constexpr long kThrottlePeriodUs = 100000;

// cpu.max quota in microseconds per kThrottlePeriodUs for a cpuset of cores CPUs. At least 1000.
long throttle_quota_us(int cores, double cut);

// Release test. host_load is busy over all host CPUs in the window, daemon_cpus the daemon's mean
// CPU use (task-clock over wall time) in that window while throttled, host_cpus the online CPU count.
// Unthrottled the daemon is assumed to use daemon_cpus / (1 - cut), so cut / (1 - cut) of its use
// comes back on top of host_load.
bool throttle_release_test(double host_load, double daemon_cpus, int host_cpus, double cut, double above);

// shift (SPEC.md "shift"): reactive move (Baselines.pdf eqs. 3-4, Sandpiper) with an L3 domain as the
// node. When an L3 domain the daemon touches is above kShiftAbove over a full window, the daemon's
// CPUs in it move to free CPUs of the least-loaded L3 domain that stays at or below the line.
inline constexpr double kShiftAbove = 0.80;
// A move must carry at least this many CPUs of the daemon's use out of the hot domain: moving an idle
// daemon relieves nothing (seen on hotel: hotel-user and the unused memcached 11214 moved, the busy stayed).
inline constexpr double kShiftMinMoved = 0.10;
inline constexpr int kShiftWindowMs = 1000;

// One move. dest is empty when the source is hot but no domain can take the daemon's CPUs from it.
struct ShiftPlan {
  std::size_t source = 0;
  std::optional<std::size_t> dest;
  std::vector<int> leave;
  std::vector<int> take;
};

// blocks carry window loads; have is the daemon's cpuset and daemon_cpus its mean CPU use in the window.
// The source is the hottest domain above `above` that the daemon holds CPUs in (ties: lower Linux CPU),
// skipping those listed in cooling. The CPUs leaving carry daemon_cpus * leave / have of load with them.
// A plan whose moved load is under kShiftMinMoved has no destination (the daemon holds).
// A destination is another domain with that many free CPUs whose load, with the moved load added
// over its CPUs, stays at or below `above`. The lowest current load wins, then the source's node,
// then its socket, then the lower Linux CPU. take is its lowest free CPUs. Empty when nothing is hot.
std::optional<ShiftPlan> plan_shift(const std::vector<L3Block>& blocks, const std::vector<int>& have,
                                    double daemon_cpus, double above,
                                    const std::vector<std::string>& cooling = {});

std::shared_ptr<Policy> make_policy(const std::string& name);
