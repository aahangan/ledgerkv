#pragma once

#include <cstddef>
#include <cstdint>

namespace ledgerkv::crc32c {

// CRC-32C (Castagnoli), the checksum used by iSCSI, ext4, and LevelDB.
uint32_t Extend(uint32_t crc, const void* data, size_t n);

inline uint32_t Value(const void* data, size_t n) { return Extend(0, data, n); }

}  // namespace ledgerkv::crc32c
