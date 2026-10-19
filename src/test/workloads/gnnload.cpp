// gnnload: synthetic per-thread CPU workload with a known demand schedule (SPEC.md "gnn policy").
// Every worker uses demand x 10 ms of thread CPU time in each 10 ms slice and sleeps the rest (the
// spinner / PARTIAL_BURN_PCT duty-cycle model, measured in CPU time, on staggered absolute deadlines).
// Demand is piecewise constant per phase; the schedule is written as ground-truth labels.
//
// Configuration, from the environment (Mercury /start "env") or --key=value arguments:
//   GNN_THREADS   workers (default 4)
//   GNN_PATTERN   uniform | hot | geometric | bursty | ramp | exp | expraw   (default uniform)
//   GNN_TOTAL     total demand in cores across workers (default 2.0); each worker is capped at 1.0
//   GNN_PHASE_MS  phase length (default 1000)
//   GNN_RAMP_S    ramp period: 0 -> TOTAL -> 0 (default 20)
//   GNN_RATIO     geometric ratio (default 0.5)
//   GNN_SEED      bursty / exp / expraw seed (default 1)
//   GNN_RESAMPLE  exp/expraw: draw new per-thread demands every N phases (default 1)
//   GNN_EXP_RATE  exp/expraw: rate multiplier of the exponential draws; 0 (default) = randomized per
//                 draw, log-uniform in [0.5, 2] (v4 dataset, SPEC.md)
//   exp:    shares X_i ~ Exp(1) (i.i.d. per thread and draw), demand_i = TOTAL * X_i / sum X, then
//           water-filled to the 1-core cap (excess goes to uncapped workers in proportion), so the
//           sum stays TOTAL when TOTAL <= threads. The rate cancels in the normalisation.
//   expraw: demand_i = min(1, X_i), X_i ~ Exp(rate) with mean TOTAL/threads / multiplier; the per-phase
//           sum varies (TOTAL is the expected uncapped sum).
//   Both quantised to 1e-4 cores, so the labels (%.4f) are exactly the demand the workers burn.
//   GNN_DURATION_S  exit after this many seconds (default 0: run until killed)
//   GNN_LABELS    labels CSV path (default: none)
// Labels CSV: header "# gnnload pid=.. pattern=.. threads=.. total=.. phase_ms=..", then
// "phase_start_ns,phase,thread,tid,demand" with CLOCK_MONOTONIC times; a final row with
// phase=-1 marks the end. The main thread is idle (it only writes labels): demand 0, not listed.

#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr std::int64_t kSliceNs = 10000000;

int threads = 4;
std::string pattern = "uniform";
double total = 2.0;
std::int64_t phase_ns = 1000000000;
double ramp_s = 20;
double ratio = 0.5;
unsigned seed = 1;
std::int64_t resample = 1;
double exp_rate = 0;
double duration_s = 0;
std::string labels;

std::int64_t start_ns = 0;
std::vector<std::atomic<int>> tids(1024);
volatile std::uint64_t sink;

std::int64_t now_ns() {
  timespec t{};
  clock_gettime(CLOCK_MONOTONIC, &t);
  return static_cast<std::int64_t>(t.tv_sec) * 1000000000LL + t.tv_nsec;
}

std::uint64_t mix(std::uint64_t x) {
  x += 0x9E3779B97F4A7C15ULL;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
  return x ^ (x >> 31);
}

double clamp01(double v) { return std::max(0.0, std::min(1.0, v)); }

double unit(std::uint64_t key) { return (static_cast<double>(mix(key) >> 11) + 0.5) / 9007199254740992.0; }  // (0,1)

// exp / expraw demands of all workers for draw e (cached: workers and the label writer call this
// for the same draw; pure function of seed, e, threads, total).
std::vector<double> exp_draw(std::int64_t e) {
  const int n = threads;
  const auto key = [&](std::uint64_t k) { return seed * 0x100000001B3ULL + static_cast<std::uint64_t>(e) * 1000003ULL + k; };
  const double mult = exp_rate > 0 ? exp_rate : std::exp(std::log(0.5) + unit(key(7777777)) * std::log(4.0));
  std::vector<double> d(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) d[static_cast<std::size_t>(i)] = -std::log(unit(key(static_cast<std::uint64_t>(i)))) / mult;
  if (pattern == "exp") {
    double sum = 0;
    for (double v : d) sum += v;
    for (double& v : d) v = total * v / sum;
    std::vector<bool> capped(static_cast<std::size_t>(n), false);
    for (int it = 0; it < n; ++it) {  // water-fill to the 1-core cap
      double excess = 0, free_sum = 0;
      for (int i = 0; i < n; ++i) {
        auto& v = d[static_cast<std::size_t>(i)];
        if (v > 1.0) { excess += v - 1.0; v = 1.0; capped[static_cast<std::size_t>(i)] = true; }
      }
      if (excess <= 1e-12) break;
      for (int i = 0; i < n; ++i) if (!capped[static_cast<std::size_t>(i)]) free_sum += d[static_cast<std::size_t>(i)];
      if (free_sum <= 0) break;
      for (int i = 0; i < n; ++i) if (!capped[static_cast<std::size_t>(i)]) d[static_cast<std::size_t>(i)] += excess * d[static_cast<std::size_t>(i)] / free_sum;
    }
  } else {
    for (double& v : d) v *= total / n;  // Exp(1)/mult scaled to mean total/n/mult
  }
  for (double& v : d) v = std::round(clamp01(v) * 10000) / 10000;
  return d;
}

double exp_demand(int i, std::int64_t p) {
  thread_local std::int64_t cached = -1;
  thread_local std::vector<double> d;
  const std::int64_t e = p / resample;
  if (e != cached) { d = exp_draw(e); cached = e; }
  return d[static_cast<std::size_t>(i)];
}

// Ground-truth demand (cores) of worker i in phase p. Pure function: workers and labels agree.
double demand(int i, std::int64_t p) {
  const int n = threads;
  if (pattern == "uniform") return clamp01(total / n);
  if (pattern == "hot") {
    const double hot = clamp01(total);
    if (i == 0) return hot;
    return n > 1 ? clamp01((total - hot) / (n - 1) + 0.05) : 0.0;  // cold workers get >= 5%
  }
  if (pattern == "geometric") {
    double sum = 0;
    for (int k = 0; k < n; ++k) sum += std::pow(ratio, k);
    return clamp01(total * std::pow(ratio, i) / sum);
  }
  if (pattern == "bursty") {
    const bool on = (mix(seed * 1000003ULL + static_cast<std::uint64_t>(p) * 4099ULL + static_cast<std::uint64_t>(i)) & 1) != 0;
    return on ? clamp01(2.0 * total / n) : 0.05;
  }
  if (pattern == "ramp") {
    const double t = static_cast<double>(p * phase_ns) / 1e9;
    const double frac = std::fmod(t, ramp_s) / ramp_s;  // 0..1
    const double level = frac < 0.5 ? 2 * frac : 2 * (1 - frac);
    return clamp01(std::round(total * level * 20) / 20 / n);  // 5% steps
  }
  if (pattern == "exp" || pattern == "expraw") return exp_demand(i, p);
  return 0;
}

std::int64_t thread_cpu_ns() {
  timespec t{};
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
  return static_cast<std::int64_t>(t.tv_sec) * 1000000000LL + t.tv_nsec;
}

// Each slice the worker asks for demand x 10 ms of its own CPU time (CLOCK_THREAD_CPUTIME_ID), so
// time spent preempted does not count as work done. A worker that cannot get its CPU time by the
// end of the slice drops the rest (unmet demand) and starts the next slice; it never builds a
// backlog. Slices are staggered by thread so workers do not wake in lockstep.
void worker(int i) {
  tids[static_cast<std::size_t>(i)] = static_cast<int>(syscall(SYS_gettid));
  std::uint64_t x = 0x1234567ULL + static_cast<std::uint64_t>(i);
  std::int64_t slice = start_ns + (kSliceNs * i) / threads;
  for (;;) {
    const std::int64_t p = std::max<std::int64_t>(0, slice - start_ns) / phase_ns;
    const auto burn = static_cast<std::int64_t>(demand(i, p) * kSliceNs);
    const std::int64_t cpu0 = thread_cpu_ns();
    const std::int64_t end = slice + kSliceNs;
    if (burn > 0) {
      for (int k = 0;; ++k) {
        for (int j = 0; j < 256; ++j) x = x * 6364136223846793005ULL + 1;
        if (thread_cpu_ns() - cpu0 >= burn || ((k & 15) == 0 && now_ns() >= end)) break;
      }
    }
    sink = x;
    slice = end;
    const std::int64_t now = now_ns();
    if (now < slice) {
      timespec d{static_cast<time_t>(slice / 1000000000LL), static_cast<long>(slice % 1000000000LL)};
      while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &d, nullptr) != 0) {
      }
    } else if (now - slice > kSliceNs) {
      slice += ((now - slice) / kSliceNs) * kSliceNs;  // skip whole slices we missed
    }
  }
}

void set(const std::string& key, const std::string& v) {
  if (key == "GNN_THREADS") threads = std::max(1, std::min(1000, std::atoi(v.c_str())));
  else if (key == "GNN_PATTERN") pattern = v;
  else if (key == "GNN_TOTAL") total = std::max(0.0, std::atof(v.c_str()));
  else if (key == "GNN_PHASE_MS") phase_ns = std::max(10LL, std::atoll(v.c_str())) * 1000000LL;
  else if (key == "GNN_RAMP_S") ramp_s = std::max(1.0, std::atof(v.c_str()));
  else if (key == "GNN_RATIO") ratio = std::atof(v.c_str());
  else if (key == "GNN_SEED") seed = static_cast<unsigned>(std::atoi(v.c_str()));
  else if (key == "GNN_RESAMPLE") resample = std::max(1LL, std::atoll(v.c_str()));
  else if (key == "GNN_EXP_RATE") exp_rate = std::max(0.0, std::atof(v.c_str()));
  else if (key == "GNN_DURATION_S") duration_s = std::atof(v.c_str());
  else if (key == "GNN_LABELS") labels = v;
}

}  // namespace

int main(int argc, char** argv) {
  for (const char* k : {"GNN_THREADS", "GNN_PATTERN", "GNN_TOTAL", "GNN_PHASE_MS", "GNN_RAMP_S", "GNN_RATIO",
                        "GNN_SEED", "GNN_DURATION_S", "GNN_LABELS", "GNN_RESAMPLE", "GNN_EXP_RATE"}) {
    if (const char* v = std::getenv(k)) set(k, v);
  }
  for (int a = 1; a < argc; ++a) {  // --threads=4 style overrides
    std::string s = argv[a];
    if (s.rfind("--", 0) != 0 || s.find('=') == std::string::npos) continue;
    std::string key = "GNN_" + s.substr(2, s.find('=') - 2);
    for (auto& c : key) c = static_cast<char>(std::toupper(c));
    std::replace(key.begin(), key.end(), '-', '_');
    set(key, s.substr(s.find('=') + 1));
  }
  if (pattern != "uniform" && pattern != "hot" && pattern != "geometric" && pattern != "bursty" && pattern != "ramp" &&
      pattern != "exp" && pattern != "expraw") {
    std::fprintf(stderr, "gnnload: unknown pattern %s\n", pattern.c_str());
    return 2;
  }
  for (auto& t : tids) t = 0;
  start_ns = (now_ns() / kSliceNs + 2) * kSliceNs;
  std::printf("gnnload pid=%d pattern=%s threads=%d total=%.3f phase_ms=%lld\n", static_cast<int>(getpid()),
              pattern.c_str(), threads, total, static_cast<long long>(phase_ns / 1000000));
  std::fflush(stdout);
  for (int i = 0; i < threads; ++i) std::thread(worker, i).detach();

  FILE* f = labels.empty() ? nullptr : std::fopen(labels.c_str(), "w");
  if (!labels.empty() && f == nullptr) std::fprintf(stderr, "gnnload: cannot write %s\n", labels.c_str());
  for (int i = 0; i < threads; ++i) {
    while (tids[static_cast<std::size_t>(i)] == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (f) {
    std::fprintf(f, "# gnnload pid=%d pattern=%s threads=%d total=%.4f phase_ms=%lld ramp_s=%g ratio=%g seed=%u resample=%lld exp_rate=%g\n",
                 static_cast<int>(getpid()), pattern.c_str(), threads, total, static_cast<long long>(phase_ns / 1000000),
                 ramp_s, ratio, seed, static_cast<long long>(resample), exp_rate);
    std::fprintf(f, "phase_start_ns,phase,thread,tid,demand\n");
    std::fflush(f);
  }
  for (std::int64_t p = 0;; ++p) {
    const std::int64_t at = start_ns + p * phase_ns;
    const bool done = duration_s > 0 && static_cast<double>(p * phase_ns) / 1e9 >= duration_s;
    if (f) {
      if (done) std::fprintf(f, "%lld,-1,-1,-1,0\n", static_cast<long long>(at));
      else
        for (int i = 0; i < threads; ++i)
          std::fprintf(f, "%lld,%lld,%d,%d,%.4f\n", static_cast<long long>(at), static_cast<long long>(p), i,
                       tids[static_cast<std::size_t>(i)].load(), demand(i, p));
      std::fflush(f);
    }
    if (done) break;
    const std::int64_t wake = at + phase_ns;
    timespec d{static_cast<time_t>(wake / 1000000000LL), static_cast<long>(wake % 1000000000LL)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &d, nullptr) != 0) {
    }
  }
  if (f) std::fclose(f);
  std::_Exit(0);
}
