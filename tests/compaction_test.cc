#include <sys/stat.h>

#include <atomic>
#include <cstdio>
#include <map>
#include <optional>
#include <random>
#include <thread>
#include <vector>

#include "filename.h"
#include "ledgerkv/db.h"
#include "testing.h"

using namespace ledgerkv;

namespace {

// Tiny limits so a few thousand writes exercise several levels.
Options Tiny() {
  Options o;
  o.sync = false;
  o.write_buffer_size = 32 << 10;
  o.max_bytes_for_level_base = 128 << 10;
  o.target_file_size = 32 << 10;
  return o;
}

std::unique_ptr<DB> Open(const std::string& dir, const Options& o = Tiny()) {
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
  return std::to_string(version) + ":" + std::string(static_cast<size_t>(20 + i % 80), 'v');
}

void CheckModel(DB* db, const std::map<std::string, std::string>& model, int key_space) {
  for (int i = 0; i < key_space; i++) {
    std::string v;
    Status s = db->Get(Key(i), &v);
    auto it = model.find(Key(i));
    if (it == model.end()) {
      if (!s.IsNotFound()) std::fprintf(stderr, "key %d should be absent: %s\n", i, s.ToString().c_str());
      CHECK(s.IsNotFound());
    } else {
      CHECK_OK(s);
      CHECK_EQ(v, it->second);
    }
  }
}

uint64_t FileSize(const std::string& path) {
  struct stat st;
  CHECK_EQ(::stat(path.c_str(), &st), 0);
  return static_cast<uint64_t>(st.st_size);
}

}  // namespace

// Random puts and deletes against a std::map model, reopening periodically.
// Checks every key and the structural invariants along the way.
TEST(CompactionMatchesModel) {
  std::string dir = testing::TempDir();
  constexpr int kKeySpace = 4000, kOps = 60000;
  std::mt19937 rng(1234);
  std::map<std::string, std::string> model;
  auto db = Open(dir);
  for (int op = 1; op <= kOps; op++) {
    int i = static_cast<int>(rng() % kKeySpace);
    if (rng() % 4 == 0) {
      CHECK_OK(db->Delete(Key(i)));
      model.erase(Key(i));
    } else {
      CHECK_OK(db->Put(Key(i), Value(i, op)));
      model[Key(i)] = Value(i, op);
    }
    if (op % 15000 == 0 && op < kOps) {
      CheckModel(db.get(), model, kKeySpace);
      CHECK_OK(db->Verify());
      db.reset();
      db = Open(dir);
      CheckModel(db.get(), model, kKeySpace);
    }
  }
  DBStats stats = db->GetStats();
  std::printf("    %llu compactions, %llu trivial moves, %llu tombstones dropped; files per level:",
              static_cast<unsigned long long>(stats.compactions),
              static_cast<unsigned long long>(stats.trivial_moves),
              static_cast<unsigned long long>(stats.tombstones_dropped));
  for (uint64_t n : stats.files_per_level) std::printf(" %llu", static_cast<unsigned long long>(n));
  std::printf("\n");
  CHECK(stats.compactions > 0);
  CHECK(stats.files_per_level[0] < static_cast<uint64_t>(Tiny().level0_stop_trigger));

  CHECK_OK(db->CompactAll());
  CheckModel(db.get(), model, kKeySpace);
  CHECK_OK(db->Verify());
}

TEST(CompactAllReclaimsOverwrittenSpace) {
  std::string dir = testing::TempDir();
  auto db = Open(dir);
  constexpr int kKeys = 5000;
  for (int i = 0; i < kKeys; i++) CHECK_OK(db->Put(Key(i), Value(i, 0)));
  CHECK_OK(db->CompactAll());
  uint64_t one_copy = db->GetStats().table_bytes;
  for (int version = 1; version <= 4; version++) {
    for (int i = 0; i < kKeys; i++) CHECK_OK(db->Put(Key(i), Value(i, version)));
  }
  CHECK_OK(db->CompactAll());
  DBStats stats = db->GetStats();
  // Values grew by one character ("4:" vs "0:" is the same length), so one
  // live copy should be about the same size as before.
  CHECK(stats.table_bytes < one_copy * 11 / 10);
  for (int level = 0; level < kNumLevels - 1; level++) CHECK_EQ(stats.files_per_level[level], 0u);
  for (int i = 0; i < kKeys; i++) {
    std::string v;
    CHECK_OK(db->Get(Key(i), &v));
    CHECK_EQ(v, Value(i, 4));
  }
}

TEST(TombstonesDroppedAtBottomLevel) {
  std::string dir = testing::TempDir();
  auto db = Open(dir);
  for (int i = 0; i < 3000; i++) CHECK_OK(db->Put(Key(i), Value(i, 0)));
  for (int i = 0; i < 3000; i++) CHECK_OK(db->Delete(Key(i)));
  CHECK_OK(db->CompactAll());
  DBStats stats = db->GetStats();
  CHECK_EQ(stats.num_tables, 0u);  // Every key was deleted; nothing is left to store.
  CHECK(stats.tombstones_dropped > 0);
  std::string v;
  CHECK(db->Get(Key(0), &v).IsNotFound());
}

// A tombstone must survive compaction while an older value for its key sits
// in a deeper level. Dropping it early would resurrect the deleted value.
TEST(TombstoneKeptWhileOlderValueBelow) {
  std::string dir = testing::TempDir();
  {
    auto db = Open(dir);
    CHECK_OK(db->Put("victim", "old"));
    CHECK_OK(db->CompactAll());  // "victim" now lives in the bottom level.
    CHECK_OK(db->Delete("victim"));
    // Churn other keys so the tombstone gets compacted down through the
    // levels above the bottom.
    for (int i = 0; i < 40000; i++) {
      CHECK_OK(db->Put(Key(i % 8000), Value(i, i)));
      if (i % 5000 == 0) {
        std::string v;
        CHECK(db->Get("victim", &v).IsNotFound());
      }
    }
    CHECK(db->GetStats().compactions > 0);
    std::string v;
    CHECK(db->Get("victim", &v).IsNotFound());
  }
  auto db = Open(dir);
  std::string v;
  CHECK(db->Get("victim", &v).IsNotFound());
  CHECK_OK(db->CompactAll());
  CHECK(db->Get("victim", &v).IsNotFound());
  CHECK_OK(db->Verify());
}

TEST(ManifestIsRewrittenOnOpen) {
  std::string dir = testing::TempDir();
  uint64_t grown;
  {
    auto db = Open(dir);
    for (int i = 0; i < 30000; i++) CHECK_OK(db->Put(Key(i % 3000), Value(i, i)));
    grown = FileSize(ManifestFileName(dir));
  }
  auto db = Open(dir);
  uint64_t rewritten = FileSize(ManifestFileName(dir));
  std::printf("    MANIFEST %llu bytes before reopen, %llu after\n", static_cast<unsigned long long>(grown),
              static_cast<unsigned long long>(rewritten));
  CHECK(rewritten < grown);
  std::string v;
  CHECK_OK(db->Get(Key(2999), &v));
}

// Readers race flushes and compactions and must always see exact values.
// Run under TSan in CI.
TEST(ReadsStayCorrectDuringCompaction) {
  std::string dir = testing::TempDir();
  auto db = Open(dir);
  constexpr int kStable = 500;
  for (int i = 0; i < kStable; i++) CHECK_OK(db->Put("stable" + Key(i), Value(i, 7)));
  std::atomic<bool> done{false};
  std::vector<std::thread> readers;
  for (int t = 0; t < 3; t++) {
    readers.emplace_back([&, t] {
      std::mt19937 rng(t);
      while (!done) {
        int i = static_cast<int>(rng() % kStable);
        std::string v;
        CHECK_OK(db->Get("stable" + Key(i), &v));
        CHECK_EQ(v, Value(i, 7));
      }
    });
  }
  for (int i = 0; i < 60000; i++) CHECK_OK(db->Put(Key(i % 10000), Value(i, i)));
  done = true;
  for (auto& r : readers) r.join();
  CHECK(db->GetStats().compactions > 0);
  CHECK_OK(db->Verify());
}
