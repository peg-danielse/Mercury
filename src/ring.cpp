#include "ring.hpp"

#include "ring_archive.hpp"

#include <iostream>
#include <stdexcept>

Ring::Ring(std::size_t capacity, std::vector<std::string> names, std::shared_ptr<RingArchive> archive)
    : slots_(capacity), names_(std::move(names)), archive_(std::move(archive)) {}

Ring::~Ring() = default;

// The oldest third of a full ring (the next pushes overwrite it), minus samples an earlier push or
// flush already archived. Advances [archived_lo_, archived_hi_), which stays contiguous.
std::vector<Sample> Ring::settled_third() {
  const std::size_t third = slots_.size() / 3;
  const std::size_t oldest = pushed_ - size_;
  std::size_t from = oldest;
  if (archived_hi_ > archived_lo_ && archived_hi_ > from) {
    from = archived_hi_;
  }
  std::vector<Sample> samples;
  for (std::size_t index = from; index < oldest + third; ++index) {
    samples.push_back(slots_[index % slots_.size()]);
  }
  if (archived_hi_ <= archived_lo_) {
    archived_lo_ = oldest;
  }
  if (oldest + third > archived_hi_) {
    archived_hi_ = oldest + third;
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
    ++pushed_;
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

void Ring::flush() {
  std::vector<Sample> pending;
  std::size_t oldest = 0;
  std::size_t end = 0;
  {
    std::lock_guard lock(mu_);
    if (!archive_ || size_ == 0 || pushed_ == 0) {
      return;
    }
    oldest = pushed_ - size_;
    end = pushed_;
    pending.reserve(size_);
    for (std::size_t index = oldest; index < end; ++index) {
      if (archived_hi_ > archived_lo_ && index >= archived_lo_ && index < archived_hi_) {
        continue;
      }
      pending.push_back(slots_[index % slots_.size()]);
    }
  }
  if (pending.empty()) {
    return;
  }
  try {
    archive_->append(pending);
  } catch (const std::exception& error) {
    std::cerr << "ring archive: " << error.what() << std::endl;
    return;
  }
  std::lock_guard lock(mu_);
  archived_lo_ = oldest;
  archived_hi_ = end;
}

std::vector<Sample> Ring::recent(std::size_t count) const {
  std::lock_guard lock(mu_);
  if (count > size_) {
    count = size_;
  }
  std::vector<Sample> out;
  if (count == 0 || slots_.empty()) {
    return out;
  }
  out.reserve(count);
  const std::size_t start = (head_ + slots_.size() - count) % slots_.size();
  for (std::size_t i = 0; i < count; ++i) {
    out.push_back(slots_[(start + i) % slots_.size()]);
  }
  return out;
}

std::size_t Ring::size() const {
  std::lock_guard lock(mu_);
  return size_;
}
