#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <shared_mutex>
#include <string>
#include <string_view>

namespace ledgerkv {

// In-memory sorted buffer of recent writes. Deletes are kept as tombstones so
// that, once SSTables exist, they can shadow older values on disk.
//
// Thread-safe: concurrent readers, exclusive writers.
class MemTable {
 public:
  enum class LookupResult { kFound, kDeleted, kMissing };

  void Put(uint64_t seq, std::string_view key, std::string_view value);
  void Delete(uint64_t seq, std::string_view key);
  LookupResult Get(std::string_view key, std::string* value) const;

  size_t ApproximateBytes() const;
  size_t Size() const;

 private:
  struct Entry {
    uint64_t seq;
    bool deleted;
    std::string value;
  };

  void Insert(uint64_t seq, std::string_view key, std::string_view value, bool deleted);

  mutable std::shared_mutex mu_;
  std::map<std::string, Entry, std::less<>> entries_;
  size_t bytes_ = 0;
};

}  // namespace ledgerkv
