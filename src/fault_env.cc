#include "fault_env.h"

#include <algorithm>

namespace ledgerkv {
namespace {

std::string DirOf(const std::string& path) {
  size_t slash = path.rfind('/');
  return slash == std::string::npos ? "." : path.substr(0, slash);
}

std::string BaseName(const std::string& path) {
  size_t slash = path.rfind('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

constexpr const char* kTrashDir = ".fault-trash";

// Stands in for a file opened while the machine is "off": swallows writes.
class NullFile final : public WritableFile {
 public:
  Status Append(std::string_view) override { return Status::OK(); }
  Status Sync() override { return Status::OK(); }
  Status Close() override { return Status::OK(); }
};

}  // namespace

class FaultInjectionEnv::File final : public WritableFile {
 public:
  File(FaultInjectionEnv* env, std::string path, std::unique_ptr<WritableFile> base)
      : env_(env), path_(std::move(path)), base_(std::move(base)) {}

  Status Append(std::string_view data) override {
    size_t keep = data.size();
    {
      std::lock_guard lock(env_->mu_);
      if (!env_->active_) return Status::OK();
      if (env_->ShouldFail(Op::kAppend, path_)) {
        // A real short write: some prefix may reach the file before the error.
        keep = data.empty() ? 0 : env_->rng_() % data.size();
        if (keep > 0 && base_->Append(data.substr(0, keep)).ok()) env_->files_[path_].size += keep;
        return Status::IOError("injected append failure: " + path_);
      }
    }
    Status s = base_->Append(data);
    if (s.ok()) {
      std::lock_guard lock(env_->mu_);
      env_->files_[path_].size += data.size();
    }
    return s;
  }

  Status Sync() override {
    std::lock_guard lock(env_->mu_);
    if (!env_->active_) return Status::OK();
    if (env_->ShouldFail(Op::kSync, path_)) return Status::IOError("injected sync failure: " + path_);
    FileState& st = env_->files_[path_];
    st.synced = st.size;
    return Status::OK();
  }

  Status Close() override { return base_->Close(); }

 private:
  FaultInjectionEnv* const env_;
  const std::string path_;
  std::unique_ptr<WritableFile> base_;
};

class FaultInjectionEnv::ReadFile final : public RandomAccessFile {
 public:
  ReadFile(FaultInjectionEnv* env, std::string path, std::unique_ptr<RandomAccessFile> base)
      : env_(env), path_(std::move(path)), base_(std::move(base)) {}

  Status Read(uint64_t offset, size_t n, std::string* out) const override {
    {
      std::lock_guard lock(env_->mu_);
      if (env_->ShouldFail(Op::kRead, path_)) return Status::IOError("injected read failure: " + path_);
    }
    return base_->Read(offset, n, out);
  }

 private:
  FaultInjectionEnv* const env_;
  const std::string path_;
  std::unique_ptr<RandomAccessFile> base_;
};

FaultInjectionEnv::FaultInjectionEnv(Env* base, uint64_t seed) : base_(base), rng_(seed) {}

void FaultInjectionEnv::SetActive(bool active) {
  std::lock_guard lock(mu_);
  active_ = active;
}

bool FaultInjectionEnv::IsActive() {
  std::lock_guard lock(mu_);
  return active_;
}

void FaultInjectionEnv::InjectFault(Op op, const std::string& path_substr, int after) {
  std::lock_guard lock(mu_);
  faults_.push_back({op, path_substr, after, -1});
}

void FaultInjectionEnv::InjectRandomFault(Op op, const std::string& path_substr, double p) {
  std::lock_guard lock(mu_);
  faults_.push_back({op, path_substr, 0, p});
}

void FaultInjectionEnv::ClearFaults() {
  std::lock_guard lock(mu_);
  faults_.clear();
}

uint64_t FaultInjectionEnv::FaultsTriggered() {
  std::lock_guard lock(mu_);
  return faults_triggered_;
}

bool FaultInjectionEnv::ShouldFail(Op op, const std::string& path) {
  for (Fault& f : faults_) {
    if (f.op != op || path.find(f.path_substr) == std::string::npos) continue;
    bool fail;
    if (f.probability >= 0) {
      fail = std::uniform_real_distribution<double>(0, 1)(rng_) < f.probability;
    } else if (f.remaining > 0) {
      f.remaining--;
      fail = false;
    } else {
      fail = true;
    }
    if (fail) {
      faults_triggered_++;
      return true;
    }
  }
  return false;
}

Status FaultInjectionEnv::Fail(Op op, const std::string& path) {
  (void)op;
  return Status::IOError("injected failure: " + path);
}

std::string FaultInjectionEnv::Park(const std::string& path) {
  std::string trash = DirOf(path) + "/" + kTrashDir;
  bool created;
  base_->CreateDirIfMissing(trash, &created);
  std::string parked = trash + "/" + BaseName(path) + "." + std::to_string(park_counter_++);
  base_->RenameFile(path, parked);
  return parked;
}

Status FaultInjectionEnv::NewWritableFile(const std::string& path, std::unique_ptr<WritableFile>* out) {
  std::lock_guard lock(mu_);
  if (!active_) {
    *out = std::make_unique<NullFile>();
    return Status::OK();
  }
  if (ShouldFail(Op::kOpenForWrite, path)) return Fail(Op::kOpenForWrite, path);
  bool existed = base_->FileExists(path);
  std::unique_ptr<WritableFile> file;
  Status s = base_->NewWritableFile(path, &file);
  if (!s.ok()) return s;
  FileState& st = files_[path];
  bool entry_durable = existed && (st.entry_durable || st.size == 0);
  // Truncating an existing file is itself unsynced data loss waiting to happen.
  st = FileState{0, 0, entry_durable};
  *out = std::make_unique<File>(this, path, std::move(file));
  return Status::OK();
}

Status FaultInjectionEnv::NewAppendableFile(const std::string& path, std::unique_ptr<WritableFile>* out) {
  std::lock_guard lock(mu_);
  if (!active_) {
    *out = std::make_unique<NullFile>();
    return Status::OK();
  }
  if (ShouldFail(Op::kOpenForWrite, path)) return Fail(Op::kOpenForWrite, path);
  bool existed = base_->FileExists(path);
  uint64_t size = 0;
  if (existed) base_->GetFileSize(path, &size);
  std::unique_ptr<WritableFile> file;
  Status s = base_->NewAppendableFile(path, &file);
  if (!s.ok()) return s;
  auto it = files_.find(path);
  if (it == files_.end()) {
    // A file this Env has not seen before: what is on disk now is durable.
    files_[path] = FileState{size, size, existed};
  }
  *out = std::make_unique<File>(this, path, std::move(file));
  return Status::OK();
}

Status FaultInjectionEnv::NewRandomAccessFile(const std::string& path, std::unique_ptr<RandomAccessFile>* out) {
  std::unique_ptr<RandomAccessFile> file;
  Status s = base_->NewRandomAccessFile(path, &file);
  if (!s.ok()) return s;
  *out = std::make_unique<ReadFile>(this, path, std::move(file));
  return Status::OK();
}

Status FaultInjectionEnv::ReadFileToString(const std::string& path, std::string* out) {
  {
    std::lock_guard lock(mu_);
    if (ShouldFail(Op::kRead, path)) return Fail(Op::kRead, path);
  }
  return base_->ReadFileToString(path, out);
}

Status FaultInjectionEnv::GetChildren(const std::string& dir, std::vector<std::string>* names) {
  Status s = base_->GetChildren(dir, names);
  std::erase(*names, std::string(kTrashDir));
  return s;
}

Status FaultInjectionEnv::RemoveFile(const std::string& path) {
  std::lock_guard lock(mu_);
  if (!active_) return Status::OK();
  if (ShouldFail(Op::kRemove, path)) return Fail(Op::kRemove, path);
  if (!base_->FileExists(path)) return Status::IOError("remove: no such file: " + path);
  // Park the file instead of deleting it, so an unsynced removal can be
  // undone by a power loss.
  PendingDirOp op;
  op.is_rename = false;
  op.from = path;
  auto it = files_.find(path);
  if (it != files_.end()) {
    op.has_state = true;
    op.state = it->second;
    files_.erase(it);
  } else {
    op.has_state = true;
    op.state = FileState{0, 0, true};
    uint64_t size = 0;
    base_->GetFileSize(path, &size);
    op.state.size = op.state.synced = size;
  }
  op.parked = Park(path);
  pending_.push_back(std::move(op));
  return Status::OK();
}

Status FaultInjectionEnv::RenameFile(const std::string& from, const std::string& to) {
  std::lock_guard lock(mu_);
  if (!active_) return Status::OK();
  if (ShouldFail(Op::kRename, from)) return Fail(Op::kRename, from);
  PendingDirOp op;
  op.is_rename = true;
  op.from = from;
  op.to = to;
  if (base_->FileExists(to)) {
    // Keep the replaced file so an unsynced rename can be undone.
    auto it = files_.find(to);
    op.has_state = true;
    if (it != files_.end()) {
      op.state = it->second;
      files_.erase(it);
    } else {
      uint64_t size = 0;
      base_->GetFileSize(to, &size);
      op.state = FileState{size, size, true};
    }
    op.parked = Park(to);
  }
  Status s = base_->RenameFile(from, to);
  if (!s.ok()) return s;
  auto it = files_.find(from);
  if (it != files_.end()) {
    op.moved_tracked = true;
    op.moved_state = it->second;
    files_[to] = it->second;
    files_.erase(it);
  }
  pending_.push_back(std::move(op));
  return Status::OK();
}

Status FaultInjectionEnv::CreateDirIfMissing(const std::string& dir, bool* created) {
  {
    std::lock_guard lock(mu_);
    if (!active_) {
      *created = false;
      return Status::OK();
    }
  }
  return base_->CreateDirIfMissing(dir, created);
}

Status FaultInjectionEnv::SyncDir(const std::string& dir) {
  std::lock_guard lock(mu_);
  if (!active_) return Status::OK();
  if (ShouldFail(Op::kSyncDir, dir)) return Fail(Op::kSyncDir, dir);
  for (auto& [path, st] : files_) {
    if (DirOf(path) == dir) st.entry_durable = true;
  }
  // Renames and removals in this directory are now permanent.
  std::vector<PendingDirOp> keep;
  for (PendingDirOp& op : pending_) {
    if (DirOf(op.from) == dir) {
      if (!op.parked.empty()) base_->RemoveFile(op.parked);
    } else {
      keep.push_back(std::move(op));
    }
  }
  pending_ = std::move(keep);
  return Status::OK();
}

Status FaultInjectionEnv::TruncateFile(const std::string& path, uint64_t size) {
  std::lock_guard lock(mu_);
  if (!active_) return Status::OK();
  Status s = base_->TruncateFile(path, size);
  if (!s.ok()) return s;
  FileState& st = files_[path];
  st.size = st.synced = size;
  return Status::OK();
}

void FaultInjectionEnv::SimulatePowerLoss(bool torn) {
  std::lock_guard lock(mu_);

  // 1. Undo unsynced renames and removals, newest first.
  for (auto op = pending_.rbegin(); op != pending_.rend(); ++op) {
    if (op->is_rename) {
      if (base_->FileExists(op->to)) {
        base_->RenameFile(op->to, op->from);
        auto it = files_.find(op->to);
        if (it != files_.end()) {
          files_[op->from] = it->second;
          files_.erase(it);
        }
      }
      if (!op->parked.empty()) {
        base_->RenameFile(op->parked, op->to);
        if (op->has_state) files_[op->to] = op->state;
      }
    } else if (!base_->FileExists(op->from)) {
      base_->RenameFile(op->parked, op->from);
      if (op->has_state) files_[op->from] = op->state;
    } else {
      base_->RemoveFile(op->parked);  // The name was reused; the old file stays gone.
    }
  }
  pending_.clear();

  // 2. Files whose directory entry never became durable vanish. Others lose
  // their unsynced tail, or keep a torn piece of it.
  for (auto it = files_.begin(); it != files_.end();) {
    const std::string& path = it->first;
    FileState& st = it->second;
    if (!base_->FileExists(path)) {
      it = files_.erase(it);
      continue;
    }
    if (!st.entry_durable) {
      base_->RemoveFile(path);
      it = files_.erase(it);
      continue;
    }
    if (st.size > st.synced) {
      uint64_t keep = st.synced;
      if (torn) keep += rng_() % (st.size - st.synced + 1);
      base_->TruncateFile(path, keep);
      if (torn && keep < st.size && rng_() % 2 == 0) {
        // The sector holding the tail was half written: garbage follows.
        std::string garbage(1 + rng_() % 64, '\0');
        for (char& c : garbage) c = static_cast<char>(rng_());
        std::unique_ptr<WritableFile> f;
        if (base_->NewAppendableFile(path, &f).ok()) {
          f->Append(garbage);
          f->Close();
          keep += garbage.size();
        }
      }
      st.size = st.synced = keep;
    }
    ++it;
  }
}

}  // namespace ledgerkv
