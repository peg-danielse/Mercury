#include "gnn_rca.hpp"

#include "gnn.hpp"
#include "json.hpp"
#include "threadstat.hpp"

#include <time.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace {

struct Tick {
  int cpus = 0;
  int required = 0;
  float pred = 0;
  int threads = 0;
};

struct Tracked {
  std::unique_ptr<GnnController> controller;
  std::deque<Tick> ticks;
  int cpus = 0;
  long since_cpus = 0;
};

std::mutex mu;
std::set<std::string> wanted;
GnnRcaConfig cfg;
std::map<std::string, Tracked> tracked;  // only touched by the sampler thread, summaries copied under mu
std::map<std::string, std::deque<Tick>> published;
std::shared_ptr<const GnnWeights> weights;
std::string model_path, model_error;
bool running = false;

int cpuset_size(const std::string& daemon_id) {
  std::ifstream in("/sys/fs/cgroup/mercury/" + daemon_id + "/cpuset.cpus.effective");
  std::string text;
  std::getline(in, text);
  int count = 0;
  std::stringstream list(text);
  std::string part;
  while (std::getline(list, part, ',')) {
    const auto dash = part.find('-');
    try {
      if (dash == std::string::npos) {
        if (!part.empty()) {
          std::stoi(part);
          ++count;
        }
      } else {
        count += std::stoi(part.substr(dash + 1)) - std::stoi(part.substr(0, dash)) + 1;
      }
    } catch (...) {
    }
  }
  return count;
}

void sampler() {
  timespec next{};
  ::clock_gettime(CLOCK_MONOTONIC, &next);
  for (;;) {
    std::set<std::string> ids;
    GnnRcaConfig config;
    std::shared_ptr<const GnnWeights> w;
    {
      std::lock_guard lock(mu);
      if (wanted.empty()) {
        tracked.clear();  // readers close their perf fds; nothing was pinned
        published.clear();
        running = false;
        return;
      }
      ids = wanted;
      config = cfg;
      w = weights;
    }
    for (auto it = tracked.begin(); it != tracked.end();) {
      it = ids.count(it->first) ? std::next(it) : tracked.erase(it);
    }
    const std::size_t keep = static_cast<std::size_t>(std::max(1, config.window_s * 1000 / std::max(1, config.tick_ms)));
    for (const auto& id : ids) {
      auto& t = tracked[id];
      if (!w) {
        continue;
      }
      if (t.since_cpus-- <= 0) {
        t.cpus = cpuset_size(id);
        t.since_cpus = std::max(1, 1000 / std::max(1, config.tick_ms));
      }
      if (t.cpus <= 0) {
        t.controller.reset();
        continue;
      }
      if (!t.controller) {
        t.controller = std::make_unique<GnnController>(id, w);
      }
      GnnTickStats st;
      if (t.controller->observe(t.cpus, 1, std::max(1, config.max_cpus), st) < 0) {
        t.controller.reset();  // cgroup gone; a later tick starts over
        continue;
      }
      t.ticks.push_back({t.cpus, st.required, static_cast<float>(st.pred_sum), st.threads});
      while (t.ticks.size() > keep) {
        t.ticks.pop_front();
      }
    }
    {
      std::lock_guard lock(mu);
      for (const auto& [id, t] : tracked) {
        published[id] = t.ticks;
      }
      for (auto it = published.begin(); it != published.end();) {
        it = tracked.count(it->first) ? std::next(it) : published.erase(it);
      }
    }
    next.tv_nsec += static_cast<long>(config.tick_ms) * 1000000L;
    while (next.tv_nsec >= 1000000000L) {
      next.tv_nsec -= 1000000000L;
      ++next.tv_sec;
    }
    timespec now{};
    ::clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec > next.tv_sec + 1) {
      next = now;  // fell far behind: do not burst
    }
    ::clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, nullptr);
  }
}

std::string num(double value) {
  std::ostringstream out;
  out.precision(4);
  out << value;
  return out.str();
}

}  // namespace

void gnn_rca_track(const std::set<std::string>& daemon_ids, const GnnRcaConfig& config) {
  std::lock_guard lock(mu);
  wanted = config.enabled ? daemon_ids : std::set<std::string>{};
  cfg = config;
  cfg.tick_ms = std::clamp(cfg.tick_ms, 1, 1000);
  cfg.window_s = std::clamp(cfg.window_s, 1, 60);
  if (wanted.empty()) {
    return;
  }
  const auto path = gnn_model_path();
  if (!weights || path != model_path) {
    std::string error;
    weights = load_gnn_weights(path, error);
    model_path = path;
    model_error = weights ? "" : error;
  }
  if (!running && weights) {
    raise_nofile_limit();  // 5 fds per observed thread
    running = true;
    std::thread(sampler).detach();
  }
}

std::string gnn_rca_json() {
  std::lock_guard lock(mu);
  std::ostringstream out;
  out << "{\"enabled\":" << (!wanted.empty() ? "true" : "false") << ",\"available\":"
      << (weights && !wanted.empty() ? "true" : "false") << ",\"model\":\"" << json_escape(model_path)
      << "\",\"error\":\"" << json_escape(model_error) << "\",\"tick_ms\":" << cfg.tick_ms
      << ",\"window_s\":" << cfg.window_s << ",\"daemons\":[";
  bool first = true;
  for (const auto& [id, ticks] : published) {
    if (ticks.empty()) {
      continue;
    }
    double required = 0, pred = 0, threads = 0;
    int required_max = 0, under = 0;
    std::vector<int> sorted;
    for (const auto& t : ticks) {
      required += t.required;
      pred += t.pred;
      threads += t.threads;
      required_max = std::max(required_max, t.required);
      under += t.required > t.cpus ? 1 : 0;
      sorted.push_back(t.required);
    }
    std::sort(sorted.begin(), sorted.end());
    const double n = static_cast<double>(ticks.size());
    const int p90 = sorted[std::min(sorted.size() - 1, static_cast<std::size_t>(0.9 * n))];
    out << (first ? "" : ",") << "{\"daemon_id\":\"" << json_escape(id) << "\",\"cpus\":" << ticks.back().cpus
        << ",\"ticks\":" << ticks.size() << ",\"required_mean\":" << num(required / n)
        << ",\"required_max\":" << required_max << ",\"required_p90\":" << p90
        << ",\"under_share\":" << num(under / n) << ",\"pred_demand_mean\":" << num(pred / n)
        << ",\"threads_mean\":" << num(threads / n) << '}';
    first = false;
  }
  out << "]}";
  return out.str();
}
