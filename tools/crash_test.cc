// Crash-recovery fault injection.
//
// Each iteration forks a child that applies a deterministic stream of puts and
// deletes to the database, reporting every acknowledged operation to the
// parent over a pipe. After a random delay the parent SIGKILLs the child
// mid-stream, reopens the database, and checks that:
//
//   * every acknowledged operation is visible,
//   * no operation beyond the one in flight at the moment of the kill is
//     visible, and
//   * the in-flight operation is either fully applied or absent, never torn.
//
// The memtable and level sizes are kept small so that kills regularly land in
// the middle of background work: a flush or compaction with tables half
// written, before or after its MANIFEST edit, or while obsolete files are
// being deleted. The final iteration also runs DB::Verify() to check level
// invariants and every block checksum.
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

#include "ledgerkv/db.h"

using namespace ledgerkv;

namespace {

uint64_t Mix(uint64_t x) {  // splitmix64
  x += 0x9E3779B97F4A7C15ULL;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
  return x ^ (x >> 31);
}

// Operation n is a delete of key (n - 5) when n % 10 == 9, else a put of key n.
bool IsDelete(uint64_t n) { return n % 10 == 9; }
uint64_t TargetKey(uint64_t n) { return IsDelete(n) ? n - 5 : n; }

std::string Key(uint64_t n) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "key%012llu", static_cast<unsigned long long>(n));
  return buf;
}

std::string Value(uint64_t n) {
  uint64_t h = Mix(n);
  std::string v(1 + h % 128, '\0');
  for (size_t i = 0; i < v.size(); i++) v[i] = static_cast<char>('a' + (Mix(h + i) % 26));
  return v;
}

Status Apply(DB* db, uint64_t n) {
  return IsDelete(n) ? db->Delete(Key(TargetKey(n))) : db->Put(Key(n), Value(n));
}

[[noreturn]] void Fail(const char* fmt, uint64_t a = 0, uint64_t b = 0) {
  std::fprintf(stderr, "FAIL: ");
  std::fprintf(stderr, fmt, static_cast<unsigned long long>(a), static_cast<unsigned long long>(b));
  std::fprintf(stderr, "\n");
  std::exit(1);
}

[[noreturn]] void RunChild(const Options& options, const std::string& dir, uint64_t start, int ack_fd) {
  std::unique_ptr<DB> db;
  Status s = DB::Open(options, dir, &db);
  if (!s.ok()) {
    std::fprintf(stderr, "child open: %s\n", s.ToString().c_str());
    _exit(2);
  }
  for (uint64_t n = start;; n++) {
    s = Apply(db.get(), n);
    if (!s.ok()) {
      std::fprintf(stderr, "child write: %s\n", s.ToString().c_str());
      _exit(3);
    }
    if (::write(ack_fd, &n, sizeof(n)) != sizeof(n)) _exit(4);
  }
}

// Expected state of key k after operations [1, last] have been applied.
bool ExpectPresent(uint64_t k, uint64_t last) {
  if (k > last || IsDelete(k)) return false;
  return !(k % 10 == 4 && k + 5 <= last);
}

void CheckKey(DB* db, uint64_t k, bool want_present) {
  std::string v;
  Status s = db->Get(Key(k), &v);
  if (want_present) {
    if (!s.ok()) Fail("key %llu missing: an acknowledged write was lost", k);
    if (v != Value(k)) Fail("key %llu has wrong value", k);
  } else if (!s.IsNotFound()) {
    Fail("key %llu present but should be absent", k);
  }
}

}  // namespace

int main(int argc, char** argv) {
  int iterations = 100;
  bool sync = false;
  int max_delay_ms = 100;
  size_t write_buffer_kb = 256;
  uint64_t seed = std::random_device{}();
  std::string dir;
  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "--iterations" && i + 1 < argc) iterations = std::atoi(argv[++i]);
    else if (arg == "--sync") sync = true;
    else if (arg == "--max-delay-ms" && i + 1 < argc) max_delay_ms = std::atoi(argv[++i]);
    else if (arg == "--seed" && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 10);
    else if (arg == "--dir" && i + 1 < argc) dir = argv[++i];
    else if (arg == "--write-buffer-kb" && i + 1 < argc) write_buffer_kb = std::strtoull(argv[++i], nullptr, 10);
    else {
      std::fprintf(stderr,
                   "usage: crash_test [--iterations N] [--sync] [--max-delay-ms MS] "
                   "[--seed S] [--dir DIR] [--write-buffer-kb KB]\n");
      return 2;
    }
  }
  if (dir.empty()) {
    dir = (std::filesystem::temp_directory_path() / ("ledgerkv-crash-" + std::to_string(::getpid()))).string();
  }
  std::filesystem::remove_all(dir);
  std::printf("crash_test: dir=%s seed=%llu sync=%d\n", dir.c_str(),
              static_cast<unsigned long long>(seed), sync);

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
  uint64_t tables = 0;
  int crashes_mid_flush = 0;
  uint64_t next_op = 1;  // Operations [1, next_op) are known to be applied.
  int inflight_applied = 0;

  for (int iter = 0; iter < iterations; iter++) {
    const uint64_t prev_next_op = next_op;
    int fds[2];
    if (::pipe(fds) != 0) Fail("pipe");
    pid_t pid = ::fork();
    if (pid < 0) Fail("fork");
    if (pid == 0) {
      ::close(fds[0]);
      RunChild(options, dir, next_op, fds[1]);
    }
    ::close(fds[1]);

    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(1 + rng() % static_cast<uint64_t>(max_delay_ms));
    uint64_t last_acked = next_op - 1;
    auto drain = [&](int timeout_ms) {
      pollfd pfd{fds[0], POLLIN, 0};
      if (::poll(&pfd, 1, timeout_ms) <= 0) return false;
      uint64_t buf[512];
      ssize_t r = ::read(fds[0], buf, sizeof(buf));
      if (r <= 0) return false;
      // Acks are 8-byte writes to a pipe, which are atomic, so r is a multiple of 8.
      last_acked = buf[r / sizeof(uint64_t) - 1];
      return true;
    };
    while (std::chrono::steady_clock::now() < deadline) {
      auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now());
      drain(static_cast<int>(std::max<int64_t>(left.count(), 0)));
    }
    ::kill(pid, SIGKILL);
    int status;
    ::waitpid(pid, &status, 0);
    while (drain(0)) {
    }
    ::close(fds[0]);
    if (!WIFSIGNALED(status)) Fail("child exited on its own with status %llu", WEXITSTATUS(status));

    // More than one log on disk means the kill landed while a flush was in
    // progress (before its MANIFEST edit committed or its log was deleted).
    int logs = 0;
    if (iter > 0 || next_op > 1) {
      for (const auto& e : std::filesystem::directory_iterator(dir)) logs += e.path().extension() == ".log";
    }
    if (logs > 1) crashes_mid_flush++;

    std::unique_ptr<DB> db;
    Status s = DB::Open(options, dir, &db);
    if (!s.ok()) {
      std::fprintf(stderr, "FAIL: reopen: %s\n", s.ToString().c_str());
      return 1;
    }

    // The op after last_acked may or may not have been applied when the kill
    // landed. Check that it is all-or-nothing, then reapply it (puts and
    // deletes are idempotent) so the model stays exact for later iterations.
    uint64_t inflight = last_acked + 1;
    uint64_t k = TargetKey(inflight);
    bool before = ExpectPresent(k, last_acked);
    bool after = ExpectPresent(k, inflight);
    std::string v;
    Status gs = db->Get(Key(k), &v);
    bool present = gs.ok();
    if (present && v != Value(k)) Fail("in-flight key %llu is torn", k);
    if (present != before && present != after) Fail("in-flight key %llu in impossible state", k);
    if (present != before) inflight_applied++;

    // Checking every key after every crash is quadratic, so check the window
    // this child wrote plus a random sample of older keys, and everything on
    // the final iteration.
    bool full = iter == iterations - 1;
    uint64_t window_start = full ? 1 : std::max<uint64_t>(1, prev_next_op > 64 ? prev_next_op - 64 : 1);
    for (uint64_t key = window_start; key < inflight + 8; key++) {
      if (key != k) CheckKey(db.get(), key, ExpectPresent(key, last_acked));
    }
    for (int i = 0; !full && i < 2000 && inflight > 1; i++) {
      uint64_t key = 1 + rng() % (inflight - 1);
      if (key != k) CheckKey(db.get(), key, ExpectPresent(key, last_acked));
    }
    tables = db->GetStats().num_tables;
    if (full) {
      Status vs = db->Verify();
      if (!vs.ok()) {
        std::fprintf(stderr, "FAIL: Verify: %s\n", vs.ToString().c_str());
        return 1;
      }
    }

    if (!Apply(db.get(), inflight).ok()) Fail("reapply failed");
    next_op = inflight + 1;
    std::printf("iter %3d: %8llu ops acked, %4llu tables, in-flight op %s\n", iter,
                static_cast<unsigned long long>(last_acked), static_cast<unsigned long long>(tables),
                present != before ? "applied" : "not applied");
  }

  std::printf("PASS: %d crashes (%d during a flush), %llu ops, %llu tables, all acknowledged "
              "writes recovered (%d crashes landed after the in-flight write was durable)\n",
              iterations, crashes_mid_flush, static_cast<unsigned long long>(next_op - 1),
              static_cast<unsigned long long>(tables), inflight_applied);
  std::filesystem::remove_all(dir);
  return 0;
}
