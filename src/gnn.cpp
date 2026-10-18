#include "gnn.hpp"

#include <sched.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <climits>
#include <cmath>
#include <map>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>

namespace {

std::int64_t mono() {
  timespec t{};
  ::clock_gettime(CLOCK_MONOTONIC, &t);
  return static_cast<std::int64_t>(t.tv_sec) * 1000000000LL + t.tv_nsec;
}

// ---- weights.json: an object of nested numeric arrays ----
struct Arr {
  std::vector<int> shape;
  std::vector<float> data;
};

class Parser {
 public:
  explicit Parser(const std::string& s) : s_(s) {}
  bool object(std::unordered_map<std::string, Arr>& out) {
    ws();
    if (!eat('{')) return false;
    ws();
    if (eat('}')) return true;
    for (;;) {
      std::string key;
      if (!str(key)) return false;
      ws();
      if (!eat(':')) return false;
      Arr a;
      std::vector<int> counts;
      if (!value(a, 0, counts)) return false;
      a.shape = counts;
      out[key] = std::move(a);
      ws();
      if (eat(',')) { ws(); continue; }
      return eat('}');
    }
  }

 private:
  bool value(Arr& a, std::size_t depth, std::vector<int>& counts) {
    ws();
    if (eat('[')) {
      if (counts.size() <= depth) counts.push_back(-1);
      int n = 0;
      ws();
      if (!eat(']')) {
        for (;;) {
          if (!value(a, depth + 1, counts)) return false;
          ++n;
          ws();
          if (eat(',')) continue;
          if (!eat(']')) return false;
          break;
        }
      }
      if (counts[depth] >= 0 && counts[depth] != n) return false;  // ragged
      counts[depth] = n;
      return true;
    }
    char* end = nullptr;
    const double v = std::strtod(s_.c_str() + i_, &end);
    if (end == s_.c_str() + i_) return false;
    i_ = static_cast<std::size_t>(end - s_.c_str());
    a.data.push_back(static_cast<float>(v));
    return true;
  }
  bool str(std::string& out) {
    if (!eat('"')) return false;
    while (i_ < s_.size() && s_[i_] != '"') out.push_back(s_[i_++]);
    return eat('"');
  }
  void ws() { while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_]))) ++i_; }
  bool eat(char c) { if (i_ < s_.size() && s_[i_] == c) { ++i_; return true; } return false; }
  const std::string& s_;
  std::size_t i_ = 0;
};

float softplus(float x) { return x > 20.f ? x : std::log1p(std::exp(x)); }

// Signal-safe registry of pinned tids.
constexpr int kSlots = 16384;
std::atomic<pid_t> g_pinned[kSlots];
std::mutex g_slot_mu;  // only for insert/erase, never in the signal handler

void slot_add(pid_t tid) {
  std::lock_guard lock(g_slot_mu);
  for (auto& s : g_pinned) if (s.load() == tid) return;
  for (auto& s : g_pinned) {
    pid_t zero = 0;
    if (s.compare_exchange_strong(zero, tid)) return;
  }
}

void slot_del(pid_t tid) {
  std::lock_guard lock(g_slot_mu);
  for (auto& s : g_pinned) {
    pid_t t = tid;
    if (s.compare_exchange_strong(t, 0)) return;
  }
}

cpu_set_t all_cpus() {
  cpu_set_t set;
  CPU_ZERO(&set);
  for (int c = 0; c < CPU_SETSIZE; ++c) CPU_SET(c, &set);
  return set;
}

const cpu_set_t g_all = all_cpus();

bool set_wide(pid_t tid) { return ::sched_setaffinity(tid, sizeof(g_all), &g_all) == 0; }

bool set_one(pid_t tid, int cpu) {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  return ::sched_setaffinity(tid, sizeof(set), &set) == 0;
}

}  // namespace

void gnn_restore_all_affinity_raw() {
  for (auto& s : g_pinned) {
    const pid_t t = s.load(std::memory_order_relaxed);
    if (t > 0) {
      ::sched_setaffinity(t, sizeof(g_all), &g_all);
      s.store(0, std::memory_order_relaxed);
    }
  }
}

std::string gnn_model_path() {
  if (const char* p = std::getenv("MERCURY_GNN_MODEL"); p != nullptr && *p != 0) return p;
  char buf[4096];
  const auto n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  std::string exe = n > 0 ? std::string(buf, static_cast<std::size_t>(n)) : std::string(".");
  const auto slash = exe.rfind('/');
  const std::string dir = slash == std::string::npos ? "." : exe.substr(0, slash);
  return dir + "/../models/gnn_v3.json";
}

std::shared_ptr<const GnnWeights> load_gnn_weights(const std::string& path, std::string& error) {
  std::ifstream in(path);
  if (!in) { error = "cannot read " + path; return nullptr; }
  std::stringstream ss;
  ss << in.rdbuf();
  const auto text = ss.str();
  std::unordered_map<std::string, Arr> m;
  if (!Parser(text).object(m)) { error = "bad json in " + path; return nullptr; }
  for (const char* k : {"g1.W", "g1.b", "g2.W", "g2.b", "readout.weight", "readout.bias"}) {
    if (!m.count(k)) { error = std::string("missing ") + k; return nullptr; }
  }
  auto w = std::make_shared<GnnWeights>();
  const auto& g1 = m["g1.W"];
  const auto& g2 = m["g2.W"];
  if (g1.shape.size() != 3 || g2.shape.size() != 3) { error = "bad filter shape"; return nullptr; }
  w->K = g1.shape[0]; w->F = g1.shape[1]; w->H = g1.shape[2];
  if (w->F != kGnnFeatures || g2.shape[0] != w->K || g2.shape[1] != w->H || g2.shape[2] != w->H ||
      m["g1.b"].data.size() != static_cast<std::size_t>(w->H) || m["g2.b"].data.size() != static_cast<std::size_t>(w->H) ||
      m["readout.weight"].data.size() != static_cast<std::size_t>(w->H) || m["readout.bias"].data.size() != 1) {
    error = "unexpected weight shapes (features must be " + std::to_string(kGnnFeatures) + ")";
    return nullptr;
  }
  w->W1 = g1.data; w->b1 = m["g1.b"].data; w->W2 = g2.data; w->b2 = m["g2.b"].data;
  w->wr = m["readout.weight"].data; w->br = m["readout.bias"].data[0];
  return w;
}

std::vector<std::vector<double>> numa_distance() {
  std::vector<std::vector<double>> d;
  for (int n = 0;; ++n) {
    std::ifstream in("/sys/devices/system/node/node" + std::to_string(n) + "/distance");
    if (!in) break;
    std::vector<double> row;
    double v;
    while (in >> v) row.push_back(v);
    d.push_back(row);
  }
  if (d.empty()) d = {{10.0}};
  return d;
}

GnnModel::GnnModel(std::shared_ptr<const GnnWeights> weights, std::vector<std::vector<double>> distance)
    : w_(std::move(weights)), dist_(std::move(distance)) {
  l1_.in = w_->F; l1_.out = w_->H; l1_.W = w_->W1.data(); l1_.b = w_->b1.data();
  l2_.in = w_->H; l2_.out = w_->H; l2_.W = w_->W2.data(); l2_.b = w_->b2.data();
  reset();
}

void GnnModel::reset() {
  const auto k = static_cast<std::size_t>(std::max(0, w_->K - 1));
  l1_.prev.assign(k, {});
  l2_.prev.assign(k, {});
}

std::vector<float> GnnModel::layer(Layer& L, const std::vector<long>& keys, const std::vector<float>& u,
                                   const std::vector<float>& S, std::size_t N) {
  const int K = w_->K;
  std::vector<float> out(N * static_cast<std::size_t>(L.out), 0.f);
  auto mul_add = [&](const std::vector<float>& z, int k) {
    const float* W = L.W + static_cast<std::size_t>(k) * L.in * L.out;
    for (std::size_t n = 0; n < N; ++n)
      for (int i = 0; i < L.in; ++i) {
        const float zi = z[n * L.in + i];
        if (zi == 0.f) continue;
        const float* row = W + static_cast<std::size_t>(i) * L.out;
        float* o = &out[n * L.out];
        for (int j = 0; j < L.out; ++j) o[j] += zi * row[j];
      }
  };
  mul_add(u, 0);
  std::vector<std::vector<float>> cur(static_cast<std::size_t>(std::max(0, K - 1)));
  if (K > 1) cur[0] = u;
  for (int k = 1; k < K; ++k) {
    // gather z_{k-1}(t-1) for the current nodes, then one hop with S(t)
    std::vector<float> g(N * L.in, 0.f);
    auto& prev = L.prev[static_cast<std::size_t>(k - 1)];
    for (std::size_t n = 0; n < N; ++n) {
      auto it = prev.find(keys[n]);
      if (it != prev.end()) std::copy(it->second.begin(), it->second.end(), g.begin() + static_cast<long>(n * L.in));
    }
    std::vector<float> z(N * L.in, 0.f);
    for (std::size_t a = 0; a < N; ++a)
      for (std::size_t b = 0; b < N; ++b) {
        const float s = S[a * N + b];
        if (s == 0.f) continue;
        for (int i = 0; i < L.in; ++i) z[a * L.in + i] += s * g[b * L.in + i];
      }
    mul_add(z, k);
    if (k < K - 1) cur[static_cast<std::size_t>(k)] = std::move(z);
  }
  for (std::size_t k = 0; k + 1 < static_cast<std::size_t>(K); ++k) {
    auto& map = L.prev[k];
    map.clear();
    for (std::size_t n = 0; n < N; ++n)
      map[keys[n]].assign(cur[k].begin() + static_cast<long>(n * L.in), cur[k].begin() + static_cast<long>((n + 1) * L.in));
  }
  for (std::size_t n = 0; n < N; ++n)
    for (int j = 0; j < L.out; ++j) out[n * L.out + j] += L.b[j];
  return out;
}

std::vector<float> GnnModel::step(const std::vector<GnnNode>& nodes) {
  const std::size_t N = nodes.size();
  if (N == 0) return {};
  std::vector<long> keys(N);
  std::vector<float> x(N * kGnnFeatures);
  for (std::size_t n = 0; n < N; ++n) {
    keys[n] = nodes[n].key;
    std::copy(nodes[n].x.begin(), nodes[n].x.end(), x.begin() + static_cast<long>(n * kGnnFeatures));
  }
  // S: same cgroup (1) + NUMA proximity (10/dist), averaged, no self loops, row-normalised.
  std::vector<float> S(N * N, 0.f);
  const int nn = static_cast<int>(dist_.size());
  for (std::size_t a = 0; a < N; ++a) {
    double sum = 0;
    for (std::size_t b = 0; b < N; ++b) {
      if (a == b) continue;
      const int ia = std::clamp(nodes[a].numa, 0, nn - 1), ib = std::clamp(nodes[b].numa, 0, nn - 1);
      const double dd = ib < static_cast<int>(dist_[static_cast<std::size_t>(ia)].size()) ? dist_[static_cast<std::size_t>(ia)][static_cast<std::size_t>(ib)] : 10.0;
      const double v = 0.5 * (1.0 + 10.0 / dd);
      S[a * N + b] = static_cast<float>(v);
      sum += v;
    }
    const double den = std::max(sum, 1e-9);
    for (std::size_t b = 0; b < N; ++b) S[a * N + b] = static_cast<float>(S[a * N + b] / den);
  }
  auto h = layer(l1_, keys, x, S, N);
  for (auto& v : h) v = std::max(v, 0.f);
  h = layer(l2_, keys, h, S, N);
  for (auto& v : h) v = std::max(v, 0.f);
  std::vector<float> out(N);
  for (std::size_t n = 0; n < N; ++n) {
    float acc = w_->br;
    for (int j = 0; j < w_->H; ++j) acc += h[n * w_->H + j] * w_->wr[j];
    out[n] = std::min(softplus(acc), 1.0f);
  }
  return out;
}

std::vector<std::vector<int>> ffd_bins(const std::vector<double>& items) {
  std::vector<int> order(items.size());
  for (std::size_t i = 0; i < items.size(); ++i) order[i] = static_cast<int>(i);
  std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return items[a] > items[b]; });
  std::vector<std::vector<int>> bins;
  std::vector<double> used;
  for (const int i : order) {
    const double d = std::min(1.0, items[static_cast<std::size_t>(i)]);
    if (d <= 1e-6) continue;
    bool placed = false;
    for (std::size_t b = 0; b < bins.size(); ++b)
      if (used[b] + d <= 1.0 + 1e-6) { used[b] += d; bins[b].push_back(i); placed = true; break; }
    if (!placed) { bins.push_back({i}); used.push_back(d); }
  }
  return bins;
}

GnnController::GnnController(std::string daemon_id, std::shared_ptr<const GnnWeights> weights, GnnConfig config)
    : daemon_id_(std::move(daemon_id)), cfg_(config), reader_("/sys/fs/cgroup/mercury/" + daemon_id_),
      model_(std::move(weights), numa_distance()) {
  // Tuning overrides for experiments (process environment only; nothing persistent).
  auto env_int = [](const char* n, int d) { const char* v = std::getenv(n); return v && *v ? std::atoi(v) : d; };
  cfg_.hold_ticks = env_int("MERCURY_GNN_HOLD_TICKS", cfg_.hold_ticks);
  cfg_.stack_k = env_int("MERCURY_GNN_STACK_K", cfg_.stack_k);
}

GnnController::~GnnController() { restore(); }

void GnnController::restore() {
  for (const auto& [tid, cpu] : pinned_) {
    set_wide(tid);
    slot_del(tid);
  }
  pinned_.clear();
  for (const auto& [tid, done] : wide_) if (!done) set_wide(tid);
  wide_.clear();
}

int GnnController::observe(int current_cores, int min_cores, int max_cores, GnnTickStats& st) {
  ++tick_;
  const auto r = reader_.tick();
  st.read_ns = r.read_ns;
  if (r.cgroup_gone) return -1;
  const auto t0 = mono();
  const double iv = static_cast<double>(r.interval_ns);
  std::vector<const ThreadDelta*> nodes;
  double other_run = 0, other_wait = 0;
  if (iv > 0) {
    for (const auto& t : r.threads) {
      const double share = static_cast<double>(t.run_ns + t.wait_ns) / iv;
      if (!t.fresh && share >= cfg_.node_min) last_busy_tick_[t.tid] = static_cast<int>(tick_);
      const auto it = last_busy_tick_.find(t.tid);
      if (it != last_busy_tick_.end() && tick_ - it->second <= cfg_.node_hold_ticks) {
        nodes.push_back(&t);
      } else {
        other_run += static_cast<double>(t.run_ns) / iv;
        other_wait += static_cast<double>(t.wait_ns) / iv;
      }
    }
  }
  // forget threads that left
  std::unordered_map<pid_t, bool> live;
  live_tids_.clear();
  for (const auto& t : r.threads) { live[t.tid] = true; live_tids_.push_back(t.tid); }
  for (auto it = last_busy_tick_.begin(); it != last_busy_tick_.end();) it = live.count(it->first) ? std::next(it) : last_busy_tick_.erase(it);
  for (auto it = pinned_.begin(); it != pinned_.end();) {
    if (!live.count(it->first)) { slot_del(it->first); it = pinned_.erase(it); } else ++it;
  }
  for (auto it = wide_.begin(); it != wide_.end();) it = live.count(it->first) ? std::next(it) : wide_.erase(it);

  std::sort(nodes.begin(), nodes.end(), [](const ThreadDelta* a, const ThreadDelta* b) { return a->tid < b->tid; });
  const int cpus = r.cgroup.cpuset_cpus;
  const float occ = iv > 0 ? static_cast<float>(r.cgroup.usage_ns / iv / std::max(1, cpus)) : 0.f;
  const float psi = iv > 0 ? static_cast<float>(r.cgroup.some_ns / iv) : 0.f;
  std::vector<GnnNode> in;
  in.reserve(nodes.size() + 1);
  for (const auto* t : nodes) {
    GnnNode n;
    n.key = t->tid;
    if (iv > 0) {
      n.x[0] = static_cast<float>(t->task_clock_ns / iv);
      n.x[1] = static_cast<float>(t->run_ns / iv);
      n.x[2] = static_cast<float>(t->wait_ns / iv);
      n.x[3] = static_cast<float>(t->context_switches * 1e9 / iv / 1000.0);
      n.x[4] = static_cast<float>(t->cpu_migrations * 1e9 / iv / 100.0);
    }
    n.numa = std::max(0, t->node);
    in.push_back(n);
  }
  GnnNode agg;
  agg.key = -1;
  agg.x[0] = static_cast<float>(other_run);
  agg.x[1] = static_cast<float>(other_run);
  agg.x[2] = static_cast<float>(other_wait);
  agg.x[8] = 1.f;
  agg.numa = in.empty() ? 0 : in.front().numa;
  in.push_back(agg);
  for (auto& n : in) {
    n.x[5] = occ;
    n.x[6] = psi;
    n.x[7] = static_cast<float>(cpus) / 16.f;
    if (n.numa >= 0 && n.numa < 8) n.x[9 + n.numa] = 1.f;
  }
  const auto pred = iv > 0 ? model_.step(in) : std::vector<float>(in.size(), 0.f);
  // Items: every thread node's predicted demand, plus the aggregate's measured run+wait.
  std::vector<double> items;
  // Demand = prediction. The only exception is the stacking breaker (see GnnConfig).
  std::vector<float> dem(pred.begin(), pred.begin() + static_cast<long>(nodes.size()));
  st.pred_sum = 0;
  for (const float p : dem) st.pred_sum += p;
  if (iv > 0) {
    std::map<int, std::vector<pid_t>> stacked;  // cpu -> waiting busy threads seen on it
    for (std::size_t i = 0; i < nodes.size(); ++i) {
      const auto* t = nodes[i];
      if (t->cpu < 0 || static_cast<double>(t->wait_ns) / iv < cfg_.stack_wait || dem[i] < cfg_.busy) continue;
      stacked[t->cpu].push_back(t->tid);
    }
    std::vector<std::string> keys;
    std::map<std::string, std::vector<pid_t>> groups;
    for (auto& [cpu, v] : stacked) {
      if (v.size() < 2) continue;
      std::sort(v.begin(), v.end());
      std::string k = std::to_string(cpu);
      for (const pid_t x : v) k += ":" + std::to_string(x);
      groups[k] = v;
    }
    // re-arm groups that dissolved
    for (auto it = fired_.begin(); it != fired_.end();) it = groups.count(it->first) ? std::next(it) : fired_.erase(it);
    // count the persistence of one group at a time (the first unfired one, stable by key order)
    std::string cand;
    if (groups.count(stack_key_) && !fired_.count(stack_key_)) cand = stack_key_;
    else for (const auto& [k, v] : groups) if (!fired_.count(k)) { cand = k; break; }
    if (cand.empty()) { stack_key_.clear(); stack_count_ = 0; }
    else if (cand == stack_key_) ++stack_count_;
    else { stack_key_ = cand; stack_count_ = 1; }
    if (!cand.empty() && stack_count_ >= cfg_.stack_k) {
      for (const pid_t x : groups[cand]) split_until_[x] = tick_ + cfg_.split_ticks;
      fired_[cand] = true;
      stack_key_.clear();
      stack_count_ = 0;
      st.breaks = 1;
    }
    for (auto it = split_until_.begin(); it != split_until_.end();) it = it->second < tick_ ? split_until_.erase(it) : std::next(it);
    for (std::size_t i = 0; i < nodes.size(); ++i)
      if (split_until_.count(nodes[i]->tid)) dem[i] = std::max(dem[i], 0.51f);
  }
  for (const float d : dem) items.push_back(d);
  const auto bins = ffd_bins(items);
  bins_.clear();
  for (const auto& b : bins) {
    std::vector<pid_t> busy;
    for (const int i : b)
      if (i < static_cast<int>(nodes.size()) && dem[static_cast<std::size_t>(i)] >= cfg_.busy) busy.push_back(nodes[static_cast<std::size_t>(i)]->tid);
    if (!busy.empty()) bins_.push_back(std::move(busy));
  }
  st.infer_ns = mono() - t0;
  st.nodes = static_cast<int>(in.size());
  st.threads = static_cast<int>(r.threads.size());
  st.demand_sum = 0;
  for (const double d : items) st.demand_sum += d;
  const int required = std::clamp(std::max(1, static_cast<int>(bins.size())), min_cores, max_cores);
  st.required = required;
  if (iv <= 0) { st.target = current_cores; return current_cores; }
  int target = current_cores;
  if (required > current_cores) {
    target = required;  // immediate scale up
    below_ticks_ = 0;
  } else if (required < current_cores) {
    if (++below_ticks_ >= cfg_.hold_ticks) { target = current_cores - 1; below_ticks_ = 0; }
  } else {
    below_ticks_ = 0;
  }
  target = std::clamp(target, min_cores, max_cores);
  st.target = target;
  return target;
}

void GnnController::pin(const std::vector<int>& cpus) {
  if (cpus.empty()) return;
  std::unordered_map<int, bool> taken;
  std::unordered_map<pid_t, int> want;
  std::vector<std::size_t> unassigned;
  // Sticky: a bin keeps a CPU one of its threads is already pinned to.
  std::vector<int> bin_cpu(bins_.size(), -1);
  for (std::size_t b = 0; b < bins_.size(); ++b) {
    for (const pid_t t : bins_[b]) {
      const auto it = pinned_.find(t);
      if (it != pinned_.end() && std::binary_search(cpus.begin(), cpus.end(), it->second) && !taken[it->second]) {
        bin_cpu[b] = it->second;
        taken[it->second] = true;
        break;
      }
    }
    if (bin_cpu[b] < 0) unassigned.push_back(b);
  }
  std::size_t next = 0;
  for (const std::size_t b : unassigned) {
    while (next < cpus.size() && taken[cpus[next]]) ++next;
    if (next < cpus.size()) {
      bin_cpu[b] = cpus[next];
      taken[cpus[next]] = true;
    } else {
      bin_cpu[b] = cpus[b % cpus.size()];  // cores exhausted: bins share CPUs
    }
  }
  for (std::size_t b = 0; b < bins_.size(); ++b)
    for (const pid_t t : bins_[b]) want[t] = bin_cpu[b];
  // unpin threads that are no longer busy
  for (auto it = pinned_.begin(); it != pinned_.end();) {
    if (!want.count(it->first)) {
      set_wide(it->first);
      slot_del(it->first);
      wide_[it->first] = true;
      it = pinned_.erase(it);
    } else {
      ++it;
    }
  }
  for (const auto& [t, cpu] : want) {
    const auto it = pinned_.find(t);
    if (it != pinned_.end() && it->second == cpu) continue;
    if (set_one(t, cpu)) {
      if (it == pinned_.end()) slot_add(t);
      pinned_[t] = cpu;
      wide_.erase(t);
    }
  }
  // every other live thread gets the wide mask once (new threads may have inherited a pin)
  for (const pid_t tid : live_tids_) {
    if (!pinned_.count(tid) && !wide_.count(tid)) { set_wide(tid); wide_[tid] = true; }
  }
}
