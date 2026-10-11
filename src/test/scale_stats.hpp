#pragma once

#include <cstdint>

inline constexpr int kScaleWorkers = 128;

// sent counts a client request once it has been written to the socket.
// A reply is late when it arrives after kResponseDeadlineNs.
// Each worker writes only its own line. failed counts exchanges that never got a reply.
struct WorkerStats {
  alignas(64) std::uint64_t sent = 0;
  std::uint64_t completed = 0;
  std::uint64_t service_ns = 0;
  std::uint64_t late = 0;
  std::uint64_t failed = 0;
};

struct ScaleStats {
  WorkerStats workers[kScaleWorkers];
};

inline constexpr std::int64_t kResponseDeadlineNs = 10000000;
