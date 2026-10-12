#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace ledgerkv {

// Bump allocator for memtable entries and skiplist nodes. Memory is freed
// all at once when the arena is destroyed, which matches the memtable's
// lifetime: entries are never removed individually.
//
// Allocate() is not thread-safe (the memtable has a single writer), but
// MemoryUsage() may be read from any thread.
class Arena {
 public:
  Arena() = default;
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  // Returns 8-byte-aligned memory.
  char* Allocate(size_t bytes);

  // Bytes handed out so far, including alignment padding. This is what the
  // memtable compares against write_buffer_size; counting whole reserved
  // blocks instead would make a small memtable look full after one write.
  size_t MemoryUsage() const { return usage_.load(std::memory_order_relaxed); }

 private:
  static constexpr size_t kBlockSize = 64 << 10;

  char* AllocateFallback(size_t bytes);
  char* NewBlock(size_t bytes);

  char* ptr_ = nullptr;
  size_t remaining_ = 0;
  std::vector<std::unique_ptr<char[]>> blocks_;
  std::atomic<size_t> usage_{0};
};

}  // namespace ledgerkv
