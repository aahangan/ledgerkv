#pragma once

namespace ledgerkv {

struct Options {
  // Create the database directory if it does not exist.
  bool create_if_missing = true;

  // If true, every write is flushed to stable storage before it is
  // acknowledged. On macOS this uses F_FULLFSYNC, since plain fsync() does not
  // flush the drive's write cache there.
  bool sync = true;
};

}  // namespace ledgerkv
