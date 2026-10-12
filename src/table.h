#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "dbformat.h"
#include "ledgerkv/status.h"

namespace ledgerkv {

// Read-only access to a sorted table. Open() loads the index and bloom filter
// into memory; Get() costs at most one pread() of a single data block, and
// none at all when the bloom filter rules the key out.
//
// Thread-safe: Get() and ForEach() may be called concurrently.
class Table {
 public:
  static Status Open(const std::string& path, uint64_t file_size, std::shared_ptr<Table>* out);

  ~Table();
  Table(const Table&) = delete;
  Table& operator=(const Table&) = delete;

  Status Get(std::string_view key, std::string* value, LookupResult* result) const;

  // Visits every entry in key order. Stops early if fn returns false.
  using Visitor = std::function<bool(std::string_view key, uint64_t seq, RecordType type,
                                     std::string_view value)>;
  Status ForEach(const Visitor& fn) const;

 private:
  struct IndexEntry {
    std::string last_key;
    uint64_t offset;
    uint64_t size;
  };

  Table(std::string path, int fd) : path_(std::move(path)), fd_(fd) {}

  Status ReadBlock(uint64_t offset, uint64_t size, std::string* out) const;

  const std::string path_;
  const int fd_;
  std::string filter_;
  std::vector<IndexEntry> index_;
};

}  // namespace ledgerkv
