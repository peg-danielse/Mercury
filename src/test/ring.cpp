// Ring archive: every pushed sample reaches the .mring exactly once, in order, whether the ring
// wrapped many times, once, or never before the final flush.
#include "ring.hpp"
#include "ring_archive.hpp"

#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace {

int failures = 0;

// Timestamps of every record in the archive at path (layout in src/ring_archive.cpp).
std::vector<std::int64_t> archived(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::uint32_t record = 0;
  std::uint64_t committed = 0, offset = 0;
  std::memcpy(&record, data.data() + 20, 4);
  std::memcpy(&committed, data.data() + 24, 8);
  std::memcpy(&offset, data.data() + 32, 8);
  std::vector<std::int64_t> out;
  for (std::uint64_t i = 0; i < committed; ++i) {
    std::int64_t ts = 0;
    std::memcpy(&ts, data.data() + offset + i * record, 8);
    out.push_back(ts);
  }
  return out;
}

void check(std::size_t capacity, int pushes) {
  const std::string path = "/tmp/ring_test_" + std::to_string(::getpid()) + "_" + std::to_string(pushes) + ".mring";
  ::unlink(path.c_str());
  {
    Ring ring(capacity, {"v"}, std::make_shared<RingArchive>(path, std::vector<std::string>{"v"}));
    for (int i = 1; i <= pushes; ++i) {
      Sample s;
      s.timestamp_ns = i;
      s.nvalues = 1;
      ring.push(s);
    }
    ring.flush();
  }
  const auto got = archived(path);
  bool ok = static_cast<int>(got.size()) == pushes;
  for (std::size_t i = 0; ok && i < got.size(); ++i) {
    ok = got[i] == static_cast<std::int64_t>(i + 1);
  }
  if (!ok) {
    ++failures;
    std::cerr << "FAIL capacity " << capacity << " pushes " << pushes << ": archived " << got.size() << " samples:";
    for (const auto ts : got) {
      std::cerr << ' ' << ts;
    }
    std::cerr << std::endl;
  }
  ::unlink(path.c_str());
}

}  // namespace

int main() {
  for (const int pushes : {1, 4, 5, 6, 7, 8, 11, 20, 61}) {
    check(6, pushes);
  }
  check(9, 100);
  if (failures != 0) {
    std::cerr << "ring_test " << failures << " failure(s)" << std::endl;
    return 1;
  }
  std::cout << "ring_test ok" << std::endl;
  return 0;
}
