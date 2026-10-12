#include "wal.h"

#include "coding.h"

namespace ledgerkv {

void EncodeWalPayload(RecordType type, uint64_t seq, std::string_view key, std::string_view value,
                      std::string* dst) {
  dst->push_back(static_cast<char>(type));
  PutFixed64(dst, seq);
  PutLengthPrefixed(dst, key);
  if (type == RecordType::kPut) PutLengthPrefixed(dst, value);
}

bool DecodeWalPayload(std::string_view payload, WalRecord* r) {
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

void EncodeWalRecord(const WalRecord& r, std::string* dst) {
  std::string payload;
  EncodeWalPayload(r.type, r.seq, r.key, r.value, &payload);
  AppendLogRecord(payload, dst);
}

Status WalWriter::Open(const std::string& path, bool sync, std::unique_ptr<WalWriter>* out) {
  std::unique_ptr<LogWriter> log;
  Status s = LogWriter::Open(path, sync, &log);
  if (!s.ok()) return s;
  out->reset(new WalWriter(std::move(log)));
  return Status::OK();
}

Status WalWriter::Append(RecordType type, uint64_t seq, std::string_view key, std::string_view value) {
  scratch_.clear();
  EncodeWalPayload(type, seq, key, value, &scratch_);
  return log_->AddRecord(scratch_);
}

Status ReadWal(const std::string& path, WalReadResult* out) {
  *out = WalReadResult();
  LogReadStats stats;
  Status s = ReadLog(
      path,
      [&](std::string_view payload) {
        WalRecord record;
        if (!DecodeWalPayload(payload, &record)) return false;
        out->records.push_back(std::move(record));
        return true;
      },
      &stats);
  out->valid_bytes = stats.valid_bytes;
  out->file_bytes = stats.file_bytes;
  return s;
}

}  // namespace ledgerkv
