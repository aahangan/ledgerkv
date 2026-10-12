#include "arena.h"

namespace ledgerkv {

char* Arena::Allocate(size_t bytes) {
  constexpr uintptr_t kAlign = 8;
  size_t pad = static_cast<size_t>((kAlign - (reinterpret_cast<uintptr_t>(ptr_) & (kAlign - 1))) & (kAlign - 1));
  usage_.fetch_add(bytes + pad, std::memory_order_relaxed);
  if (bytes + pad <= remaining_) {
    char* result = ptr_ + pad;
    ptr_ += bytes + pad;
    remaining_ -= bytes + pad;
    return result;
  }
  return AllocateFallback(bytes);
}

char* Arena::AllocateFallback(size_t bytes) {
  // Large requests get their own block so they don't waste the rest of the
  // current one.
  if (bytes > kBlockSize / 4) return NewBlock(bytes);
  ptr_ = NewBlock(kBlockSize);  // new[] returns memory aligned for any scalar.
  remaining_ = kBlockSize;
  char* result = ptr_;
  ptr_ += bytes;
  remaining_ -= bytes;
  return result;
}

char* Arena::NewBlock(size_t bytes) {
  blocks_.emplace_back(new char[bytes]);
  return blocks_.back().get();
}

}  // namespace ledgerkv
