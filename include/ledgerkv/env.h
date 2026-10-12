#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ledgerkv/status.h"

namespace ledgerkv {

// A file opened for sequential writing.
class WritableFile {
 public:
  virtual ~WritableFile() = default;

  virtual Status Append(std::string_view data) = 0;
  // Makes everything appended so far durable. Only data that has been
  // synced is guaranteed to survive a power loss.
  virtual Status Sync() = 0;
  virtual Status Close() = 0;
};

// A file opened for positional reads. Read() is safe to call concurrently.
class RandomAccessFile {
 public:
  virtual ~RandomAccessFile() = default;

  // Reads up to n bytes at offset into *out, which is resized to the number
  // of bytes read (fewer than n only at end of file).
  virtual Status Read(uint64_t offset, size_t n, std::string* out) const = 0;
};

// Held while a database directory is open by this process.
class FileLock {
 public:
  virtual ~FileLock() = default;
};

// Every filesystem operation ledgerkv performs goes through an Env. The
// default talks to POSIX. Tests substitute an Env that simulates power loss
// and injects I/O errors (see FaultInjectionEnv).
//
// Durability contract, which the database relies on and the fault-injecting
// Env enforces: appended data survives power loss only once the file is
// synced, and a newly created, renamed, or deleted file's directory entry
// survives only once its directory is synced.
class Env {
 public:
  virtual ~Env() = default;

  // The process-wide POSIX environment.
  static Env* Default();

  // Creates path, truncating it if it exists.
  virtual Status NewWritableFile(const std::string& path, std::unique_ptr<WritableFile>* out) = 0;
  // Opens path for appending, creating it if needed.
  virtual Status NewAppendableFile(const std::string& path, std::unique_ptr<WritableFile>* out) = 0;
  virtual Status NewRandomAccessFile(const std::string& path, std::unique_ptr<RandomAccessFile>* out) = 0;

  virtual Status ReadFileToString(const std::string& path, std::string* out) = 0;
  virtual bool FileExists(const std::string& path) = 0;
  virtual Status GetFileSize(const std::string& path, uint64_t* size) = 0;
  // Lists the entries of dir, excluding "." and "..".
  virtual Status GetChildren(const std::string& dir, std::vector<std::string>* names) = 0;

  virtual Status RemoveFile(const std::string& path) = 0;
  // Atomically replaces to, if it exists.
  virtual Status RenameFile(const std::string& from, const std::string& to) = 0;
  // Sets *created to whether the directory was newly created.
  virtual Status CreateDirIfMissing(const std::string& dir, bool* created) = 0;
  // Makes the directory's entries (creations, renames, deletions) durable.
  virtual Status SyncDir(const std::string& dir) = 0;
  // Truncates path to size bytes and syncs it.
  virtual Status TruncateFile(const std::string& path, uint64_t size) = 0;

  // Takes an exclusive lock on path (creating it), failing if another
  // process holds it.
  virtual Status LockFile(const std::string& path, std::unique_ptr<FileLock>* lock) = 0;
};

}  // namespace ledgerkv
