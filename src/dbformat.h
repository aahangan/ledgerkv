#pragma once

#include <cstdint>

namespace ledgerkv {

enum class RecordType : uint8_t { kPut = 1, kDelete = 2 };

// Outcome of looking a key up in one layer (memtable or table). kDeleted means
// the layer holds a tombstone, which hides any older value in lower layers.
enum class LookupResult { kFound, kDeleted, kMissing };

// Sequence number and record type packed into one integer, as stored in
// tables: seq in the high 56 bits, type in the low 8.
inline uint64_t PackTag(uint64_t seq, RecordType type) {
  return (seq << 8) | static_cast<uint64_t>(type);
}
inline uint64_t TagSequence(uint64_t tag) { return tag >> 8; }
inline RecordType TagType(uint64_t tag) { return static_cast<RecordType>(tag & 0xFF); }

}  // namespace ledgerkv

#include <string_view>

namespace ledgerkv {

// Sequence numbers fit in the 56 bits above the type byte of a tag.
constexpr uint64_t kMaxSequence = (uint64_t{1} << 56) - 1;

// Internal ordering of (user key, sequence) pairs: user key ascending, then
// sequence descending, so the newest version of a key comes first. Every
// sorted structure (memtable, table, merge) uses this order.
inline int CompareInternal(std::string_view key_a, uint64_t seq_a, std::string_view key_b, uint64_t seq_b) {
  int c = key_a.compare(key_b);
  if (c != 0) return c;
  if (seq_a > seq_b) return -1;
  if (seq_a < seq_b) return 1;
  return 0;
}

}  // namespace ledgerkv
