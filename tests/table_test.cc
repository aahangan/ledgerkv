#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <string>

#include "bloom.h"
#include "file_util.h"
#include "table.h"
#include "table_builder.h"
#include "testing.h"
#include "version_edit.h"

using namespace ledgerkv;

namespace {

std::string Key(int i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "key%08d", i);
  return buf;
}

std::string Value(int i) { return std::string(static_cast<size_t>(i % 97), static_cast<char>('a' + i % 26)); }

// Even keys from 0 to 2n; every tenth is a delete.
std::string BuildTable(int n, uint64_t* size) {
  std::string path = testing::TempDir() + "/000001.sst";
  TableBuilder b(TableBuilder::Options(), path);
  CHECK_OK(b.Open());
  for (int i = 0; i < n; i++) {
    int k = 2 * i;
    if (i % 10 == 0) {
      b.Add(Key(k), static_cast<uint64_t>(k + 1), RecordType::kDelete, "");
    } else {
      b.Add(Key(k), static_cast<uint64_t>(k + 1), RecordType::kPut, Value(k));
    }
  }
  CHECK_OK(b.Finish());
  *size = b.FileSize();
  return path;
}

}  // namespace

TEST(BloomNoFalseNegatives) {
  BloomFilterBuilder b(10);
  for (int i = 0; i < 10000; i++) b.AddKey(Key(i));
  std::string filter = b.Finish();
  for (int i = 0; i < 10000; i++) CHECK(BloomMayContain(filter, Key(i)));
}

TEST(BloomFalsePositiveRate) {
  constexpr int kKeys = 10000, kProbes = 100000;
  BloomFilterBuilder b(10);
  for (int i = 0; i < kKeys; i++) b.AddKey(Key(i));
  std::string filter = b.Finish();
  int false_positives = 0;
  for (int i = kKeys; i < kKeys + kProbes; i++) false_positives += BloomMayContain(filter, Key(i));
  double rate = static_cast<double>(false_positives) / kProbes;
  std::printf("    bloom false-positive rate at 10 bits/key: %.3f%%\n", rate * 100);
  CHECK(rate < 0.015);  // Theory says ~0.82%.
}

TEST(BloomEmptyFilterMatchesEverything) {
  CHECK(BloomMayContain("", "anything"));
  BloomFilterBuilder b(10);
  std::string filter = b.Finish();
  CHECK(!BloomMayContain(filter, "anything"));
}

TEST(TableGetAndForEach) {
  uint64_t size;
  std::string path = BuildTable(5000, &size);
  std::shared_ptr<Table> t;
  CHECK_OK(Table::Open(path, size, &t));

  for (int i = 0; i < 5000; i++) {
    int k = 2 * i;
    std::string v;
    LookupResult r;
    CHECK_OK(t->Get(Key(k), kMaxSequence, &v, &r));
    if (i % 10 == 0) {
      CHECK(r == LookupResult::kDeleted);
    } else {
      CHECK(r == LookupResult::kFound);
      CHECK_EQ(v, Value(k));
    }
    CHECK_OK(t->Get(Key(k + 1), kMaxSequence, &v, &r));  // Odd keys were never added.
    CHECK(r == LookupResult::kMissing);
  }
  std::string v;
  LookupResult r;
  CHECK_OK(t->Get("zzz", kMaxSequence, &v, &r));  // Past the last key.
  CHECK(r == LookupResult::kMissing);

  int count = 0;
  std::string prev;
  CHECK_OK(t->ForEach([&](std::string_view key, uint64_t seq, RecordType, std::string_view) {
    CHECK(count == 0 || key > std::string_view(prev));
    CHECK_EQ(seq, static_cast<uint64_t>(2 * count + 1));
    prev.assign(key);
    count++;
    return true;
  }));
  CHECK_EQ(count, 5000);
}

// Any flipped byte must surface as an error (or a bloom/index miss when it
// lands in the filter), never as a wrong value.
TEST(TableDetectsCorruption) {
  uint64_t size;
  std::string path = BuildTable(2000, &size);
  std::string original;
  CHECK_OK(ReadFileToString(path, &original));

  int detected = 0;
  for (uint64_t pos = 0; pos < size; pos += 97) {
    std::string damaged = original;
    damaged[pos] ^= 0x40;
    int fd = ::open(path.c_str(), O_WRONLY | O_TRUNC);
    CHECK(fd >= 0);
    CHECK_OK(WriteAll(fd, damaged.data(), damaged.size(), path));
    ::close(fd);

    std::shared_ptr<Table> t;
    Status s = Table::Open(path, size, &t);
    if (!s.ok()) {
      CHECK(s.IsCorruption());
      detected++;
      continue;
    }
    bool saw_error = false;
    for (int i = 1; i < 2000; i += 7) {
      if (i % 10 == 0) continue;
      std::string v;
      LookupResult r;
      Status gs = t->Get(Key(2 * i), kMaxSequence, &v, &r);
      if (!gs.ok()) {
        CHECK(gs.IsCorruption());
        saw_error = true;
      } else if (r == LookupResult::kFound) {
        CHECK_EQ(v, Value(2 * i));
      }
    }
    detected += saw_error;
  }
  CHECK(detected > 0);
}

TEST(TableRejectsTruncatedFile) {
  uint64_t size;
  std::string path = BuildTable(100, &size);
  CHECK_EQ(::truncate(path.c_str(), static_cast<off_t>(size - 1)), 0);
  std::shared_ptr<Table> t;
  CHECK(Table::Open(path, size - 1, &t).IsCorruption());
}

TEST(VersionEditRoundTrip) {
  VersionEdit e;
  e.log_number = 7;
  e.next_file_number = 12;
  e.last_sequence = 123456789012ULL;
  e.added_tables.push_back({1, 9, 4096, "aaa", "mmm"});
  e.added_tables.push_back({6, 10, 1, "", std::string("\0\xff", 2)});
  e.deleted_tables = {3, 4};
  std::string enc;
  e.EncodeTo(&enc);

  VersionEdit d;
  CHECK(d.DecodeFrom(enc));
  CHECK_EQ(*d.log_number, 7u);
  CHECK_EQ(*d.next_file_number, 12u);
  CHECK_EQ(*d.last_sequence, 123456789012ULL);
  CHECK_EQ(d.added_tables.size(), 2u);
  CHECK_EQ(d.added_tables[0].level, 1);
  CHECK_EQ(d.added_tables[1].level, 6);
  CHECK_EQ(d.added_tables[1].largest, std::string("\0\xff", 2));
  CHECK_EQ(d.deleted_tables.size(), 2u);
  CHECK(!d.DecodeFrom(enc.substr(0, enc.size() - 1)));
}
