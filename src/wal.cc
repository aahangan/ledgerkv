#include "wal.h"

#include "coding.h"
#include "write_batch_internal.h"

namespace ledgerkv {

void EncodeWalPayload(RecordType type, uint64_t seq, std::string_view key, std::string_view value,
                      std::string* dst) {
  dst->push_back(static_cast<char>(type));
  PutFixed64(dst, seq);
  PutLengthPrefixed(dst, key);
  if (type == RecordType::kPut) PutLengthPrefixed(dst, value);
}

void EncodeWalBatchPayload(uint64_t seq, std::string_view batch_contents, std::string* dst) {
  dst->push_back(static_cast<char>(kWalBatch));
  PutFixed64(dst, seq);
  dst->append(batch_contents);
}

bool DecodeWalPayload(std::string_view payload, std::vector<WalRecord>* records) {
  if (payload.size() < 9) return false;
  auto kind = static_cast<uint8_t>(payload[0]);
  uint64_t seq = DecodeFixed64(payload.data() + 1);
  payload.remove_prefix(9);

  if (kind == kWalBatch) {
    std::vector<WalRecord> batch;
    bool ok = WriteBatchInternal::Iterate(payload, [&](RecordType type, std::string_view key, std::string_view value) {
      batch.push_back({type, seq++, std::string(key), std::string(value)});
    });
    if (!ok) return false;
    for (auto& r : batch) records->push_back(std::move(r));
    return true;
  }

  auto type = static_cast<RecordType>(kind);
  if (type != RecordType::kPut && type != RecordType::kDelete) return false;
  std::string_view key, value;
  if (!GetLengthPrefixed(&payload, &key)) return false;
  if (type == RecordType::kPut && !GetLengthPrefixed(&payload, &value)) return false;
  if (!payload.empty()) return false;
  records->push_back({type, seq, std::string(key), std::string(value)});
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
      [&](std::string_view payload) { return DecodeWalPayload(payload, &out->records); },
      &stats);
  out->valid_bytes = stats.valid_bytes;
  out->file_bytes = stats.file_bytes;
  return s;
}

}  // namespace ledgerkv
