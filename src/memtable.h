#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "arena.h"
#include "dbformat.h"
#include "internal_iterator.h"
#include "skiplist.h"

namespace ledgerkv {

// In-memory sorted buffer of recent writes, holding every version of every
// key (so snapshots can read old ones) and tombstones (so deletes shadow
// older values on disk).
//
// Backed by a skiplist: one writer at a time (the write path serializes
// Add() calls), and any number of concurrent lock-free readers.
class MemTable {
 public:
  MemTable() : table_(KeyComparator(), &arena_) {}
  MemTable(const MemTable&) = delete;
  MemTable& operator=(const MemTable&) = delete;

  // Requires external serialization against other Add() calls. Each seq is
  // used once.
  void Add(uint64_t seq, RecordType type, std::string_view key, std::string_view value);

  // Looks up the newest version of key with sequence <= snapshot.
  LookupResult Get(std::string_view key, uint64_t snapshot, std::string* value) const;

  // Iterates every entry in internal order. The memtable must outlive it.
  std::unique_ptr<InternalIterator> NewIterator() const;

  size_t ApproximateBytes() const { return arena_.MemoryUsage(); }
  size_t Size() const { return count_.load(std::memory_order_acquire); }
  bool Empty() const { return Size() == 0; }
  uint64_t MaxSequence() const { return max_seq_.load(std::memory_order_acquire); }

 private:
  // Entries are stored in the arena as:
  //   varint klen | key | fixed64 tag | varint vlen | value
  // and the skiplist holds pointers to them.
  struct KeyComparator {
    int operator()(const char* a, const char* b) const;
  };
  using Table = SkipList<const char*, KeyComparator>;
  class Iter;

  Arena arena_;
  Table table_;
  std::atomic<size_t> count_{0};
  std::atomic<uint64_t> max_seq_{0};
};

}  // namespace ledgerkv
