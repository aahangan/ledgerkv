#include "ledgerkv/write_batch.h"

#include "coding.h"
#include "write_batch_internal.h"

namespace ledgerkv {

WriteBatch::WriteBatch() { Clear(); }

void WriteBatch::Clear() {
  rep_.clear();
  PutFixed32(&rep_, 0);
}

uint32_t WriteBatch::Count() const { return DecodeFixed32(rep_.data()); }

void WriteBatch::Put(std::string_view key, std::string_view value) {
  std::string count;
  PutFixed32(&count, Count() + 1);
  rep_.replace(0, 4, count);
  rep_.push_back(static_cast<char>(RecordType::kPut));
  PutLengthPrefixed(&rep_, key);
  PutLengthPrefixed(&rep_, value);
}

void WriteBatch::Delete(std::string_view key) {
  std::string count;
  PutFixed32(&count, Count() + 1);
  rep_.replace(0, 4, count);
  rep_.push_back(static_cast<char>(RecordType::kDelete));
  PutLengthPrefixed(&rep_, key);
}

bool WriteBatchInternal::Iterate(std::string_view rep, const Visitor& fn) {
  if (rep.size() < 4) return false;
  uint32_t count = DecodeFixed32(rep.data());
  rep.remove_prefix(4);
  for (uint32_t i = 0; i < count; i++) {
    if (rep.empty()) return false;
    auto type = static_cast<RecordType>(rep.front());
    rep.remove_prefix(1);
    std::string_view key, value;
    if (!GetLengthPrefixed(&rep, &key)) return false;
    if (type == RecordType::kPut) {
      if (!GetLengthPrefixed(&rep, &value)) return false;
    } else if (type != RecordType::kDelete) {
      return false;
    }
    fn(type, key, value);
  }
  return rep.empty();
}

}  // namespace ledgerkv
