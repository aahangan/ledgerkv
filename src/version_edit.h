#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ledgerkv {

struct TableMeta {
  int level = 0;
  uint64_t number = 0;
  uint64_t file_size = 0;
  std::string smallest;
  std::string largest;
};

// One MANIFEST record: a delta applied to the set of live tables plus updated
// counters. Recovery replays every edit in order to rebuild the current state.
// An edit is applied atomically: it is one checksummed log record, so after a
// crash it is either entirely present or entirely absent.
struct VersionEdit {
  // Write-ahead logs numbered below this have been fully flushed to tables
  // and can be deleted.
  std::optional<uint64_t> log_number;
  std::optional<uint64_t> next_file_number;
  std::optional<uint64_t> last_sequence;
  std::vector<TableMeta> added_tables;
  std::vector<uint64_t> deleted_tables;

  void EncodeTo(std::string* dst) const;
  bool DecodeFrom(std::string_view in);
};

}  // namespace ledgerkv
