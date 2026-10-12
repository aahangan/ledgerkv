#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ledgerkv/status.h"

namespace ledgerkv {

// Writes all n bytes, retrying on short writes and EINTR.
Status WriteAll(int fd, const char* data, size_t n, const std::string& path);

// Flushes file data to stable storage. Uses F_FULLFSYNC on macOS.
Status SyncFd(int fd, const std::string& path);

// fsyncs a directory so that newly created or renamed entries are durable.
Status SyncDir(const std::string& dir);

Status ReadFileToString(const std::string& path, std::string* out);

Status IOErrorFromErrno(const std::string& context);

}  // namespace ledgerkv

namespace ledgerkv {

// Lists the entries of dir, excluding "." and "..".
Status ListDir(const std::string& dir, std::vector<std::string>* names);

// Truncates path to size bytes and fsyncs it.
Status TruncateFile(const std::string& path, uint64_t size);

}  // namespace ledgerkv
