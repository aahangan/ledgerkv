#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "ledgerkv/options.h"
#include "ledgerkv/status.h"

namespace ledgerkv {

constexpr int kNumLevels = 7;

struct DBStats {
  std::array<uint64_t, kNumLevels> files_per_level{};
  std::array<uint64_t, kNumLevels> bytes_per_level{};
  uint64_t num_tables = 0;
  uint64_t table_bytes = 0;

  // Counters since this DB was opened.
  uint64_t flushes = 0;
  uint64_t compactions = 0;
  uint64_t trivial_moves = 0;      // Compactions done by relinking a table, no rewrite.
  uint64_t tombstones_dropped = 0;
  uint64_t user_bytes_written = 0;  // Key and value bytes passed to Put/Delete.
  uint64_t flush_bytes_written = 0;
  uint64_t compaction_bytes_read = 0;
  uint64_t compaction_bytes_written = 0;
  // Total time writers spent slowed or blocked waiting on background work.
  uint64_t stall_micros = 0;
};

// A persistent, crash-safe key-value store.
//
// Thread safety: all methods may be called concurrently from multiple threads.
// Only one process may have a given database directory open at a time.
class DB {
 public:
  static Status Open(const Options& options, const std::string& dir, std::unique_ptr<DB>* db);

  virtual ~DB() = default;

  virtual Status Put(std::string_view key, std::string_view value) = 0;
  virtual Status Delete(std::string_view key) = 0;

  // Returns NotFound if the key is absent or was deleted.
  virtual Status Get(std::string_view key, std::string* value) = 0;

  // Flushes the memtable to a table and waits for it to finish.
  virtual Status Flush() = 0;

  // Flushes, then compacts every level into the next until all data sits in
  // the bottom level, dropping overwritten values and tombstones.
  virtual Status CompactAll() = 0;

  // Checks structural invariants: levels 1+ are sorted and non-overlapping,
  // every table's keys are strictly increasing and within its recorded
  // range, and every block's checksum matches. Reads every table in full.
  virtual Status Verify() = 0;

  virtual DBStats GetStats() = 0;
};

}  // namespace ledgerkv
