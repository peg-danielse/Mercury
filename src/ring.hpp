#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class RingArchive;

inline constexpr int kMaxCounters = 8;
inline constexpr int kSampleMs = 100;
inline constexpr int kHistorySeconds = 60;
inline constexpr std::size_t kRingSlots =
    static_cast<std::size_t>(kHistorySeconds) * 1000 / static_cast<std::size_t>(kSampleMs);

struct Sample {
  std::int64_t timestamp_ns = 0;
  std::uint64_t time_enabled = 0;
  std::uint64_t time_running = 0;
  std::uint64_t value[kMaxCounters]{};
  int nvalues = 0;
};

class Ring {
 public:
  Ring(std::size_t capacity, std::vector<std::string> names,
       std::shared_ptr<RingArchive> archive = nullptr);
  ~Ring();

  void push(const Sample& sample);
  // Write samples still only in memory. A clean stop keeps the tail the ring has not committed yet.
  void flush();
  std::size_t size() const;
  const std::vector<std::string>& names() const { return names_; }

 private:
  std::vector<Sample> settled_third() const;

  mutable std::mutex mu_;
  std::vector<Sample> slots_;
  std::size_t head_ = 0;
  std::size_t size_ = 0;
  std::size_t pushed_ = 0;
  std::size_t archived_lo_ = 0;
  std::size_t archived_hi_ = 0;
  std::vector<std::string> names_;
  std::shared_ptr<RingArchive> archive_;
};
