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

  // Size at which the memtable is frozen and flushed to a sorted table in the
  // background. Larger values mean fewer, bigger tables and longer recovery.
  size_t write_buffer_size = 4 << 20;

  // Target uncompressed size of a data block within a table.
  size_t block_size = 4096;

  // Bloom filter bits per key. 10 gives roughly a 1% false-positive rate.
  int bloom_bits_per_key = 10;
};

}  // namespace ledgerkv
