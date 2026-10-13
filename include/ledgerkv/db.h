#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "ledgerkv/iterator.h"
#include "ledgerkv/options.h"
#include "ledgerkv/status.h"
#include "ledgerkv/write_batch.h"

namespace ledgerkv {

constexpr int kNumLevels = 7;

// A consistent point-in-time view, obtained from DB::GetSnapshot(). While a
// snapshot is held, compaction keeps every version it can see.
class Snapshot {
 protected:
  virtual ~Snapshot() = default;
};

// Per-read counters, filled in when ReadOptions::stats is set. Accumulates
// across calls; the caller owns it and must not share it between threads.
struct ReadStats {
  uint64_t tables_checked = 0;  // Tables whose key range covered the key.
  uint64_t bloom_skips = 0;     // Of those, ruled out by the bloom filter with no read.
  uint64_t blocks_read = 0;     // Data blocks read from the file.
};

struct ReadOptions {
  // Read as of this snapshot instead of the latest state.
  const Snapshot* snapshot = nullptr;
  // If set, Get() adds what it did to these counters.
  ReadStats* stats = nullptr;
};

struct DBStats {
  std::array<uint64_t, kNumLevels> files_per_level{};
  std::array<uint64_t, kNumLevels> bytes_per_level{};
  uint64_t num_tables = 0;
  uint64_t table_bytes = 0;

  // Counters since this DB was opened.
  uint64_t flushes = 0;
  uint64_t compactions = 0;
  uint64_t trivial_moves = 0;       // Compactions done by relinking a table, no rewrite.
  uint64_t tombstones_dropped = 0;
  uint64_t versions_dropped = 0;    // Overwritten versions no snapshot could see.
  uint64_t user_bytes_written = 0;  // Key and value bytes passed to writes.
  uint64_t flush_bytes_written = 0;
  uint64_t compaction_bytes_read = 0;
  uint64_t compaction_bytes_written = 0;
  // Group commit: writes (batches) committed, and the WAL appends that
  // carried them. writes / write_groups is the average group size.
  uint64_t writes = 0;
  uint64_t write_groups = 0;
  // Total time writers spent slowed or blocked waiting on background work.
  uint64_t stall_micros = 0;
};

// A persistent, crash-safe key-value store.
//
// Thread safety: all methods may be called concurrently from multiple threads.
// Concurrent writers are committed in groups that share one WAL append and
// one fsync. Only one process may have a given database directory open at a
// time.
class DB {
 public:
  static Status Open(const Options& options, const std::string& dir, std::unique_ptr<DB>* db);

  virtual ~DB() = default;

  virtual Status Put(std::string_view key, std::string_view value) = 0;
  virtual Status Delete(std::string_view key) = 0;
  // Applies every operation in batch atomically.
  virtual Status Write(const WriteBatch& batch) = 0;

  // Returns NotFound if the key is absent or was deleted.
  virtual Status Get(std::string_view key, std::string* value) { return Get(ReadOptions(), key, value); }
  virtual Status Get(const ReadOptions& options, std::string_view key, std::string* value) = 0;

  virtual std::unique_ptr<Iterator> NewIterator(const ReadOptions& options = ReadOptions()) = 0;

  // The caller must release every snapshot with ReleaseSnapshot().
  virtual const Snapshot* GetSnapshot() = 0;
  virtual void ReleaseSnapshot(const Snapshot* snapshot) = 0;

  // Flushes the memtable to a table and waits for it to finish.
  virtual Status Flush() = 0;

  // Flushes, then compacts every level into the next until all data sits in
  // the bottom level, dropping overwritten values and tombstones that no
  // snapshot can see.
  virtual Status CompactAll() = 0;

  // Checks structural invariants: levels 1+ are sorted and non-overlapping,
  // every table's entries are in internal order and within its recorded key
  // range, and every block's checksum matches. Reads every table in full.
  virtual Status Verify() = 0;

  virtual DBStats GetStats() = 0;
};

}  // namespace ledgerkv
