#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ledgerkv/status.h"

namespace ledgerkv {

// On-disk record layout (little-endian):
//
//   +---------+---------+------------------------------------------+
//   | crc (4) | len (4) | payload (len bytes)                      |
//   +---------+---------+------------------------------------------+
//   payload = type (1) | seq (8) | varint klen | key | [varint vlen | value]
//
// crc is CRC-32C over len and payload, so a corrupted length field is caught
// too. A crash can leave a torn record at the tail of the log; recovery stops
// at the first record that is truncated or fails its checksum.

enum class RecordType : uint8_t { kPut = 1, kDelete = 2 };

struct WalRecord {
  RecordType type;
  uint64_t seq;
  std::string key;
  std::string value;  // Empty for deletes.
};

constexpr size_t kWalHeaderSize = 8;

// Serializes a record, including its header, onto the end of *dst.
void EncodeWalRecord(const WalRecord& record, std::string* dst);

class WalWriter {
 public:
  // Opens path for appending, creating it if needed.
  static Status Open(const std::string& path, bool sync, std::unique_ptr<WalWriter>* out);

  ~WalWriter();
  WalWriter(const WalWriter&) = delete;
  WalWriter& operator=(const WalWriter&) = delete;

  // Appends a record. If sync is enabled, it is durable once this returns OK.
  Status Append(const WalRecord& record);

 private:
  WalWriter(std::string path, int fd, bool sync) : path_(std::move(path)), fd_(fd), sync_(sync) {}

  std::string path_;
  int fd_;
  bool sync_;
  std::string scratch_;
};

struct WalReadResult {
  std::vector<WalRecord> records;
  uint64_t valid_bytes = 0;  // Length of the prefix holding intact records.
  uint64_t file_bytes = 0;
};

// Reads every intact record from the start of the log. Missing files read as
// empty. A torn or corrupt tail is not an error: it is reported through
// valid_bytes < file_bytes, and the caller decides whether to truncate it.
Status ReadWal(const std::string& path, WalReadResult* out);

}  // namespace ledgerkv
