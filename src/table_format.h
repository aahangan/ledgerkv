#pragma once

#include <cstdint>

namespace ledgerkv {

// Sorted table (.sst) layout:
//
//   [data block 0] ... [data block N-1] [filter block] [index block] [footer]
//
// Every block is followed by a 4-byte CRC-32C of its contents.
//
//   data entry:  varint klen | key | fixed64 tag | varint vlen | value
//                (tag = seq << 8 | type; deletes have an empty value)
//   filter:      bloom filter over every key in the table
//   index entry: varint klen | last key in block | varint64 offset | varint64 size
//   footer:      fixed64 filter offset | fixed64 filter size |
//                fixed64 index offset  | fixed64 index size  | fixed64 magic
//
// Block sizes exclude the CRC trailer. Keys are unique within a table and
// strictly increasing.

constexpr uint64_t kTableMagic = 0x6C6564676572'6B76ULL;  // "ledgerkv"
constexpr size_t kFooterSize = 40;
constexpr size_t kBlockTrailerSize = 4;

}  // namespace ledgerkv
