#include "wal.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <string_view>

#include "coding.h"
#include "crc32c.h"
#include "file_util.h"

namespace ledgerkv {

void EncodeWalRecord(const WalRecord& record, std::string* dst) {
  size_t start = dst->size();
  dst->append(kWalHeaderSize, '\0');  // Placeholder for crc and len.
  dst->push_back(static_cast<char>(record.type));
  PutFixed64(dst, record.seq);
  PutLengthPrefixed(dst, record.key);
  if (record.type == RecordType::kPut) PutLengthPrefixed(dst, record.value);

  auto len = static_cast<uint32_t>(dst->size() - start - kWalHeaderSize);
  std::string len_bytes;
  PutFixed32(&len_bytes, len);
  dst->replace(start + 4, 4, len_bytes);

  uint32_t crc = crc32c::Value(dst->data() + start + 4, dst->size() - start - 4);
  std::string crc_bytes;
  PutFixed32(&crc_bytes, crc);
  dst->replace(start, 4, crc_bytes);
}

Status WalWriter::Open(const std::string& path, bool sync, std::unique_ptr<WalWriter>* out) {
  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  if (fd < 0) return IOErrorFromErrno("open " + path);
  out->reset(new WalWriter(path, fd, sync));
  return Status::OK();
}

WalWriter::~WalWriter() { ::close(fd_); }

Status WalWriter::Append(const WalRecord& record) {
  scratch_.clear();
  EncodeWalRecord(record, &scratch_);
  Status s = WriteAll(fd_, scratch_.data(), scratch_.size(), path_);
  if (s.ok() && sync_) s = SyncFd(fd_, path_);
  return s;
}

namespace {

bool DecodePayload(std::string_view payload, WalRecord* r) {
  if (payload.size() < 9) return false;
  auto type = static_cast<RecordType>(payload[0]);
  if (type != RecordType::kPut && type != RecordType::kDelete) return false;
  r->type = type;
  r->seq = DecodeFixed64(payload.data() + 1);
  payload.remove_prefix(9);

  std::string_view key, value;
  if (!GetLengthPrefixed(&payload, &key)) return false;
  if (type == RecordType::kPut && !GetLengthPrefixed(&payload, &value)) return false;
  if (!payload.empty()) return false;
  r->key.assign(key);
  r->value.assign(value);
  return true;
}

}  // namespace

Status ReadWal(const std::string& path, WalReadResult* out) {
  *out = WalReadResult();
  struct stat st;
  if (::stat(path.c_str(), &st) != 0) {
    if (errno == ENOENT) return Status::OK();
    return IOErrorFromErrno("stat " + path);
  }
  std::string data;
  Status s = ReadFileToString(path, &data);
  if (!s.ok()) return s;
  out->file_bytes = data.size();

  size_t pos = 0;
  while (data.size() - pos >= kWalHeaderSize) {
    const char* header = data.data() + pos;
    uint32_t crc = DecodeFixed32(header);
    uint32_t len = DecodeFixed32(header + 4);
    if (data.size() - pos - kWalHeaderSize < len) break;  // Truncated record.
    if (crc32c::Value(header + 4, 4 + static_cast<size_t>(len)) != crc) break;

    WalRecord record;
    if (!DecodePayload(std::string_view(header + kWalHeaderSize, len), &record)) break;
    out->records.push_back(std::move(record));
    pos += kWalHeaderSize + len;
  }
  out->valid_bytes = pos;
  return Status::OK();
}

}  // namespace ledgerkv
