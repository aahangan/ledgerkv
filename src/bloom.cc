#include "bloom.h"

#include <algorithm>
#include <cmath>

#include "hash.h"

namespace ledgerkv {

BloomFilterBuilder::BloomFilterBuilder(int bits_per_key) : bits_per_key_(bits_per_key) {
  // Optimal k = (m/n) ln 2.
  k_ = std::clamp(static_cast<int>(std::lround(bits_per_key * 0.69)), 1, 30);
}

void BloomFilterBuilder::AddKey(std::string_view key) { hashes_.push_back(Hash64(key)); }

std::string BloomFilterBuilder::Finish() {
  // A floor of 64 bits keeps tiny tables from having absurd false-positive rates.
  size_t bits = std::max<size_t>(64, hashes_.size() * static_cast<size_t>(bits_per_key_));
  size_t bytes = (bits + 7) / 8;
  bits = bytes * 8;

  std::string filter(bytes, '\0');
  for (uint64_t h : hashes_) {
    uint64_t h1 = h, h2 = (h >> 32) | (h << 32) | 1;
    for (int i = 0; i < k_; i++) {
      uint64_t bit = (h1 + static_cast<uint64_t>(i) * h2) % bits;
      filter[bit / 8] = static_cast<char>(filter[bit / 8] | (1 << (bit % 8)));
    }
  }
  filter.push_back(static_cast<char>(k_));
  hashes_.clear();
  return filter;
}

bool BloomMayContain(std::string_view filter, std::string_view key) {
  if (filter.size() < 2) return true;
  int k = static_cast<uint8_t>(filter.back());
  if (k < 1 || k > 30) return true;
  uint64_t bits = (filter.size() - 1) * 8;

  uint64_t h = Hash64(key);
  uint64_t h1 = h, h2 = (h >> 32) | (h << 32) | 1;
  for (int i = 0; i < k; i++) {
    uint64_t bit = (h1 + static_cast<uint64_t>(i) * h2) % bits;
    if (!(static_cast<uint8_t>(filter[bit / 8]) & (1 << (bit % 8)))) return false;
  }
  return true;
}

}  // namespace ledgerkv
