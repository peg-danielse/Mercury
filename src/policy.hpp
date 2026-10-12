#pragma once

#include "ring.hpp"

#include <memory>
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
