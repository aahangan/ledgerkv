#pragma once

#include <functional>
#include <string_view>

#include "dbformat.h"
#include "ledgerkv/write_batch.h"

namespace ledgerkv {

struct WriteBatchInternal {
  static std::string_view Contents(const WriteBatch& b) { return b.rep_; }

  // Calls fn on each operation in order. Returns false if rep is malformed.
  using Visitor = std::function<void(RecordType type, std::string_view key, std::string_view value)>;
  static bool Iterate(std::string_view rep, const Visitor& fn);
};

}  // namespace ledgerkv
