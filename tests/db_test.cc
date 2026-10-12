#include "ledgerkv/db.h"

#include <fcntl.h>
#include <unistd.h>

#include <thread>
#include <vector>

#include "file_util.h"
#include "testing.h"

using namespace ledgerkv;

namespace {

std::unique_ptr<DB> OpenDB(const std::string& dir, bool sync = false) {
  Options options;
  options.sync = sync;
  std::unique_ptr<DB> db;
  CHECK_OK(DB::Open(options, dir, &db));
  return db;
}

}  // namespace

TEST(DBPutGetDelete) {
  auto db = OpenDB(testing::TempDir());
  std::string v;
  CHECK(db->Get("a", &v).IsNotFound());
  CHECK_OK(db->Put("a", "1"));
  CHECK_OK(db->Get("a", &v));
  CHECK_EQ(v, "1");
  CHECK_OK(db->Put("a", "2"));
  CHECK_OK(db->Get("a", &v));
  CHECK_EQ(v, "2");
  CHECK_OK(db->Delete("a"));
  CHECK(db->Get("a", &v).IsNotFound());
}

TEST(DBEmptyKeyAndBinaryValues) {
  auto db = OpenDB(testing::TempDir());
  std::string binary("\0\xff\0x", 4);
  CHECK_OK(db->Put("", binary));
  std::string v;
  CHECK_OK(db->Get("", &v));
  CHECK_EQ(v, binary);
}

TEST(DBRecoversAfterReopen) {
  std::string dir = testing::TempDir();
  {
    auto db = OpenDB(dir);
    for (int i = 0; i < 1000; i++) CHECK_OK(db->Put("k" + std::to_string(i), "v" + std::to_string(i)));
    for (int i = 0; i < 1000; i += 2) CHECK_OK(db->Delete("k" + std::to_string(i)));
  }
  auto db = OpenDB(dir);
  for (int i = 0; i < 1000; i++) {
    std::string v;
    Status s = db->Get("k" + std::to_string(i), &v);
    if (i % 2 == 0) {
      CHECK(s.IsNotFound());
    } else {
      CHECK_OK(s);
      CHECK_EQ(v, "v" + std::to_string(i));
    }
  }
}

// After recovering from a torn tail, new writes must land after the last good
// record and survive the next recovery.
TEST(DBWritesAfterTornTailSurvive) {
  std::string dir = testing::TempDir();
  {
    auto db = OpenDB(dir);
    CHECK_OK(db->Put("a", "1"));
    CHECK_OK(db->Put("b", "2"));
  }
  std::string wal = dir + "/wal.log";
  std::string data;
  CHECK_OK(ReadFileToString(wal, &data));
  CHECK_EQ(::truncate(wal.c_str(), static_cast<off_t>(data.size() - 3)), 0);
  {
    auto db = OpenDB(dir);
    std::string v;
    CHECK_OK(db->Get("a", &v));
    CHECK(db->Get("b", &v).IsNotFound());
    CHECK_OK(db->Put("c", "3"));
  }
  auto db = OpenDB(dir);
  std::string v;
  CHECK_OK(db->Get("a", &v));
  CHECK_OK(db->Get("c", &v));
  CHECK_EQ(v, "3");
}

TEST(DBRejectsSecondOpen) {
  std::string dir = testing::TempDir();
  auto db = OpenDB(dir);
  std::unique_ptr<DB> second;
  CHECK(DB::Open(Options(), dir, &second).IsIOError());
}

TEST(DBConcurrentWriters) {
  std::string dir = testing::TempDir();
  constexpr int kThreads = 8, kPerThread = 2000;
  {
    auto db = OpenDB(dir);
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; t++) {
      threads.emplace_back([&, t] {
        for (int i = 0; i < kPerThread; i++) {
          std::string key = "t" + std::to_string(t) + "-" + std::to_string(i);
          CHECK_OK(db->Put(key, key));
          std::string v;
          CHECK_OK(db->Get(key, &v));
        }
      });
    }
    for (auto& th : threads) th.join();
  }
  auto db = OpenDB(dir);
  for (int t = 0; t < kThreads; t++) {
    for (int i = 0; i < kPerThread; i++) {
      std::string key = "t" + std::to_string(t) + "-" + std::to_string(i), v;
      CHECK_OK(db->Get(key, &v));
      CHECK_EQ(v, key);
    }
  }
}
