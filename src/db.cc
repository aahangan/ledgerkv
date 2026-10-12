#include "ledgerkv/db.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <queue>
#include <set>
#include <thread>
#include <vector>

#include "file_util.h"
#include "filename.h"
#include "log.h"
#include "memtable.h"
#include "table.h"
#include "table_builder.h"
#include "version_edit.h"
#include "wal.h"

namespace ledgerkv {
namespace {

constexpr size_t kMaxKeySize = 1 << 20;
constexpr size_t kMaxValueSize = std::numeric_limits<uint32_t>::max() / 2;

// Once the MANIFEST grows past this, the next edit rewrites it as a snapshot.
constexpr uint64_t kManifestRotateBytes = 4 << 20;

// How often a running compaction checks for a pending memtable flush or
// shutdown, in entries.
constexpr int kCompactionCheckInterval = 4096;

using Clock = std::chrono::steady_clock;

uint64_t MicrosSince(Clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
}

struct FileEntry {
  TableMeta meta;
  std::shared_ptr<Table> table;
};

// An immutable snapshot of the live tables. Readers grab a shared_ptr to the
// current Version and search it without holding any lock; flushes and
// compactions install a new Version rather than modifying the old one. A
// table deleted by a compaction stays readable through older Versions,
// because POSIX keeps an unlinked file alive while it is open.
struct Version {
  // levels[0] is newest first: flushes run one at a time, so higher file
  // numbers are newer, and the first level-0 table with an entry for a key
  // holds its latest value. Levels 1+ are sorted by key and non-overlapping.
  std::array<std::vector<FileEntry>, kNumLevels> levels;
};

bool Overlaps(const TableMeta& m, std::string_view lo, std::string_view hi) {
  return !(std::string_view(m.largest) < lo || std::string_view(m.smallest) > hi);
}

// Returns a new Version with edit applied to base. tables supplies the open
// Table for every added file.
std::shared_ptr<Version> ApplyEdit(const Version& base, const VersionEdit& edit,
                                   const std::map<uint64_t, std::shared_ptr<Table>>& tables) {
  auto v = std::make_shared<Version>(base);
  std::set<uint64_t> deleted(edit.deleted_tables.begin(), edit.deleted_tables.end());
  for (auto& level : v->levels) {
    std::erase_if(level, [&](const FileEntry& f) { return deleted.count(f.meta.number) > 0; });
  }
  for (const TableMeta& m : edit.added_tables) v->levels[m.level].push_back({m, tables.at(m.number)});
  std::sort(v->levels[0].begin(), v->levels[0].end(),
            [](const FileEntry& a, const FileEntry& b) { return a.meta.number > b.meta.number; });
  for (int level = 1; level < kNumLevels; level++) {
    std::sort(v->levels[level].begin(), v->levels[level].end(),
              [](const FileEntry& a, const FileEntry& b) { return a.meta.smallest < b.meta.smallest; });
  }
  return v;
}

std::set<uint64_t> LiveTables(const Version& v) {
  std::set<uint64_t> live;
  for (const auto& level : v.levels) {
    for (const FileEntry& f : level) live.insert(f.meta.number);
  }
  return live;
}

struct Compaction {
  int level = 0;                  // Inputs come from level and level + 1.
  std::vector<FileEntry> inputs;  // From level.
  std::vector<FileEntry> next;    // From level + 1, overlapping inputs' key range.
};

class DBImpl final : public DB {
 public:
  DBImpl(Options options, std::string dir) : options_(options), dir_(std::move(dir)) {}
  ~DBImpl() override;

  Status Init();

  Status Put(std::string_view key, std::string_view value) override {
    if (key.size() > kMaxKeySize) return Status::InvalidArgument("key too large");
    if (value.size() > kMaxValueSize) return Status::InvalidArgument("value too large");
    return Write(RecordType::kPut, key, value);
  }

  Status Delete(std::string_view key) override {
    if (key.size() > kMaxKeySize) return Status::InvalidArgument("key too large");
    return Write(RecordType::kDelete, key, {});
  }

  Status Get(std::string_view key, std::string* value) override;
  Status Flush() override;
  Status CompactAll() override;
  Status Verify() override;
  DBStats GetStats() override;

 private:
  Status AcquireLock();
  Status Write(RecordType type, std::string_view key, std::string_view value);

  // Makes sure the memtable has room, freezing it and handing it to the
  // background thread when full (or when force is set and it is non-empty).
  // Slows or blocks the writer when background work falls behind. Requires
  // write_mu_.
  Status MakeRoomForWrite(bool force);

  // --- Background thread only. ---
  void BackgroundLoop();
  Status FlushImmutable();
  Status RunManualCompaction();
  std::optional<Compaction> PickCompaction(const Version& v);
  Status DoCompaction(const Compaction& c);
  // Commits edit and installs the resulting Version.
  Status InstallEdit(VersionEdit* edit, const std::map<uint64_t, std::shared_ptr<Table>>& tables,
                     uint64_t min_log);
  // True if no level below out_level has a table whose range covers key, so
  // a tombstone for key being written to out_level shadows nothing.
  static bool IsBottommostForKey(const Version& v, int out_level, std::string_view key);
  void InstallVersion(std::shared_ptr<const Version> v);

  double MaxBytesForLevel(int level) const;
  // Highest compaction score across levels, and the level it belongs to. A
  // score of 1 or more means the level is over its limit.
  std::pair<int, double> CompactionScore(const Version& v) const;

  Status WriteTable(const MemTable& mem, TableMeta* meta);

  // Appends edit to the MANIFEST, or rewrites the MANIFEST as a snapshot of
  // v once it has grown large. Called only from Init() and the background
  // thread, so MANIFEST writes never race.
  Status LogAndApply(VersionEdit* edit, const Version& v);
  // Writes the full state as a single edit to MANIFEST.tmp, fsyncs it, and
  // renames it over MANIFEST. The rename is atomic, so a crash leaves either
  // the old MANIFEST or the new one.
  Status WriteManifestSnapshot(const Version& v);

  // Deletes logs numbered below min_log, MANIFEST.tmp, and tables that are
  // neither live nor being written. If set_aside_tables is set, unreferenced
  // tables are moved to lost/ instead of deleted.
  void RemoveObsoleteFiles(uint64_t min_log, const std::set<uint64_t>& live_tables,
                           bool set_aside_tables = false);

  const Options options_;
  const std::string dir_;
  int lock_fd_ = -1;
  std::atomic<uint64_t> next_file_number_{1};

  // MANIFEST state, touched only by Init() and the background thread.
  std::unique_ptr<LogWriter> manifest_;
  uint64_t manifest_bytes_ = 0;
  uint64_t committed_log_number_ = 0;
  uint64_t committed_last_seq_ = 0;
  // Tables being written by a compaction, which RemoveObsoleteFiles must not
  // treat as orphans.
  std::set<uint64_t> pending_outputs_;
  // Per level, the largest key of the last table compacted, so successive
  // compactions rotate through the key space.
  std::array<std::string, kNumLevels> compact_pointer_;

  // Write path, guarded by write_mu_. Writes are serialized so that WAL
  // order matches sequence number order.
  std::mutex write_mu_;
  uint64_t next_seq_ = 1;
  std::unique_ptr<WalWriter> wal_;
  // Once a WAL append fails, the log may end in a partial record, and
  // anything appended after it would be lost on recovery. All later writes
  // are rejected until the database is reopened.
  Status write_error_;

  // Shared state, guarded by mu_. mem_ is also only replaced while holding
  // write_mu_, so the write path may read it under write_mu_ alone. current_
  // is only replaced by the background thread (or Init), so that thread may
  // read it without mu_.
  std::mutex mu_;
  std::condition_variable bg_cv_;       // Wakes the background thread.
  std::condition_variable bg_done_cv_;  // Background work finished.
  std::shared_ptr<MemTable> mem_ = std::make_shared<MemTable>();
  std::shared_ptr<MemTable> imm_;  // Frozen memtable being flushed, if any.
  uint64_t imm_log_boundary_ = 0;  // First log number not covered by imm_.
  std::shared_ptr<const Version> current_ = std::make_shared<Version>();
  bool needs_compaction_ = false;
  bool manual_compaction_ = false;
  Status bg_error_;
  DBStats stats_;  // Counters only; level shapes are computed on demand.

  std::atomic<bool> shutting_down_{false};
  std::atomic<bool> has_imm_{false};
  std::atomic<uint64_t> user_bytes_written_{0};
  std::thread bg_thread_;
};

DBImpl::~DBImpl() {
  {
    std::lock_guard lock(mu_);
    shutting_down_ = true;
  }
  bg_cv_.notify_all();
  if (bg_thread_.joinable()) bg_thread_.join();
  // A memtable still being flushed is not lost: its log is kept until a
  // MANIFEST edit records the flush, so it is replayed on the next open. A
  // compaction cut short leaves its inputs live and its outputs as orphans.
  wal_.reset();
  manifest_.reset();
  current_.reset();
  if (lock_fd_ >= 0) ::close(lock_fd_);  // Releases the flock last.
}

Status DBImpl::AcquireLock() {
  std::string path = dir_ + "/LOCK";
  lock_fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (lock_fd_ < 0) return IOErrorFromErrno("open " + path);
  if (::flock(lock_fd_, LOCK_EX | LOCK_NB) != 0) {
    return IOErrorFromErrno("lock " + path + " (is another process using this database?)");
  }
  return Status::OK();
}

Status DBImpl::Init() {
  if (::mkdir(dir_.c_str(), 0755) != 0) {
    if (errno != EEXIST) return IOErrorFromErrno("mkdir " + dir_);
  } else if (!options_.create_if_missing) {
    return Status::InvalidArgument(dir_ + " does not exist");
  }
  Status s = AcquireLock();
  if (!s.ok()) return s;

  // 1. Rebuild the set of live tables by replaying the MANIFEST.
  std::map<uint64_t, TableMeta> tables;
  uint64_t log_number = 0, manifest_next_file = 1, last_seq = 0;
  bool bad_edit = false;
  LogReadStats mstats;
  const std::string manifest_path = ManifestFileName(dir_);
  s = ReadLog(
      manifest_path,
      [&](std::string_view record) {
        VersionEdit e;
        if (!e.DecodeFrom(record)) {
          bad_edit = true;
          return false;
        }
        if (e.log_number) log_number = *e.log_number;
        if (e.next_file_number) manifest_next_file = *e.next_file_number;
        if (e.last_sequence) last_seq = std::max(last_seq, *e.last_sequence);
        for (uint64_t n : e.deleted_tables) tables.erase(n);
        for (const TableMeta& t : e.added_tables) tables[t.number] = t;
        return true;
      },
      &mstats);
  if (!s.ok()) return s;
  // A record that passed its checksum but does not parse is real corruption,
  // not a torn write. Refuse to guess which tables are live.
  if (bad_edit) return Status::Corruption("undecodable record in " + manifest_path);
  // A damaged tail is usually a torn append from a crash, in which case any
  // table it named is an orphan. But bit rot in a committed edit looks the
  // same, and then the "orphan" may hold the only copy of flushed data. Set
  // unreferenced tables aside in lost/ rather than deleting them.
  const bool manifest_damaged = mstats.valid_bytes < mstats.file_bytes;

  // 2. Find the logs that may hold writes not yet in any table.
  std::vector<std::string> names;
  s = ListDir(dir_, &names);
  if (!s.ok()) return s;
  std::vector<uint64_t> logs;
  uint64_t max_number = 0;
  for (const std::string& name : names) {
    uint64_t number;
    FileType type = ParseFileName(name, &number);
    if (type == FileType::kLog || type == FileType::kTable) max_number = std::max(max_number, number);
    if (type == FileType::kLog && number >= log_number) logs.push_back(number);
  }
  std::sort(logs.begin(), logs.end());
  // Never reuse a number that is already on disk, even for an orphan.
  next_file_number_ = std::max(manifest_next_file, max_number + 1);

  // 3. Replay those logs, oldest first.
  MemTable recovered;
  uint64_t max_seq = last_seq;
  for (uint64_t number : logs) {
    std::string path = LogFileName(dir_, number);
    WalReadResult log;
    s = ReadWal(path, &log);
    if (!s.ok()) return s;
    if (log.valid_bytes < log.file_bytes) {
      // Drop a torn tail. Otherwise later appends would sit behind garbage.
      s = TruncateFile(path, log.valid_bytes);
      if (!s.ok()) return s;
    }
    for (const WalRecord& r : log.records) {
      if (r.type == RecordType::kPut) {
        recovered.Put(r.seq, r.key, r.value);
      } else {
        recovered.Delete(r.seq, r.key);
      }
      max_seq = std::max(max_seq, r.seq);
    }
  }
  next_seq_ = max_seq + 1;

  // 4. Open the live tables, and flush whatever was recovered to a new
  // level-0 table so the old logs can be deleted. This keeps recovery time
  // bounded by one memtable's worth of log, regardless of database size.
  std::map<uint64_t, std::shared_ptr<Table>> open_tables;
  VersionEdit edit;
  for (const auto& [number, meta] : tables) {
    s = Table::Open(TableFileName(dir_, number), meta.file_size, &open_tables[number]);
    if (!s.ok()) return s;
    edit.added_tables.push_back(meta);
  }
  if (!recovered.Empty()) {
    TableMeta meta;
    s = WriteTable(recovered, &meta);
    if (s.ok()) s = Table::Open(TableFileName(dir_, meta.number), meta.file_size, &open_tables[meta.number]);
    if (!s.ok()) return s;
    edit.added_tables.push_back(meta);
  }
  auto version = ApplyEdit(Version(), edit, open_tables);

  // 5. Start a fresh log and commit the new state as a MANIFEST snapshot,
  // which also compacts away the edit history.
  uint64_t new_log = next_file_number_++;
  s = WalWriter::Open(LogFileName(dir_, new_log), options_.sync, &wal_);
  if (!s.ok()) return s;
  committed_log_number_ = new_log;
  committed_last_seq_ = max_seq;
  s = WriteManifestSnapshot(*version);  // Also makes the new log's directory entry durable.
  if (!s.ok()) return s;

  RemoveObsoleteFiles(new_log, LiveTables(*version), /*set_aside_tables=*/manifest_damaged);
  InstallVersion(std::move(version));
  bg_thread_ = std::thread([this] { BackgroundLoop(); });
  return Status::OK();
}

Status DBImpl::WriteTable(const MemTable& mem, TableMeta* meta) {
  meta->level = 0;
  meta->number = next_file_number_++;
  std::string path = TableFileName(dir_, meta->number);
  TableBuilder::Options topts;
  topts.block_size = options_.block_size;
  topts.bloom_bits_per_key = options_.bloom_bits_per_key;
  TableBuilder builder(topts, path);
  Status s = builder.Open();
  if (s.ok()) {
    mem.ForEach([&](std::string_view key, uint64_t seq, RecordType type, std::string_view value) {
      builder.Add(key, seq, type, value);
    });
    s = builder.Finish();
  }
  if (!s.ok()) {
    builder.Abandon();
    return s;
  }
  meta->file_size = builder.FileSize();
  meta->smallest = builder.SmallestKey();
  meta->largest = builder.LargestKey();
  // The table's directory entry must be durable before the MANIFEST names it.
  return SyncDir(dir_);
}

Status DBImpl::LogAndApply(VersionEdit* edit, const Version& v) {
  if (edit->log_number) committed_log_number_ = *edit->log_number;
  if (edit->last_sequence) committed_last_seq_ = std::max(committed_last_seq_, *edit->last_sequence);
  edit->next_file_number = next_file_number_.load();
  if (manifest_bytes_ >= kManifestRotateBytes) return WriteManifestSnapshot(v);

  std::string record;
  edit->EncodeTo(&record);
  Status s = manifest_->AddRecord(record);
  if (s.ok()) manifest_bytes_ += kLogHeaderSize + record.size();
  return s;
}

Status DBImpl::WriteManifestSnapshot(const Version& v) {
  VersionEdit snapshot;
  snapshot.log_number = committed_log_number_;
  snapshot.next_file_number = next_file_number_.load();
  snapshot.last_sequence = committed_last_seq_;
  for (const auto& level : v.levels) {
    for (const FileEntry& f : level) snapshot.added_tables.push_back(f.meta);
  }
  std::string record;
  snapshot.EncodeTo(&record);

  const std::string tmp = TempManifestFileName(dir_);
  ::unlink(tmp.c_str());
  {
    std::unique_ptr<LogWriter> w;
    Status s = LogWriter::Open(tmp, /*sync=*/true, &w);
    if (s.ok()) s = w->AddRecord(record);
    if (!s.ok()) return s;
  }
  const std::string path = ManifestFileName(dir_);
  if (::rename(tmp.c_str(), path.c_str()) != 0) return IOErrorFromErrno("rename " + tmp);
  Status s = SyncDir(dir_);
  if (!s.ok()) return s;
  manifest_.reset();
  manifest_bytes_ = kLogHeaderSize + record.size();
  return LogWriter::Open(path, /*sync=*/true, &manifest_);
}

void DBImpl::RemoveObsoleteFiles(uint64_t min_log, const std::set<uint64_t>& live_tables,
                                 bool set_aside_tables) {
  std::vector<std::string> names;
  if (!ListDir(dir_, &names).ok()) return;  // Best effort: leftovers are retried next time.
  for (const std::string& name : names) {
    uint64_t number = 0;
    FileType type = ParseFileName(name, &number);
    std::string path = dir_ + "/" + name;
    if ((type == FileType::kLog && number < min_log) || type == FileType::kTemp) {
      ::unlink(path.c_str());
    } else if (type == FileType::kTable && live_tables.count(number) == 0 &&
               pending_outputs_.count(number) == 0) {
      if (set_aside_tables) {
        ::mkdir((dir_ + "/lost").c_str(), 0755);
        ::rename(path.c_str(), (dir_ + "/lost/" + name).c_str());
      } else {
        ::unlink(path.c_str());
      }
    }
  }
}

void DBImpl::InstallVersion(std::shared_ptr<const Version> v) {
  std::lock_guard lock(mu_);
  needs_compaction_ = CompactionScore(*v).second >= 1.0;
  current_ = std::move(v);
}

double DBImpl::MaxBytesForLevel(int level) const {
  double bytes = static_cast<double>(options_.max_bytes_for_level_base);
  for (int l = 1; l < level; l++) bytes *= options_.level_size_multiplier;
  return bytes;
}

std::pair<int, double> DBImpl::CompactionScore(const Version& v) const {
  int best_level = 0;
  double best = static_cast<double>(v.levels[0].size()) / options_.level0_compaction_trigger;
  // The last level has nowhere to compact to.
  for (int level = 1; level < kNumLevels - 1; level++) {
    uint64_t bytes = 0;
    for (const FileEntry& f : v.levels[level]) bytes += f.meta.file_size;
    double score = static_cast<double>(bytes) / MaxBytesForLevel(level);
    if (score > best) {
      best = score;
      best_level = level;
    }
  }
  return {best_level, best};
}

std::optional<Compaction> DBImpl::PickCompaction(const Version& v) {
  auto [level, score] = CompactionScore(v);
  if (score < 1.0) return std::nullopt;

  Compaction c;
  c.level = level;
  if (level == 0) {
    // Level-0 tables overlap, so take all of them.
    c.inputs = v.levels[0];
  } else {
    // Take the first table past the compact pointer, wrapping around.
    const auto& files = v.levels[level];
    auto it = std::find_if(files.begin(), files.end(),
                           [&](const FileEntry& f) { return f.meta.largest > compact_pointer_[level]; });
    c.inputs.push_back(it == files.end() ? files.front() : *it);
  }
  if (c.inputs.empty()) return std::nullopt;

  std::string lo = c.inputs[0].meta.smallest, hi = c.inputs[0].meta.largest;
  for (const FileEntry& f : c.inputs) {
    lo = std::min(lo, f.meta.smallest);
    hi = std::max(hi, f.meta.largest);
  }
  for (const FileEntry& f : v.levels[level + 1]) {
    if (Overlaps(f.meta, lo, hi)) c.next.push_back(f);
  }
  compact_pointer_[level] = hi;
  return c;
}

bool DBImpl::IsBottommostForKey(const Version& v, int out_level, std::string_view key) {
  for (int level = out_level + 1; level < kNumLevels; level++) {
    const auto& files = v.levels[level];
    auto it = std::lower_bound(files.begin(), files.end(), key, [](const FileEntry& f, std::string_view k) {
      return std::string_view(f.meta.largest) < k;
    });
    if (it != files.end() && std::string_view(it->meta.smallest) <= key) return false;
  }
  return true;
}

Status DBImpl::InstallEdit(VersionEdit* edit, const std::map<uint64_t, std::shared_ptr<Table>>& tables,
                           uint64_t min_log) {
  auto v = ApplyEdit(*current_, *edit, tables);
  Status s = LogAndApply(edit, *v);
  if (!s.ok()) return s;
  // The edit is committed, so anything it dropped is garbage. Delete it
  // before announcing the change so callers see a clean directory.
  for (const TableMeta& m : edit->added_tables) pending_outputs_.erase(m.number);
  RemoveObsoleteFiles(min_log, LiveTables(*v));
  InstallVersion(std::move(v));
  return Status::OK();
}

Status DBImpl::FlushImmutable() {
  std::shared_ptr<MemTable> imm;
  uint64_t log_boundary;
  {
    std::lock_guard lock(mu_);
    imm = imm_;
    log_boundary = imm_log_boundary_;
  }
  if (!imm) return Status::OK();

  // Crash ordering: the table is fsynced and its directory entry made
  // durable, then the MANIFEST edit commits it, and only then is the old log
  // deleted. A crash at any point leaves either the old log (replayed on
  // open) or a committed table, never neither.
  TableMeta meta;
  std::map<uint64_t, std::shared_ptr<Table>> tables;
  Status s = WriteTable(*imm, &meta);
  if (s.ok()) s = Table::Open(TableFileName(dir_, meta.number), meta.file_size, &tables[meta.number]);
  if (!s.ok()) return s;

  VersionEdit edit;
  edit.log_number = log_boundary;
  edit.last_sequence = imm->MaxSequence();
  edit.added_tables.push_back(meta);
  s = InstallEdit(&edit, tables, log_boundary);
  if (!s.ok()) return s;

  std::lock_guard lock(mu_);
  imm_.reset();
  has_imm_ = false;
  stats_.flushes++;
  stats_.flush_bytes_written += meta.file_size;
  bg_done_cv_.notify_all();
  return Status::OK();
}

Status DBImpl::DoCompaction(const Compaction& c) {
  const int out_level = c.level + 1;
  VersionEdit edit;
  std::map<uint64_t, std::shared_ptr<Table>> tables;

  // A lone table with nothing to merge against moves down without a rewrite.
  if (c.inputs.size() == 1 && c.next.empty()) {
    TableMeta moved = c.inputs[0].meta;
    edit.deleted_tables.push_back(moved.number);
    moved.level = out_level;
    edit.added_tables.push_back(moved);
    tables[moved.number] = c.inputs[0].table;
    Status s = InstallEdit(&edit, tables, committed_log_number_);
    if (s.ok()) {
      std::lock_guard lock(mu_);
      stats_.trivial_moves++;
    }
    return s;
  }

  // Tombstones are tested against the Version as of the start. Concurrent
  // flushes only add level-0 tables, which never sit below out_level.
  std::shared_ptr<const Version> base = current_;

  // k-way merge of every input, ordered by key, then newest first.
  std::vector<std::unique_ptr<Table::Iterator>> iters;
  uint64_t bytes_read = 0;
  for (const auto* files : {&c.inputs, &c.next}) {
    for (const FileEntry& f : *files) {
      iters.push_back(std::make_unique<Table::Iterator>(f.table.get()));
      edit.deleted_tables.push_back(f.meta.number);
      bytes_read += f.meta.file_size;
    }
  }
  auto later = [](const Table::Iterator* a, const Table::Iterator* b) {
    int cmp = a->key().compare(b->key());
    return cmp != 0 ? cmp > 0 : a->seq() < b->seq();
  };
  std::priority_queue<Table::Iterator*, std::vector<Table::Iterator*>, decltype(later)> heap(later);
  for (auto& it : iters) {
    it->SeekToFirst();
    if (it->Valid()) heap.push(it.get());
    if (!it->status().ok()) return it->status();
  }

  TableBuilder::Options topts;
  topts.block_size = options_.block_size;
  topts.bloom_bits_per_key = options_.bloom_bits_per_key;
  std::unique_ptr<TableBuilder> builder;
  TableMeta out;
  std::vector<TableMeta> outputs;
  uint64_t tombstones_dropped = 0;

  auto abandon = [&] {
    if (builder) builder->Abandon();
    pending_outputs_.clear();  // Finished outputs become orphans, deleted on the next pass.
  };
  auto finish_output = [&]() -> Status {
    Status s = builder->Finish();
    if (!s.ok()) return s;
    out.file_size = builder->FileSize();
    out.smallest = builder->SmallestKey();
    out.largest = builder->LargestKey();
    outputs.push_back(out);
    builder.reset();
    return Status::OK();
  };

  std::string last_key;
  bool has_last = false;
  int since_check = 0;
  while (!heap.empty()) {
    if (++since_check >= kCompactionCheckInterval) {
      since_check = 0;
      if (shutting_down_) {
        abandon();
        return Status::OK();
      }
      // A full memtable takes priority: writers stall until it is flushed.
      if (has_imm_) {
        Status s = FlushImmutable();
        if (!s.ok()) {
          abandon();
          return s;
        }
      }
    }

    Table::Iterator* it = heap.top();
    heap.pop();
    // Only the newest entry for each key survives. Without snapshots
    // (milestone 4) no reader can see the older ones.
    bool newest = !has_last || it->key() != std::string_view(last_key);
    if (newest) {
      last_key.assign(it->key());
      has_last = true;
      if (it->type() == RecordType::kDelete && IsBottommostForKey(*base, out_level, it->key())) {
        // Nothing older lies below, so the tombstone has nothing left to hide.
        tombstones_dropped++;
      } else {
        if (!builder) {
          out = TableMeta();
          out.level = out_level;
          out.number = next_file_number_++;
          pending_outputs_.insert(out.number);
          builder = std::make_unique<TableBuilder>(topts, TableFileName(dir_, out.number));
          Status s = builder->Open();
          if (!s.ok()) {
            abandon();
            return s;
          }
        }
        builder->Add(it->key(), it->seq(), it->type(), it->value());
        // Outputs split only between distinct keys, so tables in out_level
        // never share a key.
        if (builder->FileSize() >= options_.target_file_size) {
          Status s = finish_output();
          if (!s.ok()) {
            abandon();
            return s;
          }
        }
      }
    }
    it->Next();
    if (it->Valid()) {
      heap.push(it);
    } else if (!it->status().ok()) {
      abandon();
      return it->status();
    }
  }
  if (builder) {
    Status s = finish_output();
    if (!s.ok()) {
      abandon();
      return s;
    }
  }

  // Same crash ordering as a flush: outputs durable, then the MANIFEST edit
  // swaps inputs for outputs atomically, then inputs are deleted.
  Status s = SyncDir(dir_);
  uint64_t bytes_written = 0;
  for (const TableMeta& m : outputs) {
    if (s.ok()) s = Table::Open(TableFileName(dir_, m.number), m.file_size, &tables[m.number]);
    edit.added_tables.push_back(m);
    bytes_written += m.file_size;
  }
  if (s.ok()) s = InstallEdit(&edit, tables, committed_log_number_);
  if (!s.ok()) {
    abandon();
    return s;
  }
  pending_outputs_.clear();

  std::lock_guard lock(mu_);
  stats_.compactions++;
  stats_.tombstones_dropped += tombstones_dropped;
  stats_.compaction_bytes_read += bytes_read;
  stats_.compaction_bytes_written += bytes_written;
  return Status::OK();
}

Status DBImpl::RunManualCompaction() {
  for (int level = 0; level < kNumLevels - 1; level++) {
    if (shutting_down_) return Status::OK();
    if (has_imm_) {
      Status s = FlushImmutable();
      if (!s.ok()) return s;
    }
    std::shared_ptr<const Version> v = current_;
    if (v->levels[level].empty()) continue;
    Compaction c;
    c.level = level;
    c.inputs = v->levels[level];
    std::string lo = c.inputs[0].meta.smallest, hi = c.inputs[0].meta.largest;
    for (const FileEntry& f : c.inputs) {
      lo = std::min(lo, f.meta.smallest);
      hi = std::max(hi, f.meta.largest);
    }
    for (const FileEntry& f : v->levels[level + 1]) {
      if (Overlaps(f.meta, lo, hi)) c.next.push_back(f);
    }
    Status s = DoCompaction(c);
    if (!s.ok()) return s;
  }
  return Status::OK();
}

void DBImpl::BackgroundLoop() {
  std::unique_lock lock(mu_);
  while (true) {
    bg_cv_.wait(lock, [&] {
      return shutting_down_ || (bg_error_.ok() && (imm_ || manual_compaction_ || needs_compaction_));
    });
    if (shutting_down_) return;
    const bool flush = imm_ != nullptr;
    const bool manual = !flush && manual_compaction_;
    lock.unlock();

    // Flushes come first: writers may be waiting on them.
    Status s;
    if (flush) {
      s = FlushImmutable();
    } else if (manual) {
      s = RunManualCompaction();
    } else if (auto c = PickCompaction(*current_)) {
      s = DoCompaction(*c);
    }

    lock.lock();
    if (manual) manual_compaction_ = false;
    if (!s.ok() && bg_error_.ok()) bg_error_ = s;
    bg_done_cv_.notify_all();
  }
}

Status DBImpl::MakeRoomForWrite(bool force) {
  bool allow_delay = !force;
  std::unique_lock lock(mu_);
  while (true) {
    if (!bg_error_.ok()) return bg_error_;
    const size_t l0 = current_->levels[0].size();
    if (allow_delay && l0 >= static_cast<size_t>(options_.level0_slowdown_trigger)) {
      // Level 0 is getting deep. Rather than stopping writes outright later,
      // spread the delay: give compaction 1 ms of headroom per write.
      lock.unlock();
      auto start = Clock::now();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      lock.lock();
      stats_.stall_micros += MicrosSince(start);
      allow_delay = false;
      continue;
    }
    if (force ? mem_->Empty() : mem_->ApproximateBytes() < options_.write_buffer_size) {
      return Status::OK();
    }
    if (imm_ || l0 >= static_cast<size_t>(options_.level0_stop_trigger)) {
      // The previous memtable is still being flushed, or level 0 is full.
      // Stall rather than let unflushed data or read cost grow without bound.
      auto start = Clock::now();
      bg_done_cv_.wait(lock);
      stats_.stall_micros += MicrosSince(start);
      continue;
    }

    uint64_t number = next_file_number_++;
    std::unique_ptr<WalWriter> wal;
    Status s = WalWriter::Open(LogFileName(dir_, number), options_.sync, &wal);
    if (s.ok() && options_.sync) s = SyncDir(dir_);
    if (!s.ok()) return s;
    wal_ = std::move(wal);
    imm_ = mem_;
    imm_log_boundary_ = number;
    has_imm_ = true;
    mem_ = std::make_shared<MemTable>();
    bg_cv_.notify_one();
    return Status::OK();
  }
}

Status DBImpl::Write(RecordType type, std::string_view key, std::string_view value) {
  std::lock_guard lock(write_mu_);
  if (!write_error_.ok()) return write_error_;
  Status s = MakeRoomForWrite(/*force=*/false);
  if (!s.ok()) return s;

  uint64_t seq = next_seq_;
  s = wal_->Append(type, seq, key, value);
  if (!s.ok()) {
    write_error_ = s;
    return s;
  }
  if (type == RecordType::kPut) {
    mem_->Put(seq, key, value);
  } else {
    mem_->Delete(seq, key);
  }
  next_seq_++;
  user_bytes_written_ += key.size() + value.size();
  return Status::OK();
}

Status DBImpl::Get(std::string_view key, std::string* value) {
  std::shared_ptr<MemTable> mem, imm;
  std::shared_ptr<const Version> v;
  {
    std::lock_guard lock(mu_);
    mem = mem_;
    imm = imm_;
    v = current_;
  }

  // Search newest to oldest; the first layer with an entry for key decides.
  LookupResult r = mem->Get(key, value);
  if (r == LookupResult::kMissing && imm) r = imm->Get(key, value);
  for (int level = 0; level < kNumLevels && r == LookupResult::kMissing; level++) {
    const auto& files = v->levels[level];
    if (level == 0) {
      for (const FileEntry& f : files) {
        if (!Overlaps(f.meta, key, key)) continue;
        Status s = f.table->Get(key, value, &r);
        if (!s.ok()) return s;
        if (r != LookupResult::kMissing) break;
      }
    } else {
      // At most one table per level can hold key.
      auto it = std::lower_bound(files.begin(), files.end(), key, [](const FileEntry& f, std::string_view k) {
        return std::string_view(f.meta.largest) < k;
      });
      if (it != files.end() && std::string_view(it->meta.smallest) <= key) {
        Status s = it->table->Get(key, value, &r);
        if (!s.ok()) return s;
      }
    }
  }
  return r == LookupResult::kFound ? Status::OK() : Status::NotFound();
}

Status DBImpl::Flush() {
  {
    std::lock_guard lock(write_mu_);
    if (!write_error_.ok()) return write_error_;
    Status s = MakeRoomForWrite(/*force=*/true);
    if (!s.ok()) return s;
  }
  std::unique_lock lock(mu_);
  bg_done_cv_.wait(lock, [&] { return !imm_ || !bg_error_.ok(); });
  return bg_error_;
}

Status DBImpl::CompactAll() {
  Status s = Flush();
  if (!s.ok()) return s;
  std::unique_lock lock(mu_);
  manual_compaction_ = true;
  bg_cv_.notify_one();
  bg_done_cv_.wait(lock, [&] { return !manual_compaction_ || !bg_error_.ok(); });
  return bg_error_;
}

Status DBImpl::Verify() {
  std::shared_ptr<const Version> v;
  {
    std::lock_guard lock(mu_);
    v = current_;
  }
  auto fail = [](const TableMeta& m, const std::string& what) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "table %llu (level %d): ", static_cast<unsigned long long>(m.number), m.level);
    return Status::Corruption(buf + what);
  };
  for (int level = 0; level < kNumLevels; level++) {
    const auto& files = v->levels[level];
    for (size_t i = 0; i < files.size(); i++) {
      const TableMeta& m = files[i].meta;
      if (m.level != level) return fail(m, "recorded at the wrong level");
      if (level > 0 && i > 0 && !(files[i - 1].meta.largest < m.smallest)) {
        return fail(m, "overlaps its neighbor");
      }
      std::string prev;
      bool first = true;
      Table::Iterator it(files[i].table.get());
      for (it.SeekToFirst(); it.Valid(); it.Next()) {
        if (!first && !(std::string_view(prev) < it.key())) return fail(m, "keys out of order");
        if (it.key() < std::string_view(m.smallest) || it.key() > std::string_view(m.largest)) {
          return fail(m, "key outside recorded range");
        }
        prev.assign(it.key());
        first = false;
      }
      if (!it.status().ok()) return it.status();
    }
  }
  return Status::OK();
}

DBStats DBImpl::GetStats() {
  std::lock_guard lock(mu_);
  DBStats stats = stats_;
  for (int level = 0; level < kNumLevels; level++) {
    for (const FileEntry& f : current_->levels[level]) {
      stats.files_per_level[level]++;
      stats.bytes_per_level[level] += f.meta.file_size;
    }
    stats.num_tables += stats.files_per_level[level];
    stats.table_bytes += stats.bytes_per_level[level];
  }
  stats.user_bytes_written = user_bytes_written_;
  return stats;
}

}  // namespace

Status DB::Open(const Options& options, const std::string& dir, std::unique_ptr<DB>* db) {
  auto impl = std::make_unique<DBImpl>(options, dir);
  Status s = impl->Init();
  if (!s.ok()) return s;
  *db = std::move(impl);
  return Status::OK();
}

}  // namespace ledgerkv
