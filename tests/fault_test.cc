#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "fault_env.h"
#include "ledgerkv/db.h"
#include "ledgerkv/env.h"
#include "testing.h"

using namespace ledgerkv;
using Op = FaultInjectionEnv::Op;

namespace {

std::string ReadAll(Env* env, const std::string& path) {
  std::string s;
  CHECK_OK(env->ReadFileToString(path, &s));
  return s;
}

void WriteFile(Env* env, const std::string& path, std::string_view data, bool sync) {
  std::unique_ptr<WritableFile> f;
  CHECK_OK(env->NewWritableFile(path, &f));
  CHECK_OK(f->Append(data));
  if (sync) CHECK_OK(f->Sync());
  CHECK_OK(f->Close());
}

Options SmallOptions(Env* env, bool sync) {
  Options o;
  o.env = env;
  o.sync = sync;
  o.write_buffer_size = 32 << 10;
  o.max_bytes_for_level_base = 128 << 10;
  o.target_file_size = 32 << 10;
  return o;
}

std::string Key(int i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "key%08d", i);
  return buf;
}

}  // namespace

// --- The fault-injecting Env itself. ---

TEST(FaultEnvDropsUnsyncedData) {
  FaultInjectionEnv env;
  std::string dir = testing::TempDir();
  std::string path = dir + "/a";
  std::unique_ptr<WritableFile> f;
  CHECK_OK(env.NewWritableFile(path, &f));
  CHECK_OK(f->Append("durable"));
  CHECK_OK(f->Sync());
  CHECK_OK(env.SyncDir(dir));
  CHECK_OK(f->Append("-lost"));
  CHECK_OK(f->Close());
  CHECK_EQ(ReadAll(&env, path), "durable-lost");
  env.SimulatePowerLoss(/*torn=*/false);
  CHECK_EQ(ReadAll(&env, path), "durable");
}

TEST(FaultEnvForgetsFilesWithoutDirSync) {
  FaultInjectionEnv env;
  std::string dir = testing::TempDir();
  WriteFile(&env, dir + "/synced-data-only", "x", /*sync=*/true);
  env.SimulatePowerLoss(false);
  // The data was synced, but the directory entry never was.
  CHECK(!env.FileExists(dir + "/synced-data-only"));

  WriteFile(&env, dir + "/both", "y", true);
  CHECK_OK(env.SyncDir(dir));
  env.SimulatePowerLoss(false);
  CHECK_EQ(ReadAll(&env, dir + "/both"), "y");
}

TEST(FaultEnvUndoesUnsyncedRenameAndRemove) {
  FaultInjectionEnv env;
  std::string dir = testing::TempDir();
  WriteFile(&env, dir + "/old", "old contents", true);
  WriteFile(&env, dir + "/new", "new contents", true);
  WriteFile(&env, dir + "/doomed", "doomed", true);
  CHECK_OK(env.SyncDir(dir));

  CHECK_OK(env.RenameFile(dir + "/new", dir + "/old"));  // Replaces old.
  CHECK_OK(env.RemoveFile(dir + "/doomed"));
  CHECK_EQ(ReadAll(&env, dir + "/old"), "new contents");
  CHECK(!env.FileExists(dir + "/doomed"));
  std::vector<std::string> names;
  CHECK_OK(env.GetChildren(dir, &names));
  for (const auto& n : names) CHECK(n != ".fault-trash");  // The parking spot is hidden.

  env.SimulatePowerLoss(false);
  CHECK_EQ(ReadAll(&env, dir + "/old"), "old contents");
  CHECK_EQ(ReadAll(&env, dir + "/new"), "new contents");
  CHECK_EQ(ReadAll(&env, dir + "/doomed"), "doomed");

  CHECK_OK(env.RenameFile(dir + "/new", dir + "/old"));
  CHECK_OK(env.SyncDir(dir));  // Now it sticks.
  env.SimulatePowerLoss(false);
  CHECK_EQ(ReadAll(&env, dir + "/old"), "new contents");
  CHECK(!env.FileExists(dir + "/new"));
}

TEST(FaultEnvTornWritesKeepSyncedPrefix) {
  FaultInjectionEnv env(Env::Default(), 99);
  std::string dir = testing::TempDir();
  for (int trial = 0; trial < 50; trial++) {
    std::string path = dir + "/f" + std::to_string(trial);
    std::unique_ptr<WritableFile> f;
    CHECK_OK(env.NewWritableFile(path, &f));
    CHECK_OK(f->Append("SYNCED"));
    CHECK_OK(f->Sync());
    CHECK_OK(f->Append(std::string(100, 'u')));
    CHECK_OK(f->Close());
    CHECK_OK(env.SyncDir(dir));
    env.SimulatePowerLoss(/*torn=*/true);
    std::string s = ReadAll(&env, path);
    CHECK(s.size() >= 6 && s.compare(0, 6, "SYNCED") == 0);
  }
}

TEST(FaultEnvInjectsStickyFaults) {
  FaultInjectionEnv env;
  std::string dir = testing::TempDir();
  env.InjectFault(Op::kSync, "target", 2);
  std::unique_ptr<WritableFile> f, other;
  CHECK_OK(env.NewWritableFile(dir + "/target", &f));
  CHECK_OK(env.NewWritableFile(dir + "/other", &other));
  CHECK_OK(f->Sync());
  CHECK_OK(f->Sync());
  CHECK(f->Sync().IsIOError());
  CHECK(f->Sync().IsIOError());  // Sticky.
  CHECK_OK(other->Sync());       // Only matching paths fail.
  CHECK_EQ(env.FaultsTriggered(), 2u);
  env.ClearFaults();
  CHECK_OK(f->Sync());
}

// --- The database under power loss and I/O errors. ---

// With sync on, every acknowledged write survives a power cut, including
// a cut that lands without a clean shutdown and with torn unsynced tails.
TEST(SyncedWritesSurvivePowerLoss) {
  FaultInjectionEnv env(Env::Default(), 7);
  std::string dir = testing::TempDir();
  int next = 0;
  for (int round = 0; round < 10; round++) {
    std::unique_ptr<DB> db;
    CHECK_OK(DB::Open(SmallOptions(&env, true), dir, &db));
    for (int i = 0; i < 600; i++, next++) CHECK_OK(db->Put(Key(next), std::to_string(next)));
    env.SetActive(false);  // Power cut: nothing after this reaches the disk.
    db.reset();
    env.SimulatePowerLoss(/*torn=*/true);
    env.SetActive(true);
    CHECK_OK(DB::Open(SmallOptions(&env, true), dir, &db));
    for (int i = 0; i < next; i++) {
      std::string v;
      CHECK_OK(db->Get(Key(i), &v));
      CHECK_EQ(v, std::to_string(i));
    }
    CHECK_OK(db->Verify());
  }
}

// A failed WAL sync must be reported, and every later write refused: the
// data may never have reached the disk, and retrying fsync after a failure
// can falsely report success (the "fsyncgate" problem).
TEST(WalSyncFailureIsStickyAndLosesNothingAcknowledged) {
  FaultInjectionEnv env;
  std::string dir = testing::TempDir();
  std::unique_ptr<DB> db;
  CHECK_OK(DB::Open(SmallOptions(&env, true), dir, &db));
  env.InjectFault(Op::kSync, ".log", 100);
  int acked = 0;
  Status s;
  while ((s = db->Put(Key(acked), "v")).ok()) acked++;
  CHECK(s.IsIOError());
  CHECK_EQ(acked, 100);
  env.ClearFaults();
  CHECK(db->Put("after", "v").IsIOError());  // Still refused after the disk "recovers".

  db.reset();
  env.SimulatePowerLoss(true);
  CHECK_OK(DB::Open(SmallOptions(&env, true), dir, &db));
  for (int i = 0; i < acked; i++) {
    std::string v;
    CHECK_OK(db->Get(Key(i), &v));
  }
  CHECK_OK(db->Put("after", "v"));  // A reopened database is writable again.
}

// A short write to the WAL leaves a partial record. Later writes must not
// land after it, or recovery would stop at the garbage and lose them.
TEST(PartialWalAppendDoesNotStrandLaterWrites) {
  FaultInjectionEnv env(Env::Default(), 3);
  std::string dir = testing::TempDir();
  std::unique_ptr<DB> db;
  CHECK_OK(DB::Open(SmallOptions(&env, true), dir, &db));
  for (int i = 0; i < 50; i++) CHECK_OK(db->Put(Key(i), "v"));
  env.InjectFault(Op::kAppend, ".log", 0);
  CHECK(db->Put(Key(50), "v").IsIOError());
  env.ClearFaults();
  CHECK(db->Put(Key(51), "v").IsIOError());
  db.reset();
  env.SimulatePowerLoss(false);
  CHECK_OK(DB::Open(SmallOptions(&env, true), dir, &db));
  for (int i = 0; i < 50; i++) {
    std::string v;
    CHECK_OK(db->Get(Key(i), &v));
  }
  std::string v;
  CHECK(db->Get(Key(51), &v).IsNotFound());
}

// If a background flush cannot sync its table, the error must stop writes
// (not silently drop data), and the WAL must still hold everything.
TEST(FlushFailureStopsWritesAndKeepsData) {
  FaultInjectionEnv env;
  std::string dir = testing::TempDir();
  std::unique_ptr<DB> db;
  CHECK_OK(DB::Open(SmallOptions(&env, true), dir, &db));
  env.InjectFault(Op::kSync, ".sst", 0);
  int acked = 0;
  Status s;
  while (acked < 100000 && (s = db->Put(Key(acked), std::string(100, 'v'))).ok()) acked++;
  CHECK(s.IsIOError());
  CHECK(acked > 0);
  env.ClearFaults();
  db.reset();
  env.SimulatePowerLoss(true);
  CHECK_OK(DB::Open(SmallOptions(&env, true), dir, &db));
  for (int i = 0; i < acked; i++) {
    std::string v;
    CHECK_OK(db->Get(Key(i), &v));
  }
}

// Failing the MANIFEST commit of a flush must not lose the flushed data.
TEST(ManifestFailureKeepsData) {
  FaultInjectionEnv env;
  std::string dir = testing::TempDir();
  std::unique_ptr<DB> db;
  CHECK_OK(DB::Open(SmallOptions(&env, true), dir, &db));
  env.InjectFault(Op::kSync, "MANIFEST", 0);
  int acked = 0;
  Status s;
  while (acked < 100000 && (s = db->Put(Key(acked), std::string(100, 'v'))).ok()) acked++;
  CHECK(s.IsIOError());
  env.ClearFaults();
  db.reset();
  env.SimulatePowerLoss(true);
  CHECK_OK(DB::Open(SmallOptions(&env, true), dir, &db));
  for (int i = 0; i < acked; i++) {
    std::string v;
    CHECK_OK(db->Get(Key(i), &v));
  }
  CHECK_OK(db->Verify());
}

// Read errors must surface as errors, never as a wrong value or a false
// NotFound for a key that exists.
TEST(ReadFailuresNeverReturnWrongData) {
  FaultInjectionEnv env;
  std::string dir = testing::TempDir();
  std::unique_ptr<DB> db;
  CHECK_OK(DB::Open(SmallOptions(&env, false), dir, &db));
  for (int i = 0; i < 5000; i++) CHECK_OK(db->Put(Key(i), std::to_string(i)));
  CHECK_OK(db->CompactAll());
  env.InjectRandomFault(Op::kRead, ".sst", 0.3);
  int errors = 0;
  for (int i = 0; i < 5000; i++) {
    std::string v;
    Status s = db->Get(Key(i), &v);
    if (s.ok()) {
      CHECK_EQ(v, std::to_string(i));
    } else {
      CHECK(s.IsIOError());  // Never NotFound: every key exists.
      errors++;
    }
  }
  auto it = db->NewIterator();
  int seen = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next()) seen++;
  CHECK(it->status().ok() || it->status().IsIOError());
  if (it->status().ok()) CHECK_EQ(seen, 5000);
  std::printf("    %d of 5000 reads failed with IOError, 0 wrong\n", errors);
  CHECK(errors > 0);
}
