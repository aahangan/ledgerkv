// Crash-recovery fault injection.
//
// Each iteration forks a child in which one or more writer threads apply
// deterministic streams of puts, deletes, and two-key batches, reporting
// every acknowledged operation to the parent over a pipe. After a random
// delay the parent SIGKILLs the child mid-stream, reopens the database, and
// checks, per thread, that:
//
//   * every acknowledged operation is visible,
//   * no operation beyond the one in flight at the moment of the kill is
//     visible,
//   * the in-flight operation is either fully applied or absent, and
//   * both keys of a batch are always in the same state.
//
// With several threads, writes are committed in groups sharing one WAL
// append, so kills also land in the middle of group commits. The memtable and
// level sizes are kept small so kills regularly land in the middle of
// background flushes and compactions too. The final iteration also runs
// DB::Verify() to check level invariants and every block checksum.
//
// SIGKILL models a process crash: data passed to write() survives in the page
// cache. Power loss (losing unsynced data and tearing records mid-sector) is
// covered separately by the WAL torn-tail and bit-flip unit tests.

#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "ledgerkv/db.h"
#include "workload.h"

using namespace ledgerkv;
using namespace ledgerkv::workload;

namespace {

struct Ack {
  uint64_t thread;
  uint64_t n;
};

[[noreturn]] void RunChild(const Options& options, const std::string& dir, const std::vector<uint64_t>& start,
                           int ack_fd) {
  std::unique_ptr<DB> db;
  Status s = DB::Open(options, dir, &db);
  if (!s.ok()) {
    std::fprintf(stderr, "child open: %s\n", s.ToString().c_str());
    _exit(2);
  }
  std::vector<std::thread> threads;
  for (int t = 0; t < static_cast<int>(start.size()); t++) {
    threads.emplace_back([&, t] {
      for (uint64_t n = start[t];; n++) {
        Status ws = Apply(db.get(), t, n);
        if (!ws.ok()) {
          std::fprintf(stderr, "child write: %s\n", ws.ToString().c_str());
          _exit(3);
        }
        // Writes of at most PIPE_BUF bytes to a pipe are atomic, so acks
        // from different threads never interleave.
        Ack ack{static_cast<uint64_t>(t), n};
        if (::write(ack_fd, &ack, sizeof(ack)) != sizeof(ack)) _exit(4);
      }
    });
  }
  for (auto& th : threads) th.join();
  _exit(5);  // Unreachable: the writers run until killed.
}

int failures = 0;

void CheckKey(DB* db, int thread, uint64_t k, bool want_present) {
  for (bool mirror : {false, true}) {
    if (mirror && !IsBatch(k)) continue;
    std::string v;
    Status s = db->Get(Key(thread, k, mirror), &v);
    bool ok = want_present ? (s.ok() && v == Value(thread, k)) : s.IsNotFound();
    if (!ok) {
      std::fprintf(stderr, "FAIL: %s is %s, expected %s\n", Key(thread, k, mirror).c_str(),
                   s.ok() ? (v == Value(thread, k) ? "present" : "present with a wrong value") : "absent",
                   want_present ? "present" : "absent");
      if (++failures > 10) std::exit(1);
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  int iterations = 100;
  int num_threads = 1;
  bool sync = false;
  int max_delay_ms = 100;
  size_t write_buffer_kb = 256;
  uint64_t seed = std::random_device{}();
  std::string dir;
  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "--iterations" && i + 1 < argc) iterations = std::atoi(argv[++i]);
    else if (arg == "--threads" && i + 1 < argc) num_threads = std::max(1, std::atoi(argv[++i]));
    else if (arg == "--sync") sync = true;
    else if (arg == "--max-delay-ms" && i + 1 < argc) max_delay_ms = std::atoi(argv[++i]);
    else if (arg == "--seed" && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 10);
    else if (arg == "--dir" && i + 1 < argc) dir = argv[++i];
    else if (arg == "--write-buffer-kb" && i + 1 < argc) write_buffer_kb = std::strtoull(argv[++i], nullptr, 10);
    else {
      std::fprintf(stderr,
                   "usage: crash_test [--iterations N] [--threads T] [--sync] [--max-delay-ms MS] "
                   "[--seed S] [--dir DIR] [--write-buffer-kb KB]\n");
      return 2;
    }
  }
  if (dir.empty()) {
    dir = (std::filesystem::temp_directory_path() / ("ledgerkv-crash-" + std::to_string(::getpid()))).string();
  }
  std::filesystem::remove_all(dir);
  std::printf("crash_test: dir=%s seed=%llu sync=%d threads=%d\n", dir.c_str(),
              static_cast<unsigned long long>(seed), sync, num_threads);

  // Each open table holds a file descriptor, so lift the soft limit.
  rlimit rl;
  if (::getrlimit(RLIMIT_NOFILE, &rl) == 0) {
    rl.rlim_cur = std::min<rlim_t>(rl.rlim_max, 65536);
    ::setrlimit(RLIMIT_NOFILE, &rl);
  }

  Options options;
  options.sync = sync;
  options.write_buffer_size = write_buffer_kb << 10;
  options.max_bytes_for_level_base = options.write_buffer_size * 4;
  options.target_file_size = options.write_buffer_size / 2;

  std::mt19937_64 rng(seed);
  // Per thread, operations [1, next_op) are known to be applied.
  std::vector<uint64_t> next_op(num_threads, 1);
  uint64_t tables = 0;
  int crashes_mid_flush = 0, inflight_applied = 0, inflight_total = 0;

  for (int iter = 0; iter < iterations; iter++) {
    const std::vector<uint64_t> prev_next_op = next_op;
    int fds[2];
    if (::pipe(fds) != 0) return 1;
    pid_t pid = ::fork();
    if (pid < 0) return 1;
    if (pid == 0) {
      ::close(fds[0]);
      RunChild(options, dir, next_op, fds[1]);
    }
    ::close(fds[1]);

    std::vector<uint64_t> last_acked(num_threads);
    for (int t = 0; t < num_threads; t++) last_acked[t] = next_op[t] - 1;
    auto drain = [&](int timeout_ms) {
      pollfd pfd{fds[0], POLLIN, 0};
      if (::poll(&pfd, 1, timeout_ms) <= 0) return false;
      Ack buf[256];
      ssize_t r = ::read(fds[0], buf, sizeof(buf));
      if (r <= 0) return false;
      // Acks are written atomically, so a read returns whole acks only.
      for (ssize_t i = 0; i < r / static_cast<ssize_t>(sizeof(Ack)); i++) {
        last_acked[buf[i].thread] = std::max(last_acked[buf[i].thread], buf[i].n);
      }
      return true;
    };
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(1 + rng() % static_cast<uint64_t>(max_delay_ms));
    while (std::chrono::steady_clock::now() < deadline) {
      auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
      drain(static_cast<int>(std::max<int64_t>(left.count(), 0)));
    }
    ::kill(pid, SIGKILL);
    int status;
    ::waitpid(pid, &status, 0);
    while (drain(0)) {
    }
    ::close(fds[0]);
    if (!WIFSIGNALED(status)) {
      std::fprintf(stderr, "FAIL: child exited on its own with status %d\n", WEXITSTATUS(status));
      return 1;
    }

    // More than one log on disk means the kill landed while a flush was in
    // progress (before its MANIFEST edit committed or its log was deleted).
    int logs = 0;
    for (const auto& e : std::filesystem::directory_iterator(dir)) logs += e.path().extension() == ".log";
    if (logs > 1) crashes_mid_flush++;

    std::unique_ptr<DB> db;
    Status s = DB::Open(options, dir, &db);
    if (!s.ok()) {
      std::fprintf(stderr, "FAIL: reopen: %s\n", s.ToString().c_str());
      return 1;
    }

    const bool full = iter == iterations - 1;
    std::vector<bool> applied(num_threads);
    for (int t = 0; t < num_threads; t++) {
      // The op after last_acked may or may not have been applied when the
      // kill landed. It must be all-or-nothing; then reapply it (puts,
      // deletes, and batches are idempotent) so the model stays exact.
      const uint64_t inflight = last_acked[t] + 1;
      const uint64_t k = TargetKey(inflight);
      const bool before = ExpectPresent(k, last_acked[t]);
      std::string v, mv;
      Status gs = db->Get(Key(t, k), &v);
      const bool present = gs.ok();
      if (present && v != Value(t, k)) {
        std::fprintf(stderr, "FAIL: in-flight key %s has a wrong value\n", Key(t, k).c_str());
        return 1;
      }
      if (IsBatch(k)) {
        bool mirror_present = db->Get(Key(t, k, true), &mv).ok();
        if (mirror_present != present) {
          std::fprintf(stderr, "FAIL: batch %s applied halfway\n", Key(t, k).c_str());
          return 1;
        }
      }
      applied[t] = present != before;
      inflight_total++;
      inflight_applied += applied[t];

      // Checking every key after every crash is quadratic, so check the
      // window this child wrote plus a random sample of older keys, and
      // everything on the final iteration.
      uint64_t window_start = full ? 1 : (prev_next_op[t] > 64 ? prev_next_op[t] - 64 : 1);
      for (uint64_t key = window_start; key < inflight + 8; key++) {
        if (key != k) CheckKey(db.get(), t, key, ExpectPresent(key, last_acked[t]));
      }
      for (int i = 0; !full && i < 2000 / num_threads && inflight > 1; i++) {
        uint64_t key = 1 + rng() % (inflight - 1);
        if (key != k) CheckKey(db.get(), t, key, ExpectPresent(key, last_acked[t]));
      }
      if (failures > 0) return 1;
      if (!Apply(db.get(), t, inflight).ok()) {
        std::fprintf(stderr, "FAIL: reapply\n");
        return 1;
      }
      next_op[t] = inflight + 1;
    }
    tables = db->GetStats().num_tables;
    if (full) {
      Status vs = db->Verify();
      if (!vs.ok()) {
        std::fprintf(stderr, "FAIL: Verify: %s\n", vs.ToString().c_str());
        return 1;
      }
    }
    uint64_t total_ops = 0;
    for (uint64_t n : next_op) total_ops += n - 1;
    std::printf("iter %3d: %9llu ops acked, %4llu tables\n", iter, static_cast<unsigned long long>(total_ops),
                static_cast<unsigned long long>(tables));
  }

  uint64_t total_ops = 0;
  for (uint64_t n : next_op) total_ops += n - 1;
  std::printf("PASS: %d crashes (%d during a flush), %d threads, %llu ops, %llu tables, all acknowledged "
              "writes recovered and no batch torn (%d of %d in-flight ops were durable)\n",
              iterations, crashes_mid_flush, num_threads, static_cast<unsigned long long>(total_ops),
              static_cast<unsigned long long>(tables), inflight_applied, inflight_total);
  std::filesystem::remove_all(dir);
  return 0;
}
