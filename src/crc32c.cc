#include "crc32c.h"

#include <array>

namespace ledgerkv::crc32c {
namespace {

constexpr uint32_t kPoly = 0x82F63B78;  // Reflected Castagnoli polynomial.

constexpr std::array<uint32_t, 256> MakeTable() {
  std::array<uint32_t, 256> table{};
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ kPoly : c >> 1;
    table[i] = c;
  }
  return table;
}

constexpr auto kTable = MakeTable();

}  // namespace

uint32_t Extend(uint32_t crc, const void* data, size_t n) {
  const auto* p = static_cast<const uint8_t*>(data);
  uint32_t c = ~crc;
  for (size_t i = 0; i < n; i++) c = kTable[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return ~c;
}

}  // namespace ledgerkv::crc32c
