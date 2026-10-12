#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#include "ledgerkv/env.h"

namespace ledgerkv {

// An Env for testing that wraps a real one and can (1) simulate power loss
// and (2) inject I/O errors.
//
// Power loss. The Env tracks, for every file written through it, how much
// has been appended and how much has been synced, and whether its directory
// entry has been synced since it was created. Renames and removals are
// recorded until their directory is synced (removed files are parked in a
// hidden ".fault-trash" directory meanwhile). SimulatePowerLoss() then
// rewinds the disk to what a real one could hold after losing power:
//
//   * each file is cut back to its last synced size, optionally keeping a
//     random prefix of the unsynced data plus random garbage (a torn write),
//   * files whose directory entry was never synced disappear, and
//   * renames and removals not followed by a directory sync are undone.
//
// Sync() and SyncDir() only update this ledger; they do not call fsync on
// the real disk, because durability is simulated. That also makes tests fast.
//
// Error injection. InjectFault() makes the n-th and every later matching
// operation fail with IOError (sticky, like a dying disk). A failing Append
// first writes a random prefix of its data, as a real short write would.
//
// Thread-safe.
class FaultInjectionEnv final : public Env {
 public:
  enum class Op { kAppend, kSync, kOpenForWrite, kRename, kRemove, kRead, kSyncDir };

  explicit FaultInjectionEnv(Env* base = Env::Default(), uint64_t seed = 1);

  // While inactive the machine is "off": calls that would change the disk
  // do nothing and report success. Reads still work. Turning the Env
  // inactive freezes the disk at the instant of a simulated power cut while
  // the process finishes tearing down the database.
  void SetActive(bool active);
  bool IsActive();

  // Rewinds the disk to what survives a power cut now. If torn is set,
  // files keep a random amount of their unsynced data, sometimes followed by
  // garbage; otherwise all unsynced data is dropped. Call only while no
  // database is open on the Env.
  void SimulatePowerLoss(bool torn);

  // After `after` more successful matching operations, every operation of
  // kind op on a path containing path_substr fails until ClearFaults().
  void InjectFault(Op op, const std::string& path_substr, int after);
  // Each matching operation fails independently with probability p.
  void InjectRandomFault(Op op, const std::string& path_substr, double p);
  void ClearFaults();
  uint64_t FaultsTriggered();

  // Env interface.
  Status NewWritableFile(const std::string& path, std::unique_ptr<WritableFile>* out) override;
  Status NewAppendableFile(const std::string& path, std::unique_ptr<WritableFile>* out) override;
  Status NewRandomAccessFile(const std::string& path, std::unique_ptr<RandomAccessFile>* out) override;
  Status ReadFileToString(const std::string& path, std::string* out) override;
  bool FileExists(const std::string& path) override { return base_->FileExists(path); }
  Status GetFileSize(const std::string& path, uint64_t* size) override { return base_->GetFileSize(path, size); }
  Status GetChildren(const std::string& dir, std::vector<std::string>* names) override;
  Status RemoveFile(const std::string& path) override;
  Status RenameFile(const std::string& from, const std::string& to) override;
  Status CreateDirIfMissing(const std::string& dir, bool* created) override;
  Status SyncDir(const std::string& dir) override;
  Status TruncateFile(const std::string& path, uint64_t size) override;
  Status LockFile(const std::string& path, std::unique_ptr<FileLock>* lock) override {
    return base_->LockFile(path, lock);
  }

 private:
  class File;
  class ReadFile;
  friend class File;
  friend class ReadFile;

  struct FileState {
    uint64_t size = 0;           // Bytes appended so far.
    uint64_t synced = 0;         // Bytes guaranteed to survive power loss.
    bool entry_durable = false;  // Directory entry synced since creation.
  };

  // A rename or removal not yet made durable by syncing its directory.
  struct PendingDirOp {
    bool is_rename;
    std::string from, to;     // For a removal, from is the removed path.
    std::string parked;       // Where the removed file (or the replaced target) was moved.
    bool has_state = false;   // Ledger entry of the removed or replaced file.
    FileState state;
    FileState moved_state;    // For a rename: ledger entry of the renamed file.
    bool moved_tracked = false;
  };

  // Returns true if this operation should fail. Requires mu_.
  bool ShouldFail(Op op, const std::string& path);
  Status Fail(Op op, const std::string& path);
  std::string Park(const std::string& path);  // Requires mu_.

  Env* const base_;
  std::mutex mu_;
  bool active_ = true;
  std::mt19937_64 rng_;
  std::map<std::string, FileState> files_;
  std::vector<PendingDirOp> pending_;
  uint64_t park_counter_ = 0;

  struct Fault {
    Op op;
    std::string path_substr;
    int remaining = 0;
    double probability = -1;  // >= 0 for random faults.
  };
  std::vector<Fault> faults_;
  uint64_t faults_triggered_ = 0;
};

}  // namespace ledgerkv
