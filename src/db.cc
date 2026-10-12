#include "ledgerkv/db.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <limits>
#include <mutex>

#include "file_util.h"
#include "memtable.h"
#include "wal.h"

namespace ledgerkv {
namespace {

constexpr size_t kMaxKeySize = 1 << 20;
constexpr size_t kMaxValueSize = std::numeric_limits<uint32_t>::max() / 2;

class DBImpl final : public DB {
 public:
  DBImpl(Options options, std::string dir) : options_(options), dir_(std::move(dir)) {}

  ~DBImpl() override {
    if (lock_fd_ >= 0) ::close(lock_fd_);  // Releases the flock.
  }

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

  Status Get(std::string_view key, std::string* value) override {
    if (mem_.Get(key, value) == MemTable::LookupResult::kFound) return Status::OK();
    return Status::NotFound();
  }

 private:
  Status Write(RecordType type, std::string_view key, std::string_view value);
  Status AcquireLock();

  std::string WalPath() const { return dir_ + "/wal.log"; }

  const Options options_;
  const std::string dir_;
  int lock_fd_ = -1;

  std::mutex write_mu_;  // Serializes WAL appends so log order matches seq order.
  uint64_t next_seq_ = 1;
  std::unique_ptr<WalWriter> wal_;
  // Once a WAL append fails, the log may end in a partial record. Anything
  // appended after it would be unreachable on recovery, so all later writes
  // are rejected until the database is reopened.
  Status write_error_;

  MemTable mem_;
};

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

  WalReadResult log;
  s = ReadWal(WalPath(), &log);
  if (!s.ok()) return s;

  if (log.valid_bytes < log.file_bytes) {
    // Drop the torn tail so new records are appended right after the last
    // intact one. Otherwise they would sit behind garbage and be lost on the
    // next recovery.
    int fd = ::open(WalPath().c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) return IOErrorFromErrno("open " + WalPath());
    if (::ftruncate(fd, static_cast<off_t>(log.valid_bytes)) != 0) {
      s = IOErrorFromErrno("truncate " + WalPath());
    } else {
      s = SyncFd(fd, WalPath());
    }
    ::close(fd);
    if (!s.ok()) return s;
  }

  for (const WalRecord& r : log.records) {
    if (r.type == RecordType::kPut) {
      mem_.Put(r.seq, r.key, r.value);
    } else {
      mem_.Delete(r.seq, r.key);
    }
    next_seq_ = r.seq + 1;
  }

  bool existed = log.file_bytes > 0 || ::access(WalPath().c_str(), F_OK) == 0;
  s = WalWriter::Open(WalPath(), options_.sync, &wal_);
  if (!s.ok()) return s;
  if (!existed) s = SyncDir(dir_);  // Make the new log file's directory entry durable.
  return s;
}

Status DBImpl::Write(RecordType type, std::string_view key, std::string_view value) {
  std::lock_guard lock(write_mu_);
  if (!write_error_.ok()) return write_error_;

  WalRecord record{type, next_seq_, std::string(key), std::string(value)};
  Status s = wal_->Append(record);
  if (!s.ok()) {
    write_error_ = s;
    return s;
  }

  if (type == RecordType::kPut) {
    mem_.Put(record.seq, key, value);
  } else {
    mem_.Delete(record.seq, key);
  }
  next_seq_++;
  return Status::OK();
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
