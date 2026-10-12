// Throughput and latency benchmark.
//
//   kvbench [--num N] [--value-size BYTES] [--sync] [--write-buffer-mb MB] [--dir DIR]
//
// Phases, each reporting ops/sec and per-operation latency percentiles:
//   fillseq      sequential inserts into an empty database
//   fillrandom   random inserts into an empty database
//   overwrite    random updates of existing keys
//   reopen       close and reopen (recovery time)
//   readrandom   random reads of existing keys, served from tables on disk
//   readmissing  random reads of absent keys (exercises the bloom filters)

#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "ledgerkv/db.h"

using namespace ledgerkv;
using Clock = std::chrono::steady_clock;

namespace {

std::string Key(uint64_t n) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%016llu", static_cast<unsigned long long>(n));
  return buf;
}

void Report(const char* name, std::vector<double>& lat_us, double total_s) {
  std::sort(lat_us.begin(), lat_us.end());
  auto pct = [&](double p) { return lat_us[std::min(lat_us.size() - 1, static_cast<size_t>(p * lat_us.size()))]; };
  std::printf("%-12s %10.0f ops/s   p50 %8.2f us   p99 %8.2f us   p99.9 %9.2f us   max %10.2f us\n", name,
              lat_us.size() / total_s, pct(0.50), pct(0.99), pct(0.999), lat_us.back());
}

void Run(const char* name, uint64_t num, const std::function<void(uint64_t)>& op) {
  std::vector<double> lat_us;
  lat_us.reserve(num);
  auto start = Clock::now();
  for (uint64_t i = 0; i < num; i++) {
    auto t0 = Clock::now();
    op(i);
    lat_us.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
  }
  Report(name, lat_us, std::chrono::duration<double>(Clock::now() - start).count());
}

void Check(const Status& s) {
  if (!s.ok() && !s.IsNotFound()) {
    std::fprintf(stderr, "error: %s\n", s.ToString().c_str());
    std::exit(1);
  }
}

void PrintStats(DB* db) {
  DBStats st = db->GetStats();
  std::printf("             tables %llu (%.1f MB), flushes %llu, writers stalled %.1f ms total\n",
              static_cast<unsigned long long>(st.num_tables), st.table_bytes / 1048576.0,
              static_cast<unsigned long long>(st.flushes), st.stall_micros / 1000.0);
}

}  // namespace

int main(int argc, char** argv) {
  uint64_t num = 1000000;
  size_t value_size = 100;
  size_t write_buffer_mb = 4;
  bool sync = false;
  std::string dir;
  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "--num" && i + 1 < argc) num = std::strtoull(argv[++i], nullptr, 10);
    else if (arg == "--value-size" && i + 1 < argc) value_size = std::strtoull(argv[++i], nullptr, 10);
    else if (arg == "--write-buffer-mb" && i + 1 < argc) write_buffer_mb = std::strtoull(argv[++i], nullptr, 10);
    else if (arg == "--sync") sync = true;
    else if (arg == "--dir" && i + 1 < argc) dir = argv[++i];
    else {
      std::fprintf(stderr,
                   "usage: kvbench [--num N] [--value-size BYTES] [--sync] [--write-buffer-mb MB] [--dir DIR]\n");
      return 2;
    }
  }
  if (dir.empty()) {
    dir = (std::filesystem::temp_directory_path() / ("ledgerkv-bench-" + std::to_string(::getpid()))).string();
  }
  // Each table holds a file descriptor until compaction exists (milestone 3).
  rlimit rl;
  if (::getrlimit(RLIMIT_NOFILE, &rl) == 0) {
    rl.rlim_cur = std::min<rlim_t>(rl.rlim_max, 65536);
    ::setrlimit(RLIMIT_NOFILE, &rl);
  }

  std::printf("ledgerkv kvbench: num=%llu value_size=%zu sync=%d write_buffer=%zuMB\n",
              static_cast<unsigned long long>(num), value_size, sync, write_buffer_mb);
  std::mt19937_64 rng(42);
  std::string value(value_size, 'x');
  for (auto& c : value) c = static_cast<char>('a' + rng() % 26);

  Options options;
  options.sync = sync;
  options.write_buffer_size = write_buffer_mb << 20;
  auto open = [&] {
    std::unique_ptr<DB> db;
    Check(DB::Open(options, dir, &db));
    return db;
  };

  std::filesystem::remove_all(dir);
  {
    auto db = open();
    Run("fillseq", num, [&](uint64_t i) { Check(db->Put(Key(i), value)); });
  }
  std::filesystem::remove_all(dir);
  auto db = open();
  Run("fillrandom", num, [&](uint64_t) { Check(db->Put(Key(rng() % num), value)); });
  Run("overwrite", num, [&](uint64_t) { Check(db->Put(Key(rng() % num), value)); });
  PrintStats(db.get());

  db.reset();
  auto t0 = Clock::now();
  db = open();
  std::printf("%-12s %10.1f ms   (replays at most one memtable of log, then flushes it)\n", "reopen",
              std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
  PrintStats(db.get());

  std::string out;
  Run("readrandom", num, [&](uint64_t) { Check(db->Get(Key(rng() % num), &out)); });
  Run("readmissing", num, [&](uint64_t) { Check(db->Get(Key(num + rng() % num), &out)); });
  db.reset();
  std::filesystem::remove_all(dir);
  return 0;
}
