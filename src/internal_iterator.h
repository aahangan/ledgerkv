#pragma once

#include <cstdint>
#include <string_view>

#include "dbformat.h"
#include "ledgerkv/status.h"

namespace ledgerkv {

// Iterates over raw entries in internal order (user key ascending, newest
// version first), including every version and every tombstone. The public
// Iterator is built on top of this and hides what a reader should not see.
class InternalIterator {
 public:
  virtual ~InternalIterator() = default;

  virtual void SeekToFirst() = 0;
  // Positions at the first entry whose user key is >= target.
  virtual void Seek(std::string_view target) = 0;
  virtual void Next() = 0;
  virtual bool Valid() const = 0;

  virtual std::string_view key() const = 0;  // User key.
  virtual uint64_t seq() const = 0;
  virtual RecordType type() const = 0;
  virtual std::string_view value() const = 0;

  // Non-OK if iteration stopped because of an I/O error or corruption.
  virtual Status status() const = 0;
};

}  // namespace ledgerkv
