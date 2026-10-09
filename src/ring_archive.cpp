#include "ring_archive.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace {

constexpr char kMagic[8] = {'M', 'R', 'I', 'N', 'G', '1', '\0', '\0'};
constexpr std::uint32_t kVersion = 1;
constexpr std::size_t kHeaderBytes = 40;
constexpr std::size_t kRecordBytes = 8 + 8 + 8 + 4 + 4 + 8 * kMaxCounters;

void write_at(int fd, const void* data, std::size_t size, off_t at) {
  const auto* bytes = static_cast<const char*>(data);
  std::size_t done = 0;
  while (done < size) {
    const auto wrote = ::pwrite(fd, bytes + done, size - done, at + static_cast<off_t>(done));
    if (wrote < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw std::runtime_error(std::string("pwrite failed: ") + std::strerror(errno));
    }
    done += static_cast<std::size_t>(wrote);
  }
}

void read_at(int fd, void* data, std::size_t size, off_t at) {
  auto* bytes = static_cast<char*>(data);
  std::size_t done = 0;
  while (done < size) {
    const auto got = ::pread(fd, bytes + done, size - done, at + static_cast<off_t>(done));
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw std::runtime_error(std::string("pread failed: ") + std::strerror(errno));
    }
    if (got == 0) {
      throw std::runtime_error("short ring archive read");
    }
    done += static_cast<std::size_t>(got);
  }
}

void sync_fd(int fd) {
  while (::fsync(fd) < 0) {
    if (errno != EINTR) {
      throw std::runtime_error(std::string("fsync failed: ") + std::strerror(errno));
    }
  }
}

void put_u32(std::string& out, std::uint32_t value) {
  const char bytes[4] = {static_cast<char>(value), static_cast<char>(value >> 8),
                         static_cast<char>(value >> 16), static_cast<char>(value >> 24)};
  out.append(bytes, 4);
}

void put_u64(std::string& out, std::uint64_t value) {
  char bytes[8];
  for (int i = 0; i < 8; ++i) {
    bytes[i] = static_cast<char>(value >> (8 * i));
  }
  out.append(bytes, 8);
}

std::uint32_t get_u32(const char* bytes) {
  return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[0])) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[1])) << 8) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[2])) << 16) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[3])) << 24);
}

std::uint64_t get_u64(const char* bytes) {
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[i])) << (8 * i);
  }
  return value;
}

std::string encode_names(const std::vector<std::string>& names) {
  std::string out;
  for (const auto& name : names) {
    put_u32(out, static_cast<std::uint32_t>(name.size()));
    out.append(name);
  }
  return out;
}

}  // namespace

RingArchive::RingArchive(std::string path, std::vector<std::string> names) : path_(std::move(path)) {
  const auto parent = std::filesystem::path(path_).parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent);
  }
  fd_ = ::open(path_.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (fd_ < 0) {
    throw std::runtime_error(std::string("open failed: ") + std::strerror(errno));
  }
  try {
  const off_t bytes = ::lseek(fd_, 0, SEEK_END);
  if (bytes < 0) {
    throw std::runtime_error(std::string("lseek failed: ") + std::strerror(errno));
  }
  if (bytes == 0) {
    const auto encoded = encode_names(names);
    data_offset_ = kHeaderBytes + encoded.size();
    std::string header(kMagic, 8);
    put_u32(header, kVersion);
    put_u32(header, static_cast<std::uint32_t>(names.size()));
    put_u32(header, static_cast<std::uint32_t>(kMaxCounters));
    put_u32(header, static_cast<std::uint32_t>(kRecordBytes));
    put_u64(header, 0);
    put_u64(header, data_offset_);
    header.append(encoded);
    write_at(fd_, header.data(), header.size(), 0);
    sync_fd(fd_);
    return;
  }

  char header[kHeaderBytes];
  read_at(fd_, header, sizeof(header), 0);
  if (std::memcmp(header, kMagic, 8) != 0 || get_u32(header + 8) != kVersion ||
      get_u32(header + 16) != kMaxCounters || get_u32(header + 20) != kRecordBytes) {
    throw std::runtime_error("ring archive header mismatch");
  }
  const auto name_count = get_u32(header + 12);
  committed_ = get_u64(header + 24);
  data_offset_ = get_u64(header + 32);
  if (name_count != names.size() || data_offset_ < kHeaderBytes) {
    throw std::runtime_error("ring archive header mismatch");
  }

  std::string stored(static_cast<std::size_t>(data_offset_ - kHeaderBytes), '\0');
  if (!stored.empty()) {
    read_at(fd_, stored.data(), stored.size(), static_cast<off_t>(kHeaderBytes));
  }
  if (stored != encode_names(names)) {
    throw std::runtime_error("ring archive counters mismatch");
  }
  if (::ftruncate(fd_, static_cast<off_t>(data_offset_ + committed_ * kRecordBytes)) < 0) {
    throw std::runtime_error(std::string("ftruncate failed: ") + std::strerror(errno));
  }
  } catch (...) {
    ::close(fd_);
    fd_ = -1;
    throw;
  }
}

RingArchive::~RingArchive() {
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

void RingArchive::append(const std::vector<Sample>& samples) {
  if (samples.empty()) {
    return;
  }
  std::string records;
  records.reserve(samples.size() * kRecordBytes);
  for (const auto& sample : samples) {
    put_u64(records, static_cast<std::uint64_t>(sample.timestamp_ns));
    put_u64(records, sample.time_enabled);
    put_u64(records, sample.time_running);
    put_u32(records, static_cast<std::uint32_t>(sample.nvalues));
    put_u32(records, 0);
    for (int i = 0; i < kMaxCounters; ++i) {
      put_u64(records, sample.value[i]);
    }
  }
  const off_t at = static_cast<off_t>(data_offset_ + committed_ * kRecordBytes);
  write_at(fd_, records.data(), records.size(), at);
  sync_fd(fd_);
  committed_ += samples.size();
  char count[8];
  for (int i = 0; i < 8; ++i) {
    count[i] = static_cast<char>(committed_ >> (8 * i));
  }
  write_at(fd_, count, sizeof(count), 24);
  sync_fd(fd_);
  std::cout << "ring " << path_ << " samples=" << samples.size() << std::endl;
}
