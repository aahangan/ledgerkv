#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "bloom.h"
#include "dbformat.h"
#include "ledgerkv/status.h"

namespace ledgerkv {

// Writes a sorted table file. Keys must be added in strictly increasing order.
// The file is complete and durable once Finish() returns OK. A partially
// written file is never referenced from the MANIFEST and is deleted as an
// orphan on the next open.
class TableBuilder {
 public:
  struct Options {
    size_t block_size = 4096;
    int bloom_bits_per_key = 10;
  };

  TableBuilder(const Options& options, std::string path);
  ~TableBuilder();
  TableBuilder(const TableBuilder&) = delete;
  TableBuilder& operator=(const TableBuilder&) = delete;

  Status Open();
  void Add(std::string_view key, uint64_t seq, RecordType type, std::string_view value);

  // Writes the filter, index, and footer, then fsyncs the file.
  Status Finish();

  // Closes and deletes the file. Used when the build fails partway.
  void Abandon();

  uint64_t FileSize() const { return offset_; }
  uint64_t NumEntries() const { return num_entries_; }
  const std::string& SmallestKey() const { return smallest_; }
  const std::string& LargestKey() const { return last_key_; }

 private:
  Status FlushDataBlock();
  Status WriteBlock(const std::string& contents, uint64_t* offset, uint64_t* size);

  const Options options_;
  const std::string path_;
  int fd_ = -1;
  Status status_;

  uint64_t offset_ = 0;
  uint64_t num_entries_ = 0;
  std::string smallest_;
  std::string last_key_;

  std::string data_block_;
  std::string index_block_;
  BloomFilterBuilder bloom_;
};

}  // namespace ledgerkv
