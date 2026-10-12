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

  // Sequential scan over a table in key order, holding one block in memory.
  // The table must outlive the iterator.
  class Iterator {
   public:
    explicit Iterator(const Table* table) : table_(table) {}

    void SeekToFirst();
    bool Valid() const { return valid_; }
    void Next();

    std::string_view key() const { return key_; }
    uint64_t seq() const { return seq_; }
    RecordType type() const { return type_; }
    std::string_view value() const { return value_; }

    // Non-OK if iteration stopped because of an I/O error or corruption.
    const Status& status() const { return status_; }

   private:
    bool LoadBlock(size_t index);
    void ParseNextEntry();

    const Table* table_;
    size_t block_index_ = 0;
    std::string block_;
    std::string_view rest_;
    std::string_view key_, value_;
    uint64_t seq_ = 0;
    RecordType type_ = RecordType::kPut;
    bool valid_ = false;
    Status status_;
  };

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
