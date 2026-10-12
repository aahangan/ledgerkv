#pragma once

#include <cstddef>
#include <string>

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
