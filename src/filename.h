#pragma once

#include <cstdint>
#include <string>

namespace ledgerkv {

// Files in a database directory:
//   000123.log   write-ahead log
//   000124.sst   sorted table
//   MANIFEST     log of version edits (which tables are live, which log is current)
//   LOCK         held with flock() by the process that has the database open
//   MANIFEST.tmp a MANIFEST snapshot being written, renamed over MANIFEST when done
//   lost/        tables set aside during recovery because the MANIFEST was damaged
enum class FileType { kLog, kTable, kManifest, kLock, kTemp, kUnknown };

std::string LogFileName(const std::string& dir, uint64_t number);
std::string TableFileName(const std::string& dir, uint64_t number);
std::string ManifestFileName(const std::string& dir);
std::string TempManifestFileName(const std::string& dir);

// Classifies a bare file name. *number is set for logs and tables.
FileType ParseFileName(const std::string& name, uint64_t* number);

}  // namespace ledgerkv
