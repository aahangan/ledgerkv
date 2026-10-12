#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <shared_mutex>
#include <string>
#include <string_view>

#include "dbformat.h"

namespace ledgerkv {

// In-memory sorted buffer of recent writes. Deletes are kept as tombstones so
// that they shadow older values in tables on disk.
//
// Thread-safe: concurrent readers, exclusive writers.
class MemTable {
 public:
  void Put(uint64_t seq, std::string_view key, std::string_view value);
  void Delete(uint64_t seq, std::string_view key);
  LookupResult Get(std::string_view key, std::string* value) const;

  // Visits every entry in key order. Holds a read lock for the duration.
  using Visitor =
      std::function<void(std::string_view key, uint64_t seq, RecordType type, std::string_view value)>;
  void ForEach(const Visitor& fn) const;

  size_t ApproximateBytes() const;
  size_t Size() const;
  bool Empty() const { return Size() == 0; }
  uint64_t MaxSequence() const;

 private:
  struct Entry {
    uint64_t seq;
    bool deleted;
    std::string value;
  };

  // Per-entry bookkeeping cost in the std::map, so that many tiny writes
  // still trigger a flush at a sensible point.
  static constexpr size_t kEntryOverhead = 64;

  void Insert(uint64_t seq, std::string_view key, std::string_view value, bool deleted);

  mutable std::shared_mutex mu_;
  std::map<std::string, Entry, std::less<>> entries_;
  size_t bytes_ = 0;
  uint64_t max_seq_ = 0;
};

}  // namespace ledgerkv
