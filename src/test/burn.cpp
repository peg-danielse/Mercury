// CPU stressor used by scripts/policy_experiment.py as the injected fault.
// BURN_THREADS (default 1) threads spin until the process is killed. Started through
// Mercury /start like any daemon, then its cpuset is set with /cpus onto the victim node.

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <thread>
#include <vector>

int main() {
  int threads = 1;
  if (const char* env = std::getenv("BURN_THREADS")) {
    threads = std::max(1, std::atoi(env));
  }
  std::atomic<unsigned long> sink{0};
  std::vector<std::thread> pool;
  for (int i = 0; i < threads; ++i) {
    pool.emplace_back([&sink] {
      unsigned long x = 1;
      while (true) {
        for (int j = 0; j < 1000000; ++j) {
          x = x * 2862933555777941757UL + 3037000493UL;
        }
        sink.fetch_add(x & 1, std::memory_order_relaxed);
      }
    });
  }
  for (auto& thread : pool) {
    thread.join();
  }
  return 0;
}
