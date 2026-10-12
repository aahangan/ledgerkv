#include "wal.h"

#include <fcntl.h>
#include <unistd.h>

#include <filesystem>

#include "file_util.h"
#include "testing.h"

using namespace ledgerkv;

namespace {

WalRecord MakeRecord(uint64_t seq) {
  if (seq % 3 == 0) return {RecordType::kDelete, seq, "key" + std::to_string(seq), ""};
  return {RecordType::kPut, seq, "key" + std::to_string(seq), std::string(seq * 7 % 300, 'v')};
}

void WriteRecords(const std::string& path, int n) {
  std::unique_ptr<WalWriter> w;
  CHECK_OK(WalWriter::Open(path, /*sync=*/false, &w));
  for (int i = 1; i <= n; i++) CHECK_OK(w->Append(MakeRecord(i)));
}

void CheckRecords(const WalReadResult& r, size_t n) {
  CHECK_EQ(r.records.size(), n);
  for (size_t i = 0; i < n; i++) {
    WalRecord want = MakeRecord(i + 1);
    CHECK(r.records[i].type == want.type);
    CHECK_EQ(r.records[i].seq, want.seq);
    CHECK_EQ(r.records[i].key, want.key);
    CHECK_EQ(r.records[i].value, want.value);
  }
}

void WriteBytes(const std::string& path, const std::string& data) {
  int fd = ::open(path.c_str(), O_WRONLY | O_TRUNC | O_CREAT, 0644);
  CHECK(fd >= 0);
  CHECK_OK(WriteAll(fd, data.data(), data.size(), path));
  ::close(fd);
}

}  // namespace

TEST(WalRoundTrip) {
  std::string path = testing::TempDir() + "/wal.log";
  WriteRecords(path, 100);
  WalReadResult r;
  CHECK_OK(ReadWal(path, &r));
  CheckRecords(r, 100);
  CHECK_EQ(r.valid_bytes, r.file_bytes);
}

TEST(WalMissingFileIsEmpty) {
  WalReadResult r;
  CHECK_OK(ReadWal(testing::TempDir() + "/nope.log", &r));
  CHECK(r.records.empty());
}

// Simulates a crash at every possible byte offset within the final record: the
// log must recover exactly the records before it, never a partial one.
TEST(WalTornTailAtEveryOffset) {
  std::string path = testing::TempDir() + "/wal.log";
  WriteRecords(path, 20);
  std::string full;
  CHECK_OK(ReadFileToString(path, &full));

  std::string prefix;
  EncodeWalRecord(MakeRecord(1), &prefix);
  for (int i = 2; i <= 19; i++) EncodeWalRecord(MakeRecord(i), &prefix);
  CHECK(prefix.size() < full.size());
  CHECK_EQ(full.compare(0, prefix.size(), prefix), 0);

  for (size_t cut = prefix.size(); cut < full.size(); cut++) {
    WriteBytes(path, full.substr(0, cut));
    WalReadResult r;
    CHECK_OK(ReadWal(path, &r));
    CheckRecords(r, 19);
    CHECK_EQ(r.valid_bytes, prefix.size());
  }
}

// Flipping any single bit must be detected; recovery keeps the records before
// the damaged one and nothing after it.
TEST(WalBitFlipStopsRecovery) {
  std::string path = testing::TempDir() + "/wal.log";
  WriteRecords(path, 10);
  std::string full;
  CHECK_OK(ReadFileToString(path, &full));

  std::vector<size_t> boundaries{0};
  for (int i = 1; i <= 10; i++) {
    std::string enc;
    EncodeWalRecord(MakeRecord(i), &enc);
    boundaries.push_back(boundaries.back() + enc.size());
  }

  for (size_t byte = 0; byte < full.size(); byte++) {
    for (int bit = 0; bit < 8; bit++) {
      std::string damaged = full;
      damaged[byte] ^= static_cast<char>(1 << bit);
      WriteBytes(path, damaged);
      WalReadResult r;
      CHECK_OK(ReadWal(path, &r));
      size_t damaged_record = 0;
      while (boundaries[damaged_record + 1] <= byte) damaged_record++;
      CheckRecords(r, damaged_record);
    }
  }
}
