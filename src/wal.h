#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "dbformat.h"
#include "ledgerkv/status.h"
#include "log.h"

namespace ledgerkv {

// Write-ahead log records, framed by log.h. Two payload layouts:
//
//   single op:  type (1, put or delete) | seq (8) | varint klen | key | [varint vlen | value]
//   batch:      3 (1) | first seq (8) | WriteBatch contents
//
// A batch's operations take consecutive sequence numbers starting at the
// first. Because a batch is one framed record, a crash leaves all of it or
// none of it.

constexpr uint8_t kWalBatch = 3;

struct WalRecord {
  RecordType type;
  uint64_t seq;
  std::string key;
  std::string value;  // Empty for deletes.
};

void EncodeWalPayload(RecordType type, uint64_t seq, std::string_view key, std::string_view value,
                      std::string* dst);
// Appends the payload of a batch whose first operation gets sequence seq.
void EncodeWalBatchPayload(uint64_t seq, std::string_view batch_contents, std::string* dst);
// Decodes either layout, appending one record per operation.
bool DecodeWalPayload(std::string_view payload, std::vector<WalRecord>* records);

// Serializes a record, including its log framing, onto the end of *dst.
void EncodeWalRecord(const WalRecord& record, std::string* dst);

class WalWriter {
 public:
  static Status Open(const std::string& path, bool sync, std::unique_ptr<WalWriter>* out);

  Status Append(RecordType type, uint64_t seq, std::string_view key, std::string_view value);
  Status Append(const WalRecord& r) { return Append(r.type, r.seq, r.key, r.value); }
  // Appends pre-framed records in one write and at most one sync.
  Status AppendFramed(std::string_view framed) { return log_->AddFramedRecords(framed); }

 private:
  explicit WalWriter(std::unique_ptr<LogWriter> log) : log_(std::move(log)) {}

  std::unique_ptr<LogWriter> log_;
  std::string scratch_;
};

struct WalReadResult {
  std::vector<WalRecord> records;
  uint64_t valid_bytes = 0;  // Length of the prefix holding intact records.
  uint64_t file_bytes = 0;
};

// Reads every intact record from the start of the log. Missing files read as
// empty. A torn or corrupt tail is reported through valid_bytes < file_bytes,
// and the caller decides whether to truncate it.
Status ReadWal(const std::string& path, WalReadResult* out);

}  // namespace ledgerkv
