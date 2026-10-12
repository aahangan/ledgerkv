#pragma once

#include <string_view>

#include "ledgerkv/status.h"

namespace ledgerkv {

// Forward iterator over a consistent view of the database: the state as of
// its snapshot, or as of its creation if it has none. Writes made after that
// point are not visible. Deleted keys are skipped.
//
// key() and value() stay valid until the next call that moves the iterator.
// An iterator must be destroyed before the DB that created it. Not
// thread-safe; use one iterator per thread.
class Iterator {
 public:
  virtual ~Iterator() = default;

  virtual bool Valid() const = 0;
  virtual void SeekToFirst() = 0;
  // Positions at the first key >= target.
  virtual void Seek(std::string_view target) = 0;
  virtual void Next() = 0;

  virtual std::string_view key() const = 0;
  virtual std::string_view value() const = 0;

  // Non-OK if iteration stopped because of an I/O error or corruption.
  virtual Status status() const = 0;
};

}  // namespace ledgerkv
