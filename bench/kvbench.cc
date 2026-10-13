// Throughput and latency benchmark. Every performance figure in the README
// comes from this program; `make bench` runs it and writes a report.
//
//   kvbench [--num N] [--value-size BYTES] [--write-buffer-mb MB] [--sync-ops N]
//           [--no-sync-writes] [--report PATH] [--dir DIR]
//
// Phases, each reporting ops/sec and per-operation latency percentiles:
//   fillseq      sequential inserts into an empty database
//   fillrandom   random inserts into an empty database
//   overwrite    random updates of existing keys
//   reopen       close and reopen (recovery time)
//   readrandom   random reads of written keys, served from tables on disk
//   readmissing  random reads of absent keys that fall between stored keys, so
//                key-range checks cannot skip tables and the bloom filters must
//   readrandom8  the same reads spread across 8 threads
//   scan         full iterator scan
//   seekrandom   seek to a random key, then read the next 10
//   sync xT      fsynced writes from T concurrent threads (group commit)
//
// With --report, appends a Markdown section with every result plus the
// headline figures the README and resume quote.

#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "ledgerkv/db.h"

using namespace ledgerkv;
using Clock = std::chrono::steady_clock;

namespace {

struct Result {
  std::string name;
  double ops_per_sec;
  double p50_us, p99_us, p999_us;
};

std::vector<Result> results;

std::string Key(uint64_t n) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%016llu", static_cast<unsigned long long>(n));
  return buf;
}

// 1234567 -> "1.23M", 84433 -> "84.4K".
std::string Count(double v) {
  char buf[32];
  if (v >= 1e6) {
    std::snprintf(buf, sizeof(buf), "%.2fM", v / 1e6);
  } else if (v >= 1e4) {
    std::snprintf(buf, sizeof(buf), "%.1fK", v / 1e3);
  } else {
    std::snprintf(buf, sizeof(buf), "%.0f", v);
  }
  return buf;
}

std::string Micros(double us) {
  char buf[32];
  if (us >= 1000) {
    std::snprintf(buf, sizeof(buf), "%.2f ms", us / 1000);
  } else {
    std::snprintf(buf, sizeof(buf), "%.1f µs", us);
  }
  return buf;
}

const Result& Find(const std::string& name) {
  for (const Result& r : results) {
    if (r.name == name) return r;
  }
  std::fprintf(stderr, "no result named %s\n", name.c_str());
  std::exit(1);
}

void Record(const char* name, std::vector<double>& lat_us, double total_s) {
  std::sort(lat_us.begin(), lat_us.end());
  auto pct = [&](double p) { return lat_us[std::min(lat_us.size() - 1, static_cast<size_t>(p * lat_us.size()))]; };
  Result r{name, lat_us.size() / total_s, pct(0.50), pct(0.99), pct(0.999)};
  std::printf("%-12s %10.0f ops/s   p50 %8.2f us   p99 %8.2f us   p99.9 %9.2f us   max %10.2f us\n", name,
              r.ops_per_sec, r.p50_us, r.p99_us, r.p999_us, lat_us.back());
  results.push_back(r);
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
  Record(name, lat_us, std::chrono::duration<double>(Clock::now() - start).count());
}

// Runs ops_per_thread calls of op on each of threads threads and records
// aggregate throughput with per-operation latency percentiles.
void RunThreads(const char* name, int threads, uint64_t ops_per_thread,
                const std::function<void(int thread, uint64_t i)>& op) {
  std::vector<std::vector<double>> lat(threads);
  auto start = Clock::now();
  std::vector<std::thread> workers;
  for (int t = 0; t < threads; t++) {
    workers.emplace_back([&, t] {
      lat[t].reserve(ops_per_thread);
      for (uint64_t i = 0; i < ops_per_thread; i++) {
        auto t0 = Clock::now();
        op(t, i);
        lat[t].push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
      }
    });
  }
  for (auto& w : workers) w.join();
  double total_s = std::chrono::duration<double>(Clock::now() - start).count();
  std::vector<double> all;
  for (auto& l : lat) all.insert(all.end(), l.begin(), l.end());
  Record(name, all, total_s);
}

void Check(const Status& s) {
  if (!s.ok() && !s.IsNotFound()) {
    std::fprintf(stderr, "error: %s\n", s.ToString().c_str());
    std::exit(1);
  }
}

struct Shape {
  std::string levels;
  double space_amp = 0, write_amp = 0, stall_ms = 0;
  uint64_t compactions = 0;
};

// live_bytes: key + value bytes of the distinct live keys, for space amplification.
Shape PrintStats(DB* db, uint64_t live_bytes) {
  DBStats st = db->GetStats();
  Shape shape;
  for (int l = 0; l < kNumLevels; l++) {
    if (st.files_per_level[l] == 0) continue;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%sL%d %llu", shape.levels.empty() ? "" : ", ", l,
                  static_cast<unsigned long long>(st.files_per_level[l]));
    shape.levels += buf;
  }
  shape.space_amp = static_cast<double>(st.table_bytes) / live_bytes;
  std::printf("             tables per level: %s\n", shape.levels.c_str());
  std::printf("             %.1f MB on disk for %.1f MB live (space amp %.2fx)\n", st.table_bytes / 1048576.0,
              live_bytes / 1048576.0, shape.space_amp);
  if (st.user_bytes_written > 0) {
    shape.write_amp =
        static_cast<double>(st.flush_bytes_written + st.compaction_bytes_written) / st.user_bytes_written;
    shape.stall_ms = st.stall_micros / 1000.0;
    shape.compactions = st.compactions;
    std::printf("             flushes %llu, compactions %llu (+%llu trivial moves)\n",
                static_cast<unsigned long long>(st.flushes), static_cast<unsigned long long>(st.compactions),
                static_cast<unsigned long long>(st.trivial_moves));
    std::printf("             write amp %.2fx (table bytes written / user bytes; WAL adds ~1x), "
                "writers stalled %.0f ms\n",
                shape.write_amp, shape.stall_ms);
  }
  return shape;
}

}  // namespace

int main(int argc, char** argv) {
  uint64_t num = 1000000;
  uint64_t sync_ops = 100;  // Per thread, per sync-write run.
  size_t value_size = 100;
  size_t write_buffer_mb = 4;
  bool sync_writes = true;
  std::string dir, report;
  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "--num" && i + 1 < argc) num = std::strtoull(argv[++i], nullptr, 10);
    else if (arg == "--value-size" && i + 1 < argc) value_size = std::strtoull(argv[++i], nullptr, 10);
    else if (arg == "--write-buffer-mb" && i + 1 < argc) write_buffer_mb = std::strtoull(argv[++i], nullptr, 10);
    else if (arg == "--sync-ops" && i + 1 < argc) sync_ops = std::strtoull(argv[++i], nullptr, 10);
    else if (arg == "--no-sync-writes") sync_writes = false;
    else if (arg == "--report" && i + 1 < argc) report = argv[++i];
    else if (arg == "--dir" && i + 1 < argc) dir = argv[++i];
    else {
      std::fprintf(stderr,
                   "usage: kvbench [--num N] [--value-size BYTES] [--write-buffer-mb MB] [--sync-ops N] "
                   "[--no-sync-writes] [--report PATH] [--dir DIR]\n");
      return 2;
    }
  }
  if (dir.empty()) {
    dir = (std::filesystem::temp_directory_path() / ("ledgerkv-bench-" + std::to_string(::getpid()))).string();
  }
  // Each open table holds a file descriptor.
  rlimit rl;
  if (::getrlimit(RLIMIT_NOFILE, &rl) == 0) {
    rl.rlim_cur = std::min<rlim_t>(rl.rlim_max, 65536);
    ::setrlimit(RLIMIT_NOFILE, &rl);
  }

  std::printf("ledgerkv kvbench: num=%llu value_size=%zu write_buffer=%zuMB\n", static_cast<unsigned long long>(num),
              value_size, write_buffer_mb);
  std::mt19937_64 rng(42);
  std::string value(value_size, 'x');
  for (auto& c : value) c = static_cast<char>('a' + rng() % 26);

  Options options;
  options.sync = false;
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
  std::vector<bool> written(num);
  auto put_random = [&](uint64_t) {
    uint64_t k = rng() % num;
    written[k] = true;
    Check(db->Put(Key(k), value));
  };
  Run("fillrandom", num, put_random);
  Run("overwrite", num, put_random);
  const uint64_t live_keys = static_cast<uint64_t>(std::count(written.begin(), written.end(), true));
  const uint64_t live_bytes = live_keys * (16 + value_size);
  const Shape shape = PrintStats(db.get(), live_bytes);

  db.reset();
  auto t0 = Clock::now();
  db = open();
  const double reopen_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
  std::printf("%-12s %10.1f ms   (replays at most one memtable of log, then flushes it)\n", "reopen", reopen_ms);

  std::string out;
  Run("readrandom", num, [&](uint64_t) { Check(db->Get(Key(rng() % num), &out)); });
  ReadStats missing;
  ReadOptions counted;
  counted.stats = &missing;
  Run("readmissing", num, [&](uint64_t) { Check(db->Get(counted, Key(rng() % num) + "!", &out)); });
  const double skip_pct = 100.0 * static_cast<double>(missing.bloom_skips) / static_cast<double>(missing.tables_checked);
  std::printf("             %llu table checks for absent keys: %.2f%% skipped by the bloom filter with no read "
              "(%.3f block reads per lookup)\n",
              static_cast<unsigned long long>(missing.tables_checked), skip_pct,
              static_cast<double>(missing.blocks_read) / num);
  RunThreads("readrandom8", 8, num / 8, [&](int t, uint64_t i) {
    std::string v;
    Check(db->Get(Key(std::hash<uint64_t>()(i * 8 + static_cast<uint64_t>(t)) % num), &v));
  });

  double scan_per_sec = 0;
  {
    auto it = db->NewIterator();
    uint64_t entries = 0, bytes = 0;
    auto t1 = Clock::now();
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      entries++;
      bytes += it->key().size() + it->value().size();
    }
    Check(it->status());
    double secs = std::chrono::duration<double>(Clock::now() - t1).count();
    scan_per_sec = entries / secs;
    std::printf("%-12s %10.0f entries/s   %.0f MB/s   (%llu entries)\n", "scan", scan_per_sec,
                bytes / 1048576.0 / secs, static_cast<unsigned long long>(entries));
    Run("seekrandom", num / 10, [&](uint64_t) {
      it->Seek(Key(rng() % num));
      for (int j = 0; j < 10 && it->Valid(); j++) it->Next();
      Check(it->status());
    });
  }
  db.reset();
  std::filesystem::remove_all(dir);

  // Group commit: every write is fsynced before it is acknowledged, from T
  // concurrent writers. One fsync can cover every writer queued behind it.
  struct SyncRun {
    int threads;
    double writes_per_fsync;
  };
  std::vector<SyncRun> sync_runs;
  if (sync_writes) {
    std::printf("sync writes: every write fsynced (F_FULLFSYNC on macOS), %llu writes per thread\n",
                static_cast<unsigned long long>(sync_ops));
    for (int threads : {1, 4, 16, 64}) {
      std::filesystem::remove_all(dir);
      Options sync_options = options;
      sync_options.sync = true;
      std::unique_ptr<DB> sdb;
      Check(DB::Open(sync_options, dir, &sdb));
      char name[32];
      std::snprintf(name, sizeof(name), "sync x%d", threads);
      RunThreads(name, threads, sync_ops, [&](int t, uint64_t i) {
        Check(sdb->Put(Key(static_cast<uint64_t>(t) * 1000000 + i), value));
      });
      DBStats st = sdb->GetStats();
      sync_runs.push_back({threads, static_cast<double>(st.writes) / st.write_groups});
      std::printf("             %.1f writes per fsync\n", sync_runs.back().writes_per_fsync);
    }
    std::filesystem::remove_all(dir);
  }

  if (report.empty()) return 0;
  std::ostringstream md;
  md << "## " << Count(static_cast<double>(num)) << " operations per phase, " << value_size << "-byte values\n\n";
  md << "| Phase | ops/s | p50 | p99 | p99.9 |\n|---|---|---|---|---|\n";
  for (const Result& r : results) {
    if (r.name.rfind("sync", 0) == 0) continue;
    md << "| " << r.name << " | " << Count(r.ops_per_sec) << " | " << Micros(r.p50_us) << " | " << Micros(r.p99_us)
       << " | " << Micros(r.p999_us) << " |\n";
  }
  char line[512];
  std::snprintf(line, sizeof(line),
                "| scan | %s entries/s | | | |\n\n"
                "- Tables per level after %s writes: %s\n"
                "- Space amplification: %.2fx (%.0f MB on disk for %.0f MB of live data)\n"
                "- Write amplification: %.2fx table bytes per user byte (the WAL adds about 1x more)\n"
                "- Writers stalled on background work: %.1f s in total; %llu compactions\n"
                "- Reopen (recovery) time: %.0f ms\n"
                "- Absent-key reads: %.2f%% of %s table checks were answered by the bloom filter with no disk read\n",
                Count(scan_per_sec).c_str(), Count(2.0 * static_cast<double>(num)).c_str(), shape.levels.c_str(),
                shape.space_amp, shape.space_amp * live_bytes / 1048576.0, live_bytes / 1048576.0, shape.write_amp,
                shape.stall_ms / 1000, static_cast<unsigned long long>(shape.compactions), reopen_ms, skip_pct,
                Count(static_cast<double>(missing.tables_checked)).c_str());
  md << line;
  if (!sync_runs.empty()) {
    md << "\n### Group commit (every write fsynced before it is acknowledged)\n\n"
          "| Concurrent writers | Writes/s | Writes per fsync | p50 latency |\n|---|---|---|---|\n";
    for (const SyncRun& sr : sync_runs) {
      const Result& r = Find("sync x" + std::to_string(sr.threads));
      std::snprintf(line, sizeof(line), "| %d | %s | %.1f | %s |\n", sr.threads, Count(r.ops_per_sec).c_str(),
                    sr.writes_per_fsync, Micros(r.p50_us).c_str());
      md << line;
    }
  }

  md << "\n### Headline figures\n\n";
  std::snprintf(line, sizeof(line),
                "- **Random writes at %s keys:** %s/s inserting, %s/s overwriting (sequential: %s/s)\n"
                "- **Amplification:** %.1fx write, %.2fx space\n"
                "- **Random-read p99 at %s keys:** %s (p50 %s); %s reads/s on 8 threads\n"
                "- **Bloom filters:** %.1f%% of disk reads avoided for absent keys\n",
                Count(static_cast<double>(num)).c_str(), Count(Find("fillrandom").ops_per_sec).c_str(),
                Count(Find("overwrite").ops_per_sec).c_str(), Count(Find("fillseq").ops_per_sec).c_str(),
                shape.write_amp, shape.space_amp, Count(static_cast<double>(num)).c_str(),
                Micros(Find("readrandom").p99_us).c_str(), Micros(Find("readrandom").p50_us).c_str(),
                Count(Find("readrandom8").ops_per_sec).c_str(), skip_pct);
  md << line;
  if (!sync_runs.empty()) {
    double one = Find("sync x1").ops_per_sec, many = Find("sync x64").ops_per_sec;
    std::snprintf(line, sizeof(line), "- **Group commit:** %.0fx fsynced write throughput, %s -> %s writes/s at 64 writers\n",
                  many / one, Count(one).c_str(), Count(many).c_str());
    md << line;
  }
  md << "\n";
  std::ofstream(report, std::ios::app) << md.str();
  std::printf("appended results to %s\n", report.c_str());
  return 0;
}
