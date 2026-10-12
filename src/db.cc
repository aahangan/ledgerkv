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
#include <limits>
#include <map>
#include <mutex>
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

// An immutable snapshot of the live tables. Readers grab a shared_ptr to the
// current Version and search it without holding any lock; a flush installs a
// new Version rather than modifying the old one.
struct Version {
  // Newest first: the first table with an entry for a key holds its latest
  // value. Flushes run one at a time, so higher file numbers are newer.
  std::vector<std::shared_ptr<Table>> tables;
  std::vector<TableMeta> metas;  // Parallel to tables.
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
  DBStats GetStats() override;

 private:
  Status AcquireLock();
  Status Write(RecordType type, std::string_view key, std::string_view value);

  // Freezes the memtable and hands it to the background thread if it is full
  // (or if force is set and it is non-empty). Blocks while a previous flush
  // is still running. Requires write_mu_.
  Status MakeRoomForWrite(bool force);

  void BackgroundLoop();

  // Writes mem to a new, fsynced table file.
  Status WriteTable(const MemTable& mem, TableMeta* meta);

  // Appends edit to the MANIFEST and fsyncs it. Called only from Init() and
  // the background thread, so MANIFEST writes never race.
  Status LogAndApply(VersionEdit* edit);

  // Deletes logs numbered below min_log and, if delete_tables is set, tables
  // not in live_tables.
  void RemoveObsoleteFiles(uint64_t min_log, const std::set<uint64_t>& live_tables,
                           bool delete_tables = true);

  const Options options_;
  const std::string dir_;
  int lock_fd_ = -1;
  std::atomic<uint64_t> next_file_number_{1};
  std::unique_ptr<LogWriter> manifest_;

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
  // write_mu_, so the write path may read it under write_mu_ alone.
  std::mutex mu_;
  std::condition_variable bg_cv_;     // Signals the background thread.
  std::condition_variable flush_cv_;  // Signals that a flush finished.
  std::shared_ptr<MemTable> mem_ = std::make_shared<MemTable>();
  std::shared_ptr<MemTable> imm_;  // Frozen memtable being flushed, if any.
  uint64_t imm_log_boundary_ = 0;  // First log number not covered by imm_.
  std::shared_ptr<const Version> current_ = std::make_shared<Version>();
  Status bg_error_;
  bool shutting_down_ = false;
  uint64_t flushes_ = 0;
  uint64_t stall_micros_ = 0;

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
  // MANIFEST edit records the flush, so it is replayed on the next open.
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
        for (const TableMeta& t : e.added_tables) tables[t.number] = t;
        for (uint64_t n : e.deleted_tables) tables.erase(n);
        return true;
      },
      &mstats);
  if (!s.ok()) return s;
  // A record that passed its checksum but does not parse is real corruption,
  // not a torn write. Refuse to guess which tables are live.
  if (bad_edit) return Status::Corruption("undecodable record in " + manifest_path);
  // A damaged tail is usually a torn append from a crash, in which case any
  // table it named is an orphan. But bit rot in a committed edit looks the
  // same, and then the "orphan" may hold the only copy of flushed data. Keep
  // unreferenced tables in that case: leaking a file beats deleting data.
  const bool manifest_damaged = mstats.valid_bytes < mstats.file_bytes;
  if (manifest_damaged) {
    s = TruncateFile(manifest_path, mstats.valid_bytes);
    if (!s.ok()) return s;
  }

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

  // 4. Start a fresh log, and flush whatever was recovered to a table so the
  // old logs can be deleted. This keeps recovery time bounded by one
  // memtable's worth of log, regardless of total database size.
  uint64_t new_log = next_file_number_++;
  s = WalWriter::Open(LogFileName(dir_, new_log), options_.sync, &wal_);
  if (!s.ok()) return s;
  s = LogWriter::Open(manifest_path, /*sync=*/true, &manifest_);
  if (!s.ok()) return s;

  VersionEdit edit;
  edit.log_number = new_log;
  edit.last_sequence = max_seq;
  if (!recovered.Empty()) {
    TableMeta meta;
    s = WriteTable(recovered, &meta);
    if (!s.ok()) return s;
    edit.added_tables.push_back(meta);
    tables[meta.number] = meta;
  }
  s = SyncDir(dir_);  // New log and MANIFEST entries must be durable first.
  if (s.ok()) s = LogAndApply(&edit);
  if (!s.ok()) return s;

  // 5. Open the live tables, newest first.
  auto version = std::make_shared<Version>();
  std::set<uint64_t> live;
  for (auto it = tables.rbegin(); it != tables.rend(); ++it) {
    std::shared_ptr<Table> table;
    s = Table::Open(TableFileName(dir_, it->first), it->second.file_size, &table);
    if (!s.ok()) return s;
    version->tables.push_back(std::move(table));
    version->metas.push_back(it->second);
    live.insert(it->first);
  }
  current_ = std::move(version);

  RemoveObsoleteFiles(new_log, live, /*delete_tables=*/!manifest_damaged);
  bg_thread_ = std::thread([this] { BackgroundLoop(); });
  return Status::OK();
}

Status DBImpl::WriteTable(const MemTable& mem, TableMeta* meta) {
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

Status DBImpl::LogAndApply(VersionEdit* edit) {
  edit->next_file_number = next_file_number_.load();
  std::string record;
  edit->EncodeTo(&record);
  return manifest_->AddRecord(record);
}

void DBImpl::RemoveObsoleteFiles(uint64_t min_log, const std::set<uint64_t>& live_tables,
                                 bool delete_tables) {
  std::vector<std::string> names;
  if (!ListDir(dir_, &names).ok()) return;  // Best effort: leftovers are retried next time.
  for (const std::string& name : names) {
    uint64_t number;
    FileType type = ParseFileName(name, &number);
    if ((type == FileType::kLog && number < min_log) ||
        (delete_tables && type == FileType::kTable && live_tables.count(number) == 0)) {
      ::unlink((dir_ + "/" + name).c_str());
    }
  }
}

void DBImpl::BackgroundLoop() {
  std::unique_lock lock(mu_);
  while (true) {
    bg_cv_.wait(lock, [&] { return shutting_down_ || (imm_ && bg_error_.ok()); });
    if (shutting_down_) return;
    std::shared_ptr<MemTable> imm = imm_;
    uint64_t log_boundary = imm_log_boundary_;
    lock.unlock();

    // Crash ordering: the table is fsynced and its directory entry made
    // durable, then the MANIFEST edit commits it, and only then is the old
    // log deleted. A crash at any point leaves either the old log (replayed
    // on open) or a committed table, never neither.
    TableMeta meta;
    std::shared_ptr<Table> table;
    Status s = WriteTable(*imm, &meta);
    if (s.ok()) s = Table::Open(TableFileName(dir_, meta.number), meta.file_size, &table);
    if (s.ok()) {
      VersionEdit edit;
      edit.log_number = log_boundary;
      edit.last_sequence = imm->MaxSequence();
      edit.added_tables.push_back(meta);
      s = LogAndApply(&edit);
    }

    if (!s.ok()) {
      lock.lock();
      bg_error_ = s;
      flush_cv_.notify_all();
      continue;
    }

    // Only this thread replaces current_, so reading it unlocked is safe.
    auto version = std::make_shared<Version>(*current_);
    version->tables.insert(version->tables.begin(), table);
    version->metas.insert(version->metas.begin(), meta);
    std::set<uint64_t> live;
    for (const TableMeta& m : version->metas) live.insert(m.number);
    // The edit is committed, so the flushed log is garbage. Delete it before
    // announcing the flush so that Flush() returns with the directory clean.
    RemoveObsoleteFiles(log_boundary, live);

    lock.lock();
    current_ = std::move(version);
    imm_.reset();
    flushes_++;
    flush_cv_.notify_all();
  }
}

Status DBImpl::MakeRoomForWrite(bool force) {
  if (force ? mem_->Empty() : mem_->ApproximateBytes() < options_.write_buffer_size) {
    return Status::OK();
  }
  std::unique_lock lock(mu_);
  if (imm_) {
    // The previous memtable is still being flushed. Stall rather than let
    // unflushed data pile up without bound.
    auto start = std::chrono::steady_clock::now();
    flush_cv_.wait(lock, [&] { return !imm_ || !bg_error_.ok(); });
    stall_micros_ += std::chrono::duration_cast<std::chrono::microseconds>(
                         std::chrono::steady_clock::now() - start)
                         .count();
  }
  if (!bg_error_.ok()) return bg_error_;

  uint64_t number = next_file_number_++;
  std::unique_ptr<WalWriter> wal;
  Status s = WalWriter::Open(LogFileName(dir_, number), options_.sync, &wal);
  if (s.ok() && options_.sync) s = SyncDir(dir_);
  if (!s.ok()) return s;
  wal_ = std::move(wal);
  imm_ = mem_;
  imm_log_boundary_ = number;
  mem_ = std::make_shared<MemTable>();
  bg_cv_.notify_one();
  return Status::OK();
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
  return Status::OK();
}

Status DBImpl::Get(std::string_view key, std::string* value) {
  std::shared_ptr<MemTable> mem, imm;
  std::shared_ptr<const Version> version;
  {
    std::lock_guard lock(mu_);
    mem = mem_;
    imm = imm_;
    version = current_;
  }

  // Search newest to oldest; the first layer with an entry for key decides.
  LookupResult r = mem->Get(key, value);
  if (r == LookupResult::kMissing && imm) r = imm->Get(key, value);
  if (r == LookupResult::kMissing) {
    for (const auto& table : version->tables) {
      Status s = table->Get(key, value, &r);
      if (!s.ok()) return s;
      if (r != LookupResult::kMissing) break;
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
  flush_cv_.wait(lock, [&] { return !imm_ || !bg_error_.ok(); });
  return bg_error_;
}

DBStats DBImpl::GetStats() {
  std::lock_guard lock(mu_);
  DBStats stats;
  stats.num_tables = current_->tables.size();
  for (const TableMeta& m : current_->metas) stats.table_bytes += m.file_size;
  stats.flushes = flushes_;
  stats.stall_micros = stall_micros_;
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
