#include <algorithm>
#include <atomic>
#include <cstdio>
#include <map>
#include <random>
#include <thread>
#include <vector>

#include "arena.h"
#include "ledgerkv/db.h"
#include "memtable.h"
#include "skiplist.h"
#include "testing.h"
#include "wal.h"

using namespace ledgerkv;

namespace {

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

std::string Get(DB* db, std::string_view key, const Snapshot* snap = nullptr) {
  ReadOptions ro;
  ro.snapshot = snap;
  std::string v;
  Status s = db->Get(ro, key, &v);
  if (s.IsNotFound()) return "<missing>";
  CHECK_OK(s);
  return v;
}

std::map<std::string, std::string> Scan(DB* db, const Snapshot* snap = nullptr) {
  ReadOptions ro;
  ro.snapshot = snap;
  auto it = db->NewIterator(ro);
  std::map<std::string, std::string> out;
  std::string prev;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    CHECK(out.empty() || std::string(it->key()) > prev);
    prev.assign(it->key());
    out.emplace(it->key(), it->value());
  }
  CHECK_OK(it->status());
  return out;
}

struct U64Cmp {
  int operator()(uint64_t a, uint64_t b) const { return a < b ? -1 : a > b ? 1 : 0; }
};

}  // namespace

// One writer inserts while readers iterate without locks. Readers must always
// see a sorted list, and every key they see must be one that was inserted.
// Run under TSan in CI.
TEST(SkipListConcurrentReaders) {
  Arena arena;
  SkipList<uint64_t, U64Cmp> list(U64Cmp(), &arena);
  constexpr uint64_t kInserts = 50000;
  std::atomic<bool> done{false};
  std::vector<std::thread> readers;
  for (int t = 0; t < 3; t++) {
    readers.emplace_back([&] {
      while (!done) {
        SkipList<uint64_t, U64Cmp>::Iterator it(&list);
        uint64_t prev = 0;
        for (it.SeekToFirst(); it.Valid(); it.Next()) {
          CHECK(it.key() > prev);  // Strictly increasing; keys start at 1.
          CHECK(it.key() % 7 == 0);
          prev = it.key();
        }
      }
    });
  }
  std::mt19937_64 rng(1);
  std::vector<uint64_t> keys;
  for (uint64_t i = 1; i <= kInserts; i++) keys.push_back(i * 7);
  std::shuffle(keys.begin(), keys.end(), rng);
  for (uint64_t k : keys) list.Insert(k);
  done = true;
  for (auto& r : readers) r.join();

  SkipList<uint64_t, U64Cmp>::Iterator it(&list);
  uint64_t n = 0;
  for (it.SeekToFirst(); it.Valid(); it.Next()) CHECK_EQ(it.key(), ++n * 7);
  CHECK_EQ(n, kInserts);
  it.Seek(700);
  CHECK(it.Valid() && it.key() == 700);
  it.Seek(701);
  CHECK(it.Valid() && it.key() == 707);
}

TEST(MemTableKeepsVersions) {
  MemTable mem;
  mem.Add(1, RecordType::kPut, "a", "v1");
  mem.Add(2, RecordType::kPut, "a", "v2");
  mem.Add(3, RecordType::kDelete, "a", "");
  mem.Add(4, RecordType::kPut, "b", "b4");
  std::string v;
  CHECK(mem.Get("a", 0, &v) == LookupResult::kMissing);
  CHECK(mem.Get("a", 1, &v) == LookupResult::kFound && v == "v1");
  CHECK(mem.Get("a", 2, &v) == LookupResult::kFound && v == "v2");
  CHECK(mem.Get("a", 3, &v) == LookupResult::kDeleted);
  CHECK(mem.Get("a", kMaxSequence, &v) == LookupResult::kDeleted);
  CHECK(mem.Get("b", 3, &v) == LookupResult::kMissing);
  CHECK(mem.Get("ab", kMaxSequence, &v) == LookupResult::kMissing);

  auto it = mem.NewIterator();
  std::vector<uint64_t> seqs;
  for (it->SeekToFirst(); it->Valid(); it->Next()) seqs.push_back(it->seq());
  CHECK((seqs == std::vector<uint64_t>{3, 2, 1, 4}));  // Newest first within a key.
}

TEST(WalBatchRecordRoundTrip) {
  std::string path = testing::TempDir() + "/000001.log";
  WriteBatch b;
  b.Put("x", "1");
  b.Delete("y");
  b.Put("z", "3");
  std::string payload, framed;
  {
    std::unique_ptr<WalWriter> w;
    CHECK_OK(WalWriter::Open(path, false, &w));
    // Mirror what the write path does: one framed record per batch. The
    // contents match what WriteBatch b above encodes.
    std::string contents;
    {
      contents.append("\x03\x00\x00\x00", 4);
      contents.append("\x01\x01x\x01" "1", 5);
      contents.append("\x02\x01y", 3);
      contents.append("\x01\x01z\x01" "3", 5);
    }
    EncodeWalBatchPayload(100, contents, &payload);
    AppendLogRecord(payload, &framed);
    CHECK_OK(w->AppendFramed(framed));
  }
  WalReadResult r;
  CHECK_OK(ReadWal(path, &r));
  CHECK_EQ(r.records.size(), 3u);
  CHECK(r.records[0].key == "x" && r.records[0].seq == 100 && r.records[0].value == "1");
  CHECK(r.records[1].key == "y" && r.records[1].seq == 101 && r.records[1].type == RecordType::kDelete);
  CHECK(r.records[2].key == "z" && r.records[2].seq == 102);
  CHECK_EQ(b.Count(), 3u);
}

TEST(WriteBatchAppliesInOrder) {
  std::string dir = testing::TempDir();
  {
    auto db = Open(dir);
    WriteBatch b;
    b.Put("a", "1");
    b.Put("b", "2");
    b.Delete("a");
    b.Put("c", "3");
    b.Put("c", "4");  // Later operations on a key win.
    CHECK_OK(db->Write(b));
    CHECK_EQ(Get(db.get(), "a"), "<missing>");
    CHECK_EQ(Get(db.get(), "b"), "2");
    CHECK_EQ(Get(db.get(), "c"), "4");
  }
  auto db = Open(dir);  // Recovered from the WAL batch record.
  CHECK_EQ(Get(db.get(), "a"), "<missing>");
  CHECK_EQ(Get(db.get(), "c"), "4");
  CHECK_OK(db->Write(WriteBatch()));  // Empty batches are a no-op.
}

// A writer keeps setting two keys to the same value in one batch. A reader
// with a snapshot must never see them disagree.
TEST(BatchesAreVisibleAtomically) {
  std::string dir = testing::TempDir();
  auto db = Open(dir);
  CHECK_OK(db->Put("left", "0"));
  CHECK_OK(db->Put("right", "0"));
  std::atomic<bool> done{false};
  std::thread writer([&] {
    for (int i = 1; i <= 20000; i++) {
      WriteBatch b;
      b.Put("left", std::to_string(i));
      b.Put("right", std::to_string(i));
      CHECK_OK(db->Write(b));
    }
    done = true;
  });
  int checks = 0;
  while (!done) {
    const Snapshot* snap = db->GetSnapshot();
    CHECK_EQ(Get(db.get(), "left", snap), Get(db.get(), "right", snap));
    db->ReleaseSnapshot(snap);
    auto view = Scan(db.get());
    CHECK_EQ(view["left"], view["right"]);
    checks++;
  }
  writer.join();
  std::printf("    %d consistent reads during 20000 batches\n", checks);
}

TEST(SnapshotSurvivesFlushAndCompaction) {
  std::string dir = testing::TempDir();
  auto db = Open(dir);
  for (int i = 0; i < 2000; i++) CHECK_OK(db->Put(Key(i), "old"));
  const Snapshot* snap = db->GetSnapshot();
  for (int i = 0; i < 2000; i++) {
    if (i % 2) {
      CHECK_OK(db->Delete(Key(i)));
    } else {
      CHECK_OK(db->Put(Key(i), "new"));
    }
  }
  CHECK_OK(db->Put("added-later", "x"));
  auto check = [&] {
    for (int i = 0; i < 2000; i += 37) {
      CHECK_EQ(Get(db.get(), Key(i), snap), "old");
      CHECK_EQ(Get(db.get(), Key(i)), i % 2 ? "<missing>" : "new");
    }
    CHECK_EQ(Get(db.get(), "added-later", snap), "<missing>");
    CHECK_EQ(Scan(db.get(), snap).size(), 2000u);
    CHECK_EQ(Scan(db.get()).size(), 1001u);
  };
  check();
  CHECK_OK(db->CompactAll());  // Must keep the versions the snapshot needs.
  check();
  CHECK_OK(db->Verify());
  uint64_t bytes_with_snapshot = db->GetStats().table_bytes;

  db->ReleaseSnapshot(snap);
  CHECK_OK(db->CompactAll());  // Now the old versions are garbage.
  DBStats stats = db->GetStats();
  CHECK(stats.versions_dropped > 0);
  CHECK(stats.table_bytes < bytes_with_snapshot);
  CHECK_EQ(Get(db.get(), Key(0)), "new");
  CHECK_EQ(Get(db.get(), Key(1)), "<missing>");
}

// Ordered scans across memtable, level 0, and deeper levels must match a
// std::map model, including Seek to present and absent keys.
TEST(IteratorMatchesModel) {
  std::string dir = testing::TempDir();
  auto db = Open(dir);
  std::map<std::string, std::string> model;
  std::mt19937 rng(7);
  for (int op = 0; op < 40000; op++) {
    int i = static_cast<int>(rng() % 3000);
    if (rng() % 5 == 0) {
      CHECK_OK(db->Delete(Key(i)));
      model.erase(Key(i));
    } else {
      std::string v = std::to_string(op);
      CHECK_OK(db->Put(Key(i), v));
      model[Key(i)] = v;
    }
  }
  CHECK(db->GetStats().num_tables > 0);
  CHECK((Scan(db.get()) == model));

  auto it = db->NewIterator();
  for (int probe = 0; probe < 500; probe++) {
    std::string target = Key(static_cast<int>(rng() % 3100)) + (probe % 2 ? "" : "~");
    it->Seek(target);
    auto want = model.lower_bound(target);
    if (want == model.end()) {
      CHECK(!it->Valid());
    } else {
      CHECK(it->Valid());
      CHECK_EQ(std::string(it->key()), want->first);
      CHECK_EQ(std::string(it->value()), want->second);
      it->Next();
      ++want;
      CHECK_EQ(it->Valid(), want != model.end());
      if (it->Valid()) CHECK_EQ(std::string(it->key()), want->first);
    }
  }
  CHECK_OK(it->status());
}

// An iterator is a fixed view: writes, flushes, and compactions after it is
// created must not change what it returns.
TEST(IteratorIsStableUnderWritesAndCompaction) {
  std::string dir = testing::TempDir();
  auto db = Open(dir);
  for (int i = 0; i < 3000; i++) CHECK_OK(db->Put(Key(i), "before"));
  auto expected = Scan(db.get());
  auto it = db->NewIterator();
  it->SeekToFirst();
  for (int i = 0; i < 3000; i += 2) CHECK_OK(db->Delete(Key(i)));
  for (int i = 3000; i < 6000; i++) CHECK_OK(db->Put(Key(i), "after"));
  CHECK_OK(db->CompactAll());

  std::map<std::string, std::string> seen;
  for (; it->Valid(); it->Next()) seen.emplace(it->key(), it->value());
  CHECK_OK(it->status());
  CHECK((seen == expected));
  CHECK_EQ(Scan(db.get()).size(), 4500u);
}

// Concurrent writers share WAL appends. Every write must land, and with
// sync on, writers that queue behind an fsync should be grouped.
TEST(GroupCommitConcurrentWriters) {
  std::string dir = testing::TempDir();
  Options o = Tiny();
  o.sync = true;
  o.write_buffer_size = 4 << 20;
  auto db = Open(dir, o);
  constexpr int kThreads = 16, kPerThread = 25;
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; t++) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < kPerThread; i++) CHECK_OK(db->Put(Key(t * 1000 + i), std::to_string(t)));
    });
  }
  for (auto& th : threads) th.join();
  DBStats stats = db->GetStats();
  std::printf("    %llu writes in %llu WAL appends (%.1f per fsync)\n",
              static_cast<unsigned long long>(stats.writes), static_cast<unsigned long long>(stats.write_groups),
              static_cast<double>(stats.writes) / stats.write_groups);
  CHECK_EQ(stats.writes, static_cast<uint64_t>(kThreads * kPerThread));
  CHECK(stats.write_groups < stats.writes);
  db.reset();
  db = Open(dir, o);
  for (int t = 0; t < kThreads; t++) {
    for (int i = 0; i < kPerThread; i++) CHECK_EQ(Get(db.get(), Key(t * 1000 + i)), std::to_string(t));
  }
}
