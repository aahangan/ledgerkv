#include "file_util.h"

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace ledgerkv {

Status IOErrorFromErrno(const std::string& context) {
  return Status::IOError(context + ": " + std::strerror(errno));
}

Status WriteAll(int fd, const char* data, size_t n, const std::string& path) {
  while (n > 0) {
    ssize_t w = ::write(fd, data, n);
    if (w < 0) {
      if (errno == EINTR) continue;
      return IOErrorFromErrno("write " + path);
    }
    data += w;
    n -= static_cast<size_t>(w);
  }
  return Status::OK();
}

Status SyncFd(int fd, const std::string& path) {
#ifdef __APPLE__
  if (::fcntl(fd, F_FULLFSYNC) == 0) return Status::OK();
  // Some filesystems (e.g. network mounts) do not support F_FULLFSYNC.
#endif
  if (::fsync(fd) != 0) return IOErrorFromErrno("fsync " + path);
  return Status::OK();
}

Status SyncDir(const std::string& dir) {
  int fd = ::open(dir.c_str(), O_RDONLY);
  if (fd < 0) return IOErrorFromErrno("open dir " + dir);
  Status s = SyncFd(fd, dir);
  ::close(fd);
  return s;
}

Status ReadFileToString(const std::string& path, std::string* out) {
  out->clear();
  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return IOErrorFromErrno("open " + path);
  char buf[1 << 16];
  while (true) {
    ssize_t r = ::read(fd, buf, sizeof(buf));
    if (r < 0) {
      if (errno == EINTR) continue;
      Status s = IOErrorFromErrno("read " + path);
      ::close(fd);
      return s;
    }
    if (r == 0) break;
    out->append(buf, static_cast<size_t>(r));
  }
  ::close(fd);
  return Status::OK();
}

}  // namespace ledgerkv

namespace ledgerkv {

Status ListDir(const std::string& dir, std::vector<std::string>* names) {
  names->clear();
  DIR* d = ::opendir(dir.c_str());
  if (d == nullptr) return IOErrorFromErrno("opendir " + dir);
  while (struct dirent* e = ::readdir(d)) {
    std::string name = e->d_name;
    if (name != "." && name != "..") names->push_back(std::move(name));
  }
  ::closedir(d);
  return Status::OK();
}

Status TruncateFile(const std::string& path, uint64_t size) {
  int fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
  if (fd < 0) return IOErrorFromErrno("open " + path);
  Status s;
  if (::ftruncate(fd, static_cast<off_t>(size)) != 0) {
    s = IOErrorFromErrno("truncate " + path);
  } else {
    s = SyncFd(fd, path);
  }
  ::close(fd);
  return s;
}

}  // namespace ledgerkv
