#include "memtable.h"

#include <mutex>

namespace ledgerkv {

void MemTable::Put(uint64_t seq, std::string_view key, std::string_view value) {
  Insert(seq, key, value, /*deleted=*/false);
}

void MemTable::Delete(uint64_t seq, std::string_view key) {
  Insert(seq, key, {}, /*deleted=*/true);
}

void MemTable::Insert(uint64_t seq, std::string_view key, std::string_view value, bool deleted) {
  std::unique_lock lock(mu_);
  auto it = entries_.find(key);
  if (it == entries_.end()) {
    entries_.emplace(std::string(key), Entry{seq, deleted, std::string(value)});
    bytes_ += key.size() + value.size();
  } else {
    bytes_ = bytes_ - it->second.value.size() + value.size();
    it->second = Entry{seq, deleted, std::string(value)};
  }
}

MemTable::LookupResult MemTable::Get(std::string_view key, std::string* value) const {
  std::shared_lock lock(mu_);
  auto it = entries_.find(key);
  if (it == entries_.end()) return LookupResult::kMissing;
  if (it->second.deleted) return LookupResult::kDeleted;
  value->assign(it->second.value);
  return LookupResult::kFound;
}

size_t MemTable::ApproximateBytes() const {
  std::shared_lock lock(mu_);
  return bytes_;
}

size_t MemTable::Size() const {
  std::shared_lock lock(mu_);
  return entries_.size();
}

}  // namespace ledgerkv
