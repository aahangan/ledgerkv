#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "ledgerkv/status.h"

namespace ledgerkv {

// Record framing shared by the write-ahead log and the MANIFEST:
//
//   +---------+---------+---------------------+
//   | crc (4) | len (4) | payload (len bytes) |
//   +---------+---------+---------------------+
//
// crc is CRC-32C over len and payload, so a corrupted length field is caught
// too. A crash can leave a torn record at the tail; readers stop at the first
// record that is truncated or fails its checksum.

constexpr size_t kLogHeaderSize = 8;

// Frames payload and appends it to *dst.
void AppendLogRecord(std::string_view payload, std::string* dst);

class LogWriter {
 public:
  // Opens path for appending, creating it if needed.
  static Status Open(const std::string& path, bool sync, std::unique_ptr<LogWriter>* out);

  ~LogWriter();
  LogWriter(const LogWriter&) = delete;
  LogWriter& operator=(const LogWriter&) = delete;

  // Appends one framed record. If sync is enabled, it is durable once this
  // returns OK.
  Status AddRecord(std::string_view payload);

 private:
  LogWriter(std::string path, int fd, bool sync) : path_(std::move(path)), fd_(fd), sync_(sync) {}

  std::string path_;
  int fd_;
  bool sync_;
  std::string scratch_;
};

struct LogReadStats {
  uint64_t valid_bytes = 0;  // Length of the prefix holding accepted records.
  uint64_t file_bytes = 0;
};

// Calls fn on each intact record in order. Stops at the first truncated or
// corrupt record, or when fn returns false (that record is not counted in
// valid_bytes). A missing file reads as empty. A torn tail is not an error:
// it shows up as valid_bytes < file_bytes.
Status ReadLog(const std::string& path, const std::function<bool(std::string_view)>& fn,
               LogReadStats* stats);

}  // namespace ledgerkv
