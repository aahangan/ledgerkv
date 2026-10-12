// Fuzzes the decoders for everything read back from the WAL and MANIFEST:
// log record framing, WAL payloads (single ops and batches), and version
// edits. None may crash or read out of bounds on any input, and anything that
// decodes must survive an encode/decode round trip unchanged.

#include <string_view>
#include <vector>

#include "fuzz.h"
#include "log.h"
#include "version_edit.h"
#include "wal.h"
#include "write_batch_internal.h"

using namespace ledgerkv;

std::vector<std::string> FuzzSeeds() { return fuzz::LogSeeds(); }

namespace {

void CheckVersionEditRoundTrip(std::string_view in) {
  VersionEdit e;
  if (!e.DecodeFrom(in)) return;
  std::string enc;
  e.EncodeTo(&enc);
  VersionEdit again;
  FUZZ_CHECK(again.DecodeFrom(enc));
  std::string enc2;
  again.EncodeTo(&enc2);
  FUZZ_CHECK(enc == enc2);
}

bool SameRecords(const std::vector<WalRecord>& a, const std::vector<WalRecord>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); i++) {
    if (a[i].type != b[i].type || a[i].seq != b[i].seq || a[i].key != b[i].key || a[i].value != b[i].value) {
      return false;
    }
  }
  return true;
}

void CheckWalPayloadRoundTrip(std::string_view in) {
  std::vector<WalRecord> records;
  if (!DecodeWalPayload(in, &records)) return;
  // Re-encode each operation on its own and decode again: same operations.
  std::vector<WalRecord> again;
  for (const WalRecord& r : records) {
    std::string payload;
    EncodeWalPayload(r.type, r.seq, r.key, r.value, &payload);
    FUZZ_CHECK(DecodeWalPayload(payload, &again));
  }
  FUZZ_CHECK(SameRecords(records, again));
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  std::string_view in(reinterpret_cast<const char*>(data), size);

  // As a log file: framing, then each accepted record as both kinds of payload.
  size_t accepted = 0;
  uint64_t valid = ParseLogRecords(in, [&](std::string_view record) {
    accepted++;
    CheckWalPayloadRoundTrip(record);
    CheckVersionEditRoundTrip(record);
    return true;
  });
  FUZZ_CHECK(valid <= size);
  // The accepted prefix parses to the same records on its own.
  size_t again = 0;
  FUZZ_CHECK(ParseLogRecords(in.substr(0, valid), [&](std::string_view) { return ++again, true; }) == valid);
  FUZZ_CHECK(again == accepted);

  // And the raw input as a payload, which skips the checksum barrier.
  CheckWalPayloadRoundTrip(in);
  CheckVersionEditRoundTrip(in);
  return 0;
}
