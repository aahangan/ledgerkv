#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "dbformat.h"
#include "internal_iterator.h"
#include "ledgerkv/env.h"
#include "ledgerkv/status.h"

namespace ledgerkv {

// Read-only access to a sorted table. Open() loads the index and bloom filter
// into memory; Get() costs at most one pread() of a single data block, and
// none at all when the bloom filter rules the key out.
//
// Thread-safe: Get() and iterators may be used concurrently.
class Table {
 public:
  static Status Open(const std::string& path, uint64_t file_size, std::shared_ptr<Table>* out,
                     Env* env = Env::Default());

  Table(const Table&) = delete;
  Table& operator=(const Table&) = delete;

  // Looks up the newest version of key with sequence <= snapshot.
  Status Get(std::string_view key, uint64_t snapshot, std::string* value, LookupResult* result) const;

  // Scans the table in internal order, holding one block in memory. The
  // table must outlive the iterator.
  class Iterator final : public InternalIterator {
   public:
    explicit Iterator(const Table* table) : table_(table) {}

    void SeekToFirst() override;
    void Seek(std::string_view target) override { SeekInternal(target, kMaxSequence); }
    // Positions at the first entry at or after (target, seq) in internal order.
    void SeekInternal(std::string_view target, uint64_t seq);
    void Next() override { ParseNextEntry(); }
    bool Valid() const override { return valid_; }

    std::string_view key() const override { return key_; }
    uint64_t seq() const override { return seq_; }
    RecordType type() const override { return type_; }
    std::string_view value() const override { return value_; }
    Status status() const override { return status_; }

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

  // Visits every entry in internal order. Stops early if fn returns false.
  using Visitor = std::function<bool(std::string_view key, uint64_t seq, RecordType type,
                                     std::string_view value)>;
  Status ForEach(const Visitor& fn) const;

 private:
  struct IndexEntry {
    std::string last_key;
    uint64_t last_seq;
    uint64_t offset;
    uint64_t size;
  };

  Table(std::string path, std::unique_ptr<RandomAccessFile> file, uint64_t file_size)
      : path_(std::move(path)), file_(std::move(file)), file_size_(file_size) {}

  Status ReadBlock(uint64_t offset, uint64_t size, std::string* out) const;

  const std::string path_;
  const std::unique_ptr<RandomAccessFile> file_;
  const uint64_t file_size_;
  std::string filter_;
  std::vector<IndexEntry> index_;
};

}  // namespace ledgerkv
