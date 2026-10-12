#include "log.h"

#include "coding.h"
#include "crc32c.h"

namespace ledgerkv {

void AppendLogRecord(std::string_view payload, std::string* dst) {
  std::string len;
  PutFixed32(&len, static_cast<uint32_t>(payload.size()));
  uint32_t crc = crc32c::Extend(crc32c::Value(len.data(), len.size()), payload.data(), payload.size());
  PutFixed32(dst, crc);
  dst->append(len);
  dst->append(payload);
}

Status LogWriter::Open(const std::string& path, bool sync, std::unique_ptr<LogWriter>* out, Env* env) {
  std::unique_ptr<WritableFile> file;
  Status s = env->NewAppendableFile(path, &file);
  if (!s.ok()) return s;
  out->reset(new LogWriter(std::move(file), sync));
  return Status::OK();
}

Status LogWriter::AddRecord(std::string_view payload) {
  scratch_.clear();
  AppendLogRecord(payload, &scratch_);
  return AddFramedRecords(scratch_);
}

Status LogWriter::AddFramedRecords(std::string_view framed) {
  Status s = file_->Append(framed);
  if (s.ok() && sync_) s = file_->Sync();
  return s;
}

Status ReadLog(const std::string& path, const std::function<bool(std::string_view)>& fn,
               LogReadStats* stats, Env* env) {
  *stats = LogReadStats();
  if (!env->FileExists(path)) return Status::OK();
  std::string data;
  Status s = env->ReadFileToString(path, &data);
  if (!s.ok()) return s;
  stats->file_bytes = data.size();
  stats->valid_bytes = ParseLogRecords(data, fn);
  return Status::OK();
}

uint64_t ParseLogRecords(std::string_view data, const std::function<bool(std::string_view)>& fn) {
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
  return pos;
}

}  // namespace ledgerkv
