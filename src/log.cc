#include "log.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>

#include "coding.h"
#include "crc32c.h"
#include "file_util.h"

namespace ledgerkv {

void AppendLogRecord(std::string_view payload, std::string* dst) {
  std::string len;
  PutFixed32(&len, static_cast<uint32_t>(payload.size()));
  uint32_t crc = crc32c::Extend(crc32c::Value(len.data(), len.size()), payload.data(), payload.size());
  PutFixed32(dst, crc);
  dst->append(len);
  dst->append(payload);
}

Status LogWriter::Open(const std::string& path, bool sync, std::unique_ptr<LogWriter>* out) {
  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  if (fd < 0) return IOErrorFromErrno("open " + path);
  out->reset(new LogWriter(path, fd, sync));
  return Status::OK();
}

LogWriter::~LogWriter() { ::close(fd_); }

Status LogWriter::AddRecord(std::string_view payload) {
  scratch_.clear();
  AppendLogRecord(payload, &scratch_);
  Status s = WriteAll(fd_, scratch_.data(), scratch_.size(), path_);
  if (s.ok() && sync_) s = SyncFd(fd_, path_);
  return s;
}

Status LogWriter::AddFramedRecords(std::string_view framed) {
  Status s = WriteAll(fd_, framed.data(), framed.size(), path_);
  if (s.ok() && sync_) s = SyncFd(fd_, path_);
  return s;
}

Status ReadLog(const std::string& path, const std::function<bool(std::string_view)>& fn,
               LogReadStats* stats) {
  *stats = LogReadStats();
  struct stat st;
  if (::stat(path.c_str(), &st) != 0) {
    if (errno == ENOENT) return Status::OK();
    return IOErrorFromErrno("stat " + path);
  }
  std::string data;
  Status s = ReadFileToString(path, &data);
  if (!s.ok()) return s;
  stats->file_bytes = data.size();

  size_t pos = 0;
  while (data.size() - pos >= kLogHeaderSize) {
    const char* header = data.data() + pos;
    uint32_t crc = DecodeFixed32(header);
    uint32_t len = DecodeFixed32(header + 4);
    if (data.size() - pos - kLogHeaderSize < len) break;  // Truncated record.
    if (crc32c::Value(header + 4, 4 + static_cast<size_t>(len)) != crc) break;
    if (!fn(std::string_view(header + kLogHeaderSize, len))) break;
    pos += kLogHeaderSize + len;
  }
  stats->valid_bytes = pos;
  return Status::OK();
}

}  // namespace ledgerkv
