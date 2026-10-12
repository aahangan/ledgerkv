#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

#include "file_util.h"
#include "filename.h"
#include "log.h"
#include "ledgerkv/db.h"
#include "testing.h"

using namespace ledgerkv;

namespace {

Options SmallBuffer() {
  Options o;
  o.sync = false;
  o.write_buffer_size = 64 << 10;
  return o;
}

std::unique_ptr<DB> Open(const std::string& dir, const Options& o = SmallBuffer()) {
  std::unique_ptr<DB> db;
  CHECK_OK(DB::Open(o, dir, &db));
  return db;
}

std::string Key(int i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "key%08d", i);
  return buf;
}

std::string Value(int i, int version) {
  return std::to_string(version) + ":" + std::string(static_cast<size_t>(50 + i % 50), 'v');
}

int CountFiles(const std::string& dir, FileType want) {
  std::vector<std::string> names;
  CHECK_OK(ListDir(dir, &names));
  int n = 0;
  for (const auto& name : names) {
    uint64_t number;
    n += ParseFileName(name, &number) == want;
  }
  return n;
}

}  // namespace

TEST(FlushCreatesTablesAndBoundsLogs) {
  std::string dir = testing::TempDir();
  auto db = Open(dir);
  for (int i = 0; i < 20000; i++) CHECK_OK(db->Put(Key(i), Value(i, 1)));
  CHECK_OK(db->Flush());
  DBStats stats = db->GetStats();
  CHECK(stats.flushes >= 10);
  // Flushed logs are deleted; only the active one remains.
  CHECK_EQ(CountFiles(dir, FileType::kLog), 1);
  CHECK_EQ(CountFiles(dir, FileType::kTable), static_cast<int>(stats.num_tables));
  for (int i = 0; i < 20000; i++) {
    std::string v;
    CHECK_OK(db->Get(Key(i), &v));
    CHECK_EQ(v, Value(i, 1));
  }
}

// Newer writes and tombstones in the memtable or newer tables must shadow
// older values in older tables, both live and after reopening.
TEST(NewerLayersShadowOlder) {
  std::string dir = testing::TempDir();
  auto check = [](DB* db) {
    for (int i = 0; i < 6000; i++) {
      std::string v;
      Status s = db->Get(Key(i), &v);
      if (i % 3 == 0) {
        CHECK(s.IsNotFound());
      } else {
        CHECK_OK(s);
        CHECK_EQ(v, Value(i, i % 3 == 1 ? 2 : 1));
      }
    }
  };
  {
    auto db = Open(dir);
    for (int i = 0; i < 6000; i++) CHECK_OK(db->Put(Key(i), Value(i, 1)));
    CHECK_OK(db->Flush());
    for (int i = 0; i < 6000; i += 3) CHECK_OK(db->Delete(Key(i)));
    for (int i = 1; i < 6000; i += 3) CHECK_OK(db->Put(Key(i), Value(i, 2)));
    check(db.get());  // Mix of memtable and tables.
  }
  auto db = Open(dir);
  check(db.get());
}

TEST(ReopenRecoversUnflushedWrites) {
  std::string dir = testing::TempDir();
  Options big = SmallBuffer();
  big.write_buffer_size = 64 << 20;  // Nothing flushes on its own.
  {
    auto db = Open(dir, big);
    for (int i = 0; i < 1000; i++) CHECK_OK(db->Put(Key(i), Value(i, 1)));
    CHECK_EQ(db->GetStats().num_tables, 0u);
  }
  auto db = Open(dir, big);
  // Recovery flushes replayed writes to a table and drops the old log.
  CHECK_EQ(db->GetStats().num_tables, 1u);
  CHECK_EQ(CountFiles(dir, FileType::kLog), 1);
  for (int i = 0; i < 1000; i++) {
    std::string v;
    CHECK_OK(db->Get(Key(i), &v));
  }
}

TEST(OrphanTablesAreDeletedOnOpen) {
  std::string dir = testing::TempDir();
  { auto db = Open(dir); CHECK_OK(db->Put("a", "1")); }
  // Simulate a crash after writing a table but before the MANIFEST edit.
  std::string orphan = TableFileName(dir, 999);
  int fd = ::open(orphan.c_str(), O_WRONLY | O_CREAT, 0644);
  CHECK(fd >= 0);
  CHECK_OK(WriteAll(fd, "garbage", 7, orphan));
  ::close(fd);

  auto db = Open(dir);
  CHECK_EQ(::access(orphan.c_str(), F_OK), -1);
  std::string v;
  CHECK_OK(db->Get("a", &v));
  // New files must not reuse the orphan's number.
  for (int i = 0; i < 5000; i++) CHECK_OK(db->Put(Key(i), Value(i, 1)));
  CHECK_OK(db->Flush());
  CHECK_OK(db->Get(Key(4999), &v));
}

// A torn MANIFEST tail (crash mid-append) loses only the uncommitted edit.
// The flush it described is redone from the log, which was not yet deleted.
TEST(TornManifestTailIsTolerated) {
  std::string dir = testing::TempDir();
  { auto db = Open(dir); for (int i = 0; i < 3000; i++) CHECK_OK(db->Put(Key(i), Value(i, 1))); }
  std::string manifest = ManifestFileName(dir);
  std::string data;
  CHECK_OK(ReadFileToString(manifest, &data));
  int fd = ::open(manifest.c_str(), O_WRONLY | O_APPEND);
  CHECK(fd >= 0);
  CHECK_OK(WriteAll(fd, "\x01\x02\x03\x04\x05\x06", 6, manifest));
  ::close(fd);

  {
    auto db = Open(dir);
    for (int i = 0; i < 3000; i++) {
      std::string v;
      CHECK_OK(db->Get(Key(i), &v));
    }
  }
  auto db = Open(dir);  // And the truncated MANIFEST keeps working.
  std::string v;
  CHECK_OK(db->Get(Key(2999), &v));
}

// Flipping a bit in the last MANIFEST record is indistinguishable from a torn
// append, so the edit is dropped. The table it named must not be deleted as
// an orphan, since it may hold the only copy of flushed data.
TEST(DamagedManifestTailKeepsUnreferencedTables) {
  std::string dir = testing::TempDir();
  {
    auto db = Open(dir);
    CHECK_OK(db->Put("a", "1"));
    CHECK_OK(db->Flush());
  }
  int tables_before = CountFiles(dir, FileType::kTable);
  std::string manifest = ManifestFileName(dir);
  std::string data;
  CHECK_OK(ReadFileToString(manifest, &data));
  data[data.size() - 1] ^= 0x01;
  int fd = ::open(manifest.c_str(), O_WRONLY | O_TRUNC);
  CHECK_OK(WriteAll(fd, data.data(), data.size(), manifest));
  ::close(fd);

  auto db = Open(dir);
  // The table the dropped edit named is set aside in lost/, not deleted.
  CHECK_EQ(CountFiles(dir + "/lost", FileType::kTable), tables_before);
}

// A record with a valid checksum that does not parse is real corruption, not
// a torn write, so open must fail rather than guess at the live table set.
TEST(UndecodableManifestRecordFailsOpen) {
  std::string dir = testing::TempDir();
  { auto db = Open(dir); }
  std::unique_ptr<LogWriter> w;
  CHECK_OK(LogWriter::Open(ManifestFileName(dir), false, &w));
  CHECK_OK(w->AddRecord("\x7f\x7f garbage"));
  w.reset();
  std::unique_ptr<DB> db;
  CHECK(DB::Open(SmallBuffer(), dir, &db).IsCorruption());
}

// Readers and writers race with background flushes. Run under TSan in CI.
TEST(ConcurrentReadsDuringFlushes) {
  std::string dir = testing::TempDir();
  auto db = Open(dir);
  constexpr int kWriters = 4, kPerWriter = 5000;
  std::atomic<bool> done{false};
  std::vector<std::thread> threads;
  for (int t = 0; t < kWriters; t++) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < kPerWriter; i++) CHECK_OK(db->Put(Key(t * kPerWriter + i), Value(i, t)));
    });
  }
  std::thread reader([&] {
    while (!done) {
      for (int t = 0; t < kWriters; t++) {
        std::string v;
        Status s = db->Get(Key(t * kPerWriter), &v);
        CHECK(s.ok() || s.IsNotFound());
        if (s.ok()) CHECK_EQ(v, Value(0, t));
      }
    }
  });
  for (auto& th : threads) th.join();
  done = true;
  reader.join();
  CHECK(db->GetStats().flushes > 0);
  for (int t = 0; t < kWriters; t++) {
    for (int i = 0; i < kPerWriter; i++) {
      std::string v;
      CHECK_OK(db->Get(Key(t * kPerWriter + i), &v));
      CHECK_EQ(v, Value(i, t));
    }
  }
}
