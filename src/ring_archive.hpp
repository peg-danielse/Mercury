#pragma once

#include "ring.hpp"

#include <cstdint>
#include <string>
#include <vector>

// Append-only file of exported ring thirds. A crash keeps every committed third.
class RingArchive {
 public:
  RingArchive(std::string path, std::vector<std::string> names);
  ~RingArchive();
  RingArchive(const RingArchive&) = delete;
  RingArchive& operator=(const RingArchive&) = delete;

  void append(const std::vector<Sample>& samples);
  const std::string& path() const { return path_; }

 private:
  std::string path_;
  int fd_ = -1;
  std::uint64_t data_offset_ = 0;
  std::uint64_t committed_ = 0;
};
