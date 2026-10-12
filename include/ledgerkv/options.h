#pragma once

#include <cstddef>

namespace ledgerkv {

struct Options {
  // Create the database directory if it does not exist.
  bool create_if_missing = true;

  // If true, every write is flushed to stable storage before it is
  // acknowledged. On macOS this uses F_FULLFSYNC, since plain fsync() does not
  // flush the drive's write cache there.
  bool sync = true;

  // Size at which the memtable is frozen and flushed to a level-0 table in
  // the background. Larger values mean fewer, bigger tables and longer
  // recovery.
  size_t write_buffer_size = 4 << 20;

  // Target uncompressed size of a data block within a table.
  size_t block_size = 4096;

  // Bloom filter bits per key. 10 gives roughly a 1% false-positive rate.
  int bloom_bits_per_key = 10;

  // Leveled compaction. Level 0 holds freshly flushed tables, which may
  // overlap. Each level from 1 down is a sorted run of non-overlapping
  // tables, and level L may hold max_bytes_for_level_base *
  // level_size_multiplier^(L-1) bytes before it is compacted into level L+1.
  size_t max_bytes_for_level_base = 10 << 20;
  int level_size_multiplier = 10;
  // Compaction splits its output into tables of about this size.
  size_t target_file_size = 2 << 20;

  // Level-0 table counts that trigger a compaction, slow each write by 1 ms,
  // and stop writes until compaction catches up. Every level-0 table may
  // need to be checked on a read, so these bound read amplification.
  int level0_compaction_trigger = 4;
  int level0_slowdown_trigger = 8;
  int level0_stop_trigger = 12;
};

}  // namespace ledgerkv
