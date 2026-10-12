#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ledgerkv {

// A group of puts and deletes applied atomically: after a crash, either all
// of a batch's operations are present or none are, and readers never see a
// batch half applied. Within a batch, later operations on a key win.
class WriteBatch {
 public:
  WriteBatch();

  void Put(std::string_view key, std::string_view value);
  void Delete(std::string_view key);
  void Clear();

  uint32_t Count() const;
  size_t ApproximateSize() const { return rep_.size(); }

 private:
  friend struct WriteBatchInternal;

  // fixed32 count | entries, each: type (1) | varint klen | key | [varint vlen | value]
  std::string rep_;
};

}  // namespace ledgerkv
