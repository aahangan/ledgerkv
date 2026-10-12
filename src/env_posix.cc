#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>

#include "file_util.h"
#include "ledgerkv/env.h"

namespace ledgerkv {
namespace {

class PosixWritableFile final : public WritableFile {
 public:
  PosixWritableFile(std::string path, int fd) : path_(std::move(path)), fd_(fd) {}
  ~PosixWritableFile() override {
    if (fd_ >= 0) ::close(fd_);
  }

  Status Append(std::string_view data) override { return WriteAll(fd_, data.data(), data.size(), path_); }
  Status Sync() override { return SyncFd(fd_, path_); }
  Status Close() override {
    int fd = fd_;
    fd_ = -1;
    if (::close(fd) != 0) return IOErrorFromErrno("close " + path_);
    return Status::OK();
  }

 private:
  const std::string path_;
  int fd_;
};

class PosixRandomAccessFile final : public RandomAccessFile {
 public:
  PosixRandomAccessFile(std::string path, int fd) : path_(std::move(path)), fd_(fd) {}
  ~PosixRandomAccessFile() override { ::close(fd_); }

  Status Read(uint64_t offset, size_t n, std::string* out) const override {
    out->resize(n);
    size_t done = 0;
    while (done < n) {
      ssize_t r = ::pread(fd_, out->data() + done, n - done, static_cast<off_t>(offset + done));
      if (r < 0) {
        if (errno == EINTR) continue;
        return IOErrorFromErrno("pread " + path_);
      }
      if (r == 0) break;
      done += static_cast<size_t>(r);
    }
    out->resize(done);
    return Status::OK();
  }

 private:
  const std::string path_;
  const int fd_;
};

class PosixFileLock final : public FileLock {
 public:
  explicit PosixFileLock(int fd) : fd_(fd) {}
  ~PosixFileLock() override { ::close(fd_); }  // Closing releases the flock.

 private:
  const int fd_;
};

class PosixEnv final : public Env {
 public:
  Status NewWritableFile(const std::string& path, std::unique_ptr<WritableFile>* out) override {
    return Open(path, O_WRONLY | O_CREAT | O_TRUNC, out);
  }

  Status NewAppendableFile(const std::string& path, std::unique_ptr<WritableFile>* out) override {
    return Open(path, O_WRONLY | O_CREAT | O_APPEND, out);
  }

  Status NewRandomAccessFile(const std::string& path, std::unique_ptr<RandomAccessFile>* out) override {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return IOErrorFromErrno("open " + path);
    *out = std::make_unique<PosixRandomAccessFile>(path, fd);
    return Status::OK();
  }

  Status ReadFileToString(const std::string& path, std::string* out) override {
    return ledgerkv::ReadFileToString(path, out);
  }

  bool FileExists(const std::string& path) override { return ::access(path.c_str(), F_OK) == 0; }

  Status GetFileSize(const std::string& path, uint64_t* size) override {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return IOErrorFromErrno("stat " + path);
    *size = static_cast<uint64_t>(st.st_size);
    return Status::OK();
  }

  Status GetChildren(const std::string& dir, std::vector<std::string>* names) override {
    return ListDir(dir, names);
  }

  Status RemoveFile(const std::string& path) override {
    if (::unlink(path.c_str()) != 0) return IOErrorFromErrno("unlink " + path);
    return Status::OK();
  }

  Status RenameFile(const std::string& from, const std::string& to) override {
    if (::rename(from.c_str(), to.c_str()) != 0) return IOErrorFromErrno("rename " + from);
    return Status::OK();
  }

  Status CreateDirIfMissing(const std::string& dir, bool* created) override {
    if (::mkdir(dir.c_str(), 0755) == 0) {
      *created = true;
      return Status::OK();
    }
    if (errno == EEXIST) {
      *created = false;
      return Status::OK();
    }
    return IOErrorFromErrno("mkdir " + dir);
  }

  Status SyncDir(const std::string& dir) override { return ledgerkv::SyncDir(dir); }

  Status TruncateFile(const std::string& path, uint64_t size) override {
    return ledgerkv::TruncateFile(path, size);
  }

  Status LockFile(const std::string& path, std::unique_ptr<FileLock>* lock) override {
    int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return IOErrorFromErrno("open " + path);
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
      Status s = IOErrorFromErrno("lock " + path + " (is another process using this database?)");
      ::close(fd);
      return s;
    }
    *lock = std::make_unique<PosixFileLock>(fd);
    return Status::OK();
  }

 private:
  static Status Open(const std::string& path, int flags, std::unique_ptr<WritableFile>* out) {
    int fd = ::open(path.c_str(), flags | O_CLOEXEC, 0644);
    if (fd < 0) return IOErrorFromErrno("open " + path);
    *out = std::make_unique<PosixWritableFile>(path, fd);
    return Status::OK();
  }
};

}  // namespace

Env* Env::Default() {
  static PosixEnv env;
  return &env;
}

}  // namespace ledgerkv
