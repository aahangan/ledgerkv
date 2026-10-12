#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "ledgerkv/options.h"
#include "ledgerkv/status.h"

namespace ledgerkv {

// A persistent, crash-safe key-value store.
//
// Thread safety: all methods may be called concurrently from multiple threads.
// Only one process may have a given database directory open at a time.
class DB {
 public:
  static Status Open(const Options& options, const std::string& dir, std::unique_ptr<DB>* db);

  virtual ~DB() = default;

  virtual Status Put(std::string_view key, std::string_view value) = 0;
  virtual Status Delete(std::string_view key) = 0;

  // Returns NotFound if the key is absent or was deleted.
  virtual Status Get(std::string_view key, std::string* value) = 0;
};

}  // namespace ledgerkv
