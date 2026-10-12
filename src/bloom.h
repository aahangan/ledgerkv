#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ledgerkv {

// Bloom filter over a table's keys. With 10 bits per key and k = 7 probes the
// theoretical false-positive rate is about 0.8%.
//
// Layout: the bit array, then one byte holding k. Probe positions use double
// hashing (Kirsch & Mitzenmacher): g_i(x) = h1(x) + i * h2(x) mod m, which
// matches k independent hashes asymptotically at the cost of one.
class BloomFilterBuilder {
 public:
  explicit BloomFilterBuilder(int bits_per_key);

  void AddKey(std::string_view key);
  std::string Finish();

 private:
  int bits_per_key_;
  int k_;
  std::vector<uint64_t> hashes_;
};

// Returns false only if key was definitely not added. An empty or malformed
// filter matches everything, so a damaged filter costs speed, not correctness.
bool BloomMayContain(std::string_view filter, std::string_view key);

}  // namespace ledgerkv
