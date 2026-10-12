// Power-loss and I/O-error fault injection, in process.
//
// Each iteration opens the database on a FaultInjectionEnv, runs a writer
// thread over the shared deterministic workload (puts, deletes, two-key
// batches), and then cuts the power at a random moment: the Env stops
// persisting anything, the database is torn down, and the Env rewinds the
// disk to what a real one could hold. Unsynced data is lost or torn, files
// whose directory entry was never synced vanish, and unsynced renames and
// deletes are undone. Background flushes and compactions are interrupted at
// arbitrary points along the way.
//
// After reopening, the recovered state must equal the model's state after
// some prefix p of the operations issued, so no holes, no reordering, no torn
// batches, and no wrong values:
//
//   --sync:  p must include every acknowledged write.
//   default: (sync off) acknowledged writes may be lost, but only as a
//            suffix. The recovered database is still exactly some prefix.
//
// With --faults, each iteration also arms one injected I/O error (failed
// fsync, short write, failed rename or directory sync, ...) on the WAL,
// tables, or MANIFEST. Errors must reach the writer. Once the WAL has
// failed, every later write must be refused. And the prefix rule above must
// still hold after the power cut that follows.

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "fault_env.h"
#include "ledgerkv/db.h"
#include "workload.h"

using namespace ledgerkv;
using namespace ledgerkv::workload;
using Op = FaultInjectionEnv::Op;

namespace {

struct FaultChoice {
  Op op;
  const char* path;
  const char* name;
  bool wal;  // Failures here must make every later write fail.
};

const FaultChoice kFaults[] = {
    {Op::kSync, ".log", "WAL fsync", true},
    {Op::kAppend, ".log", "WAL short write", true},
    {Op::kSync, ".sst", "table fsync", false},
    {Op::kAppend, ".sst", "table short write", false},
    {Op::kOpenForWrite, ".sst", "table create", false},
    {Op::kSync, "MANIFEST", "MANIFEST fsync", false},
    {Op::kRename, "MANIFEST", "MANIFEST rename", false},
    {Op::kSyncDir, "", "directory fsync", false},
};

[[noreturn]] void Fail(const std::string& msg) {
  std::fprintf(stderr, "FAIL: %s\n", msg.c_str());
  std::exit(1);
}

// Reads key k (and its mirror, for batch keys) and returns whether it is
// present. Fails on a wrong value or a batch whose two keys disagree.
bool Present(DB* db, uint64_t k) {
  std::string v;
  Status s = db->Get(Key(0, k), &v);
  if (!s.ok() && !s.IsNotFound()) Fail("read " + Key(0, k) + ": " + s.ToString());
  bool present = s.ok();
  if (present && v != Value(0, k)) Fail(Key(0, k) + " has a wrong value");
  if (IsBatch(k)) {
    std::string mv;
    bool mirror = db->Get(Key(0, k, true), &mv).ok();
    if (mirror != present) Fail("batch " + Key(0, k) + " recovered halfway");
    if (mirror && mv != Value(0, k)) Fail(Key(0, k, true) + " has a wrong value");
  }
  return present;
}

}  // namespace

int main(int argc, char** argv) {
  int iterations = 200;
  bool sync = false;
  bool faults = false;
  bool torn = true;
  int max_delay_ms = 60;
  uint64_t seed = std::random_device{}();
  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "--iterations" && i + 1 < argc) iterations = std::atoi(argv[++i]);
    else if (arg == "--sync") sync = true;
    else if (arg == "--faults") faults = true;
    else if (arg == "--no-torn") torn = false;
    else if (arg == "--max-delay-ms" && i + 1 < argc) max_delay_ms = std::atoi(argv[++i]);
    else if (arg == "--seed" && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 10);
    else {
      std::fprintf(stderr,
                   "usage: powerloss_test [--iterations N] [--sync] [--faults] [--no-torn] "
                   "[--max-delay-ms MS] [--seed S]\n");
      return 2;
    }
  }
  std::string dir =
      (std::filesystem::temp_directory_path() / ("ledgerkv-powerloss-" + std::to_string(::getpid()))).string();
  std::filesystem::remove_all(dir);
  std::printf("powerloss_test: seed=%llu sync=%d faults=%d torn=%d\n", static_cast<unsigned long long>(seed), sync,
              faults, torn);

  std::mt19937_64 rng(seed);
  FaultInjectionEnv env(Env::Default(), seed);
  Options options;
  options.env = &env;
  options.sync = sync;
  options.write_buffer_size = 64 << 10;
  options.max_bytes_for_level_base = 256 << 10;
  options.target_file_size = 32 << 10;

  uint64_t next_op = 1;  // The database holds exactly operations [1, next_op).
  uint64_t lost_ops_total = 0, lost_ops_max = 0, ambiguous_applied = 0, ambiguous_total = 0;
  int faults_fired = 0, wal_refusals = 0;
  std::vector<int> fired_by_kind(std::size(kFaults));

  for (int iter = 0; iter < iterations; iter++) {
    const FaultChoice* fault = nullptr;
    size_t fault_index = 0;
    if (faults) {
      fault_index = rng() % std::size(kFaults);
      fault = &kFaults[fault_index];
    }
    const uint64_t faults_before = env.FaultsTriggered();

    std::unique_ptr<DB> db;
    Status s;
    if (fault && fault->op == Op::kRename) {
      // The MANIFEST is renamed into place while opening, so fail that
      // open. It must report the error, and the next open must succeed with
      // nothing lost.
      env.InjectFault(fault->op, fault->path, 0);
      s = DB::Open(options, dir, &db);
      if (s.ok()) Fail("open succeeded despite a failed MANIFEST rename");
      env.ClearFaults();
    }
    s = DB::Open(options, dir, &db);
    if (!s.ok()) Fail("open: " + s.ToString());
    if (fault && fault->op != Op::kRename) {
      env.InjectFault(fault->op, fault->path, static_cast<int>(rng() % 200));
    }

    // The writer counts an operation as acknowledged only if the Env was
    // still powered when the call returned: then its sync, if any, happened
    // before the cut. The one operation that returned after the cut may or
    // may not have made it.
    std::atomic<bool> stop{false};
    uint64_t last_acked = next_op - 1, last_issued = next_op - 1;
    bool wal_failed = false;
    std::thread writer([&] {
      for (uint64_t n = next_op; !stop; n++) {
        Status ws = Apply(db.get(), 0, n);
        last_issued = n;
        if (!ws.ok()) {
          if (fault && fault->wal) {
            wal_failed = true;
            // fsync failures cannot be retried safely, and a short write
            // leaves a partial record that later writes must not follow. So
            // even once the disk "recovers", the database must keep
            // refusing writes. (Clear the fault first: otherwise the next
            // write would fail on the fault itself and prove nothing.)
            env.ClearFaults();
            if (Apply(db.get(), 0, n + 1).ok()) Fail("write accepted after a WAL failure");
          }
          return;
        }
        if (!env.IsActive()) return;
        last_acked = n;
      }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(rng() % static_cast<uint64_t>(max_delay_ms + 1)));
    env.SetActive(false);  // Power cut.
    stop = true;
    writer.join();
    db.reset();
    env.SimulatePowerLoss(torn);
    env.ClearFaults();
    env.SetActive(true);
    if (env.FaultsTriggered() > faults_before) {
      faults_fired++;
      fired_by_kind[fault_index]++;
    }
    wal_refusals += wal_failed;

    s = DB::Open(options, dir, &db);
    if (!s.ok()) Fail("reopen after power loss: " + s.ToString());

    // Find the largest prefix p the recovered state matches. Only keys in
    // [next_op - 8, last_issued] can differ between candidate prefixes; D
    // counts the keys in that window that disagree with model(p), updated
    // in O(1) per step because operation p changes exactly one key.
    const uint64_t lo = next_op > 8 ? next_op - 8 : 1;
    std::vector<bool> present(last_issued + 1 - lo + 1);
    auto r = [&](uint64_t k) { return static_cast<bool>(present[k - lo]); };
    int64_t d = 0;
    for (uint64_t k = lo; k <= last_issued; k++) {
      present[k - lo] = Present(db.get(), k);
      d += r(k) != ExpectPresent(k, next_op - 1);
    }
    const uint64_t min_p = sync ? last_acked : next_op - 1;
    int64_t best = (d == 0 && next_op - 1 >= min_p) ? static_cast<int64_t>(next_op - 1) : -1;
    for (uint64_t p = next_op; p <= last_issued; p++) {
      uint64_t k = TargetKey(p);
      bool before = ExpectPresent(k, p - 1), after = ExpectPresent(k, p);
      if (before != after) d += static_cast<int64_t>(r(k) != after) - static_cast<int64_t>(r(k) != before);
      if (d == 0 && p >= min_p) best = static_cast<int64_t>(p);
    }
    if (best < 0) {
      Fail("iteration " + std::to_string(iter) + ": recovered state is not a prefix of the writes (" +
           (sync ? "every acknowledged write must survive; acked " : "issued ") +
           std::to_string(sync ? last_acked : last_issued) + ")");
    }
    const uint64_t p = static_cast<uint64_t>(best);

    // Keys below the window should be untouched. Sample them, and check
    // everything plus structural invariants periodically.
    const bool full = iter == iterations - 1 || iter % 25 == 24;
    for (int i = 0; lo > 1 && i < (full ? 0 : 300); i++) {
      uint64_t k = 1 + rng() % (lo - 1);
      if (Present(db.get(), k) != ExpectPresent(k, p)) Fail("older key " + Key(0, k) + " changed");
    }
    if (full) {
      for (uint64_t k = 1; k < lo; k++) {
        if (Present(db.get(), k) != ExpectPresent(k, p)) Fail("older key " + Key(0, k) + " changed");
      }
      Status vs = db->Verify();
      if (!vs.ok()) Fail("Verify: " + vs.ToString());
    }

    if (sync && last_issued > last_acked) {
      ambiguous_total++;
      ambiguous_applied += p > last_acked;
    }
    uint64_t lost = last_issued - p;
    if (!sync) {
      lost_ops_total += lost;
      lost_ops_max = std::max(lost_ops_max, lost);
    }
    next_op = p + 1;
    db.reset();
    std::printf("iter %3d: %8llu ops recovered%s%s\n", iter, static_cast<unsigned long long>(p),
                sync ? "" : (", lost unsynced tail of " + std::to_string(lost)).c_str(),
                fault && env.FaultsTriggered() > faults_before ? (std::string(", ") + fault->name + " failed").c_str()
                                                               : "");
  }

  std::printf("PASS: %d power cuts, %llu ops, every recovery an exact prefix of the writes", iterations,
              static_cast<unsigned long long>(next_op - 1));
  if (sync) {
    std::printf(", no acknowledged write lost (%llu of %llu in-flight writes survived)",
                static_cast<unsigned long long>(ambiguous_applied), static_cast<unsigned long long>(ambiguous_total));
  } else {
    std::printf(", unsynced tail lost: avg %.0f ops, max %llu", static_cast<double>(lost_ops_total) / iterations,
                static_cast<unsigned long long>(lost_ops_max));
  }
  if (faults) {
    std::printf("; %d injected I/O errors fired (%d on the WAL, all later writes refused)", faults_fired,
                wal_refusals);
  }
  std::printf("\n");
  if (faults) {
    for (size_t i = 0; i < std::size(kFaults); i++) std::printf("  %-18s fired %d times\n", kFaults[i].name, fired_by_kind[i]);
  }
  std::filesystem::remove_all(dir);
  return 0;
}
