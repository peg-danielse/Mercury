#include "policy.hpp"

#include <algorithm>

std::optional<WindowMeans> window_means(const std::vector<Sample>& cgroup, int cores, std::size_t count,
                                        bool runnable) {
  if (cores < 1 || count == 0 || cgroup.size() < count + 1) {
    return std::nullopt;
  }
  const std::size_t first = cgroup.size() - count;
  double used = 0;
  double pressure = 0;
  for (std::size_t i = first; i < cgroup.size(); ++i) {
    const auto& sample = cgroup[i];
    if (sample.timestamp_ns <= cgroup[i - 1].timestamp_ns || sample.nvalues <= kCgroupPressure) {
      return std::nullopt;
    }
    used += static_cast<double>(sample.value[kCgroupTaskClock]);
    pressure += static_cast<double>(sample.value[kCgroupPressure]);
  }
  const auto span = static_cast<double>(cgroup.back().timestamp_ns - cgroup[first - 1].timestamp_ns);
  WindowMeans means;
  means.occupancy = used / span / static_cast<double>(cores);
  means.pressure = runnable ? pressure / static_cast<double>(count) : pressure / span;
  return means;
}

bool saturate_add_test(const WindowMeans& means, int cores, bool runnable) {
  if (means.occupancy < kAddOccupancy) {
    return false;
  }
  return runnable ? means.pressure >= static_cast<double>(cores) + kRunnableAddMargin : means.pressure >= kAddPressure;
}

bool saturate_remove_test(const WindowMeans& means, int cores, bool runnable) {
  if (cores < 2) {
    return false;
  }
  const double fits = static_cast<double>(cores - 1) / static_cast<double>(cores);
  const double line = fits - kRemoveMargin;
  const bool idle_queue =
      runnable ? means.pressure <= static_cast<double>(cores - 1) : means.pressure <= kRemovePressure;
  // Margin rule: clearly under one core less, and nothing waits.
  if (means.occupancy <= line && idle_queue) {
    return true;
  }
  // Relaxed rule: with no run-queue pressure the cgroup fits on one core less
  // whenever its mean occupancy does, without the margin. Without it, N busy
  // threads that picked up an (N+1)th CPU (N/(N+1) > N/(N+1) - margin) would
  // never give it back.
  return means.occupancy <= fits && idle_queue;
}

Decision SaturatePolicy::decide(const OccupancyWindow& window) {
  if (window.cores < 1) {
    return Decision::Hold;
  }
  if (window.cores < window.min_cores) {
    return Decision::AddCore;
  }
  if (window.cores > window.max_cores) {
    return Decision::RemoveCore;
  }
  const auto means =
      window_means(window.cgroup, window.cores, static_cast<std::size_t>(kScaleWindow), window.runnable);
  if (!means) {
    return Decision::Hold;
  }
  if (window.cores < window.max_cores && saturate_add_test(*means, window.cores, window.runnable)) {
    return Decision::AddCore;
  }
  if (window.cores > window.min_cores && saturate_remove_test(*means, window.cores, window.runnable)) {
    return Decision::RemoveCore;
  }
  return Decision::Hold;
}

std::optional<std::size_t> pick_l3_block(const std::vector<L3Block>& blocks) {
  std::vector<std::string> held_nodes;
  std::vector<std::string> held_sockets;
  for (const auto& block : blocks) {
    if (block.held > 0) {
      held_nodes.push_back(block.node);
      held_sockets.push_back(block.socket);
    }
  }
  const auto has = [](const std::vector<std::string>& names, const std::string& name) {
    return std::find(names.begin(), names.end(), name) != names.end();
  };
  // 0: partly held, 1: unused on a held node, 2: unused on a held socket, 3: unused anywhere.
  const auto tier = [&](const L3Block& block) {
    if (block.held > 0) {
      return 0;
    }
    if (block.free.size() != block.cpus.size()) {
      return -1;
    }
    return has(held_nodes, block.node) ? 1 : has(held_sockets, block.socket) ? 2 : 3;
  };
  std::optional<std::size_t> best;
  for (std::size_t i = 0; i < blocks.size(); ++i) {
    const auto& block = blocks[i];
    const int t = tier(block);
    if (block.free.empty() || t < 0) {
      continue;
    }
    if (!best) {
      best = i;
      continue;
    }
    const auto& other = blocks[*best];
    const int ot = tier(other);
    if (t != ot ? t < ot
                : block.load != other.load ? block.load < other.load
                                           : block.cpus.front() < other.cpus.front()) {
      best = i;
    }
  }
  return best;
}

bool l3_take_block(std::vector<L3Block>& blocks, std::vector<int>& have, std::size_t room) {
  const auto pick = pick_l3_block(blocks);
  if (!pick || room == 0) {
    return false;
  }
  auto& block = blocks[*pick];
  std::sort(block.free.begin(), block.free.end());
  const auto take = std::min(room, block.free.size());
  have.insert(have.end(), block.free.begin(), block.free.begin() + static_cast<std::ptrdiff_t>(take));
  block.free.erase(block.free.begin(), block.free.begin() + static_cast<std::ptrdiff_t>(take));
  block.held += static_cast<int>(take);
  std::sort(have.begin(), have.end());
  return true;
}

long throttle_quota_us(int cores, double cut) {
  const auto quota = static_cast<long>(static_cast<double>(std::max(cores, 1)) * (1 - cut) * kThrottlePeriodUs);
  return std::max(quota, 1000L);
}

bool throttle_release_test(double host_load, double daemon_cpus, int host_cpus, double cut, double above) {
  if (host_cpus < 1 || cut >= 1) {
    return false;
  }
  const double back = daemon_cpus * cut / (1 - cut) / static_cast<double>(host_cpus);
  return host_load + back <= above;
}

std::optional<ShiftPlan> plan_shift(const std::vector<L3Block>& blocks, const std::vector<int>& have,
                                    double daemon_cpus, double above, const std::vector<std::string>& cooling) {
  std::optional<std::size_t> source;
  for (std::size_t i = 0; i < blocks.size(); ++i) {
    const auto& block = blocks[i];
    if (block.held == 0 || block.load <= above ||
        std::find(cooling.begin(), cooling.end(), block.name) != cooling.end()) {
      continue;
    }
    if (!source || block.load > blocks[*source].load ||
        (block.load == blocks[*source].load && block.cpus.front() < blocks[*source].cpus.front())) {
      source = i;
    }
  }
  if (!source || have.empty()) {
    return std::nullopt;
  }
  const auto& from = blocks[*source];
  ShiftPlan plan;
  plan.source = *source;
  for (const int cpu : have) {
    if (std::find(from.cpus.begin(), from.cpus.end(), cpu) != from.cpus.end()) {
      plan.leave.push_back(cpu);
    }
  }
  const double moved = daemon_cpus * static_cast<double>(plan.leave.size()) / static_cast<double>(have.size());
  if (moved < kShiftMinMoved) {
    return plan;
  }
  // 0: same node as the source, 1: same socket, 2: elsewhere.
  const auto near = [&](const L3Block& block) { return block.node == from.node ? 0 : block.socket == from.socket ? 1 : 2; };
  for (std::size_t i = 0; i < blocks.size(); ++i) {
    const auto& block = blocks[i];
    if (i == *source || block.free.size() < plan.leave.size()) {
      continue;
    }
    const auto size = static_cast<double>(block.cpus.size());
    if ((block.load * size + moved) / size > above) {
      continue;
    }
    if (plan.dest) {
      const auto& best = blocks[*plan.dest];
      if (block.load != best.load ? block.load > best.load
          : near(block) != near(best) ? near(block) > near(best)
                                      : block.cpus.front() > best.cpus.front()) {
        continue;
      }
    }
    plan.dest = i;
  }
  if (plan.dest) {
    auto free = blocks[*plan.dest].free;
    std::sort(free.begin(), free.end());
    plan.take.assign(free.begin(), free.begin() + static_cast<std::ptrdiff_t>(plan.leave.size()));
  }
  return plan;
}

std::vector<int> pool_start(std::vector<L3Block> blocks, std::size_t start, int seed_cpu) {
  std::vector<int> pool;
  if (blocks.empty() || start == 0) {
    return pool;
  }
  const auto complete = [](const L3Block& block) { return !block.partial && block.free.size() == block.cpus.size(); };
  std::optional<std::size_t> seed;
  for (std::size_t i = 0; i < blocks.size() && !seed; ++i) {
    if (std::find(blocks[i].cpus.begin(), blocks[i].cpus.end(), seed_cpu) != blocks[i].cpus.end()) {
      seed = i;
    }
  }
  for (std::size_t i = 0; i < blocks.size() && !seed; ++i) {
    if (complete(blocks[i])) {
      seed = i;
    }
  }
  if (!seed) {
    seed = 0;
  }
  const auto take = [&](std::size_t i) {
    pool.insert(pool.end(), blocks[i].cpus.begin(), blocks[i].cpus.end());
    blocks[i].held = static_cast<int>(blocks[i].cpus.size());
    blocks[i].free.clear();
  };
  take(*seed);
  const auto& from = blocks[*seed];
  // 0: same node, 1: same socket, 2: elsewhere; a short (partly allocatable) domain goes after them all.
  const auto rank = [&](const L3Block& block) {
    const int near = block.node == from.node ? 0 : block.socket == from.socket ? 1 : 2;
    return block.partial ? near + 3 : near;
  };
  while (pool.size() < start) {
    std::optional<std::size_t> best;
    for (std::size_t i = 0; i < blocks.size(); ++i) {
      if (blocks[i].held > 0 || blocks[i].free.empty()) {
        continue;
      }
      if (!best || rank(blocks[i]) < rank(blocks[*best]) ||
          (rank(blocks[i]) == rank(blocks[*best]) && blocks[i].cpus.front() < blocks[*best].cpus.front())) {
        best = i;
      }
    }
    if (!best) {
      break;
    }
    take(*best);
  }
  std::sort(pool.begin(), pool.end());
  return pool;
}

std::optional<PoolSwap> plan_pool_swap(const std::vector<L3Block>& blocks, const std::vector<double>& foreign,
                                       double share, double clean) {
  std::optional<std::size_t> from;
  for (std::size_t i = 0; i < blocks.size() && i < foreign.size(); ++i) {
    if (blocks[i].held == 0 || foreign[i] < share * blocks[i].held) {
      continue;
    }
    if (!from || foreign[i] > foreign[*from]) {
      from = i;
    }
  }
  if (!from) {
    return std::nullopt;
  }
  const auto& src = blocks[*from];
  const auto near = [&](const L3Block& b) { return b.node == src.node ? 0 : b.socket == src.socket ? 1 : 2; };
  std::optional<std::size_t> to;
  for (std::size_t i = 0; i < blocks.size(); ++i) {
    const auto& b = blocks[i];
    if (b.held > 0 || b.partial || b.free.size() != b.cpus.size() || static_cast<int>(b.cpus.size()) < src.held ||
        b.load > clean) {
      continue;
    }
    if (to) {
      const auto& best = blocks[*to];
      if (b.load != best.load ? b.load > best.load
          : near(b) != near(best) ? near(b) > near(best)
                                  : b.cpus.front() > best.cpus.front()) {
        continue;
      }
    }
    to = i;
  }
  if (!to) {
    return std::nullopt;
  }
  return PoolSwap{*from, *to};
}

std::shared_ptr<Policy> make_policy(const std::string& name) {
  if (name == "saturate") {
    return std::make_shared<SaturatePolicy>();
  }
  if (name == "gnn") {
    return std::make_shared<GnnPolicy>();
  }
  return nullptr;
}
