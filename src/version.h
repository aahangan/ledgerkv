#pragma once

#include <array>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include "internal_iterator.h"
#include "ledgerkv/db.h"
#include "table.h"
#include "version_edit.h"

namespace ledgerkv {

struct FileEntry {
  TableMeta meta;
  std::shared_ptr<Table> table;
};

// An immutable snapshot of the live tables. Readers grab a shared_ptr to the
// current Version and search it without holding any lock; flushes and
// compactions install a new Version rather than modifying the old one. A
// table deleted by a compaction stays readable through older Versions,
// because POSIX keeps an unlinked file alive while it is open.
struct Version {
  // levels[0] is newest first: flushes run one at a time, so higher file
  // numbers hold newer data. Levels 1+ are sorted by key and non-overlapping.
  // For any user key, versions in shallower levels are newer than versions
  // in deeper ones.
  std::array<std::vector<FileEntry>, kNumLevels> levels;
};

bool Overlaps(const TableMeta& m, std::string_view lo, std::string_view hi);

// Returns a new Version with edit applied to base. tables supplies the open
// Table for every added file.
std::shared_ptr<Version> ApplyEdit(const Version& base, const VersionEdit& edit,
                                   const std::map<uint64_t, std::shared_ptr<Table>>& tables);

std::set<uint64_t> LiveTables(const Version& v);

// Iterates a sorted, non-overlapping run of tables (one level >= 1) as if it
// were one table, opening each table's iterator only when reached.
std::unique_ptr<InternalIterator> NewLevelIterator(std::vector<FileEntry> files);

}  // namespace ledgerkv
