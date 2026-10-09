#include "ring.hpp"

#include "ring_archive.hpp"

#include <iostream>
#include <stdexcept>

Ring::Ring(std::size_t capacity, std::vector<std::string> names, std::shared_ptr<RingArchive> archive)
    : slots_(capacity), names_(std::move(names)), archive_(std::move(archive)) {}

Ring::~Ring() = default;

std::vector<Sample> Ring::settled_third() const {
  const std::size_t third = slots_.size() / 3;
  const std::size_t begin = (head_ + third) % slots_.size();
  std::vector<Sample> samples;
  samples.reserve(third);
  for (std::size_t i = 0; i < third; ++i) {
    samples.push_back(slots_[(begin + i) % slots_.size()]);
  }
  return samples;
}

void Ring::push(const Sample& sample) {
  std::vector<Sample> settled;
  {
    std::lock_guard lock(mu_);
    slots_[head_] = sample;
    head_ = (head_ + 1) % slots_.size();
    if (size_ < slots_.size()) {
      ++size_;
    }
    const std::size_t third = slots_.size() / 3;
    if (archive_ && third > 0 && size_ == slots_.size() && head_ % third == 0) {
      settled = settled_third();
    }
  }
  if (settled.empty()) {
    return;
  }
  try {
    archive_->append(settled);
  } catch (const std::exception& error) {
    std::cerr << "ring archive: " << error.what() << std::endl;
  }
}

std::size_t Ring::size() const {
  std::lock_guard lock(mu_);
  return size_;
}
