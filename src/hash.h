#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ledgerkv {

// 64-bit FNV-1a followed by the splitmix64 finalizer, which spreads FNV's
// weak low bits across the whole word. Not cryptographic.
inline uint64_t Hash64(std::string_view s) {
  uint64_t h = 0xCBF29CE484222325ULL;
  for (char c : s) {
    h ^= static_cast<uint8_t>(c);
    h *= 0x100000001B3ULL;
  }
  h ^= h >> 30;
  h *= 0xBF58476D1CE4E5B9ULL;
  h ^= h >> 27;
  h *= 0x94D049BB133111EBULL;
  h ^= h >> 31;
  return h;
}

}  // namespace ledgerkv
