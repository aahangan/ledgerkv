#include "table_builder.h"

#include <fcntl.h>
#include <unistd.h>

#include <cassert>

#include "coding.h"
#include "crc32c.h"
#include "file_util.h"
#include "table_format.h"

namespace ledgerkv {

TableBuilder::TableBuilder(const Options& options, std::string path)
    : options_(options), path_(std::move(path)), bloom_(options.bloom_bits_per_key) {}

TableBuilder::~TableBuilder() {
  if (fd_ >= 0) ::close(fd_);
}

Status TableBuilder::Open() {
  fd_ = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd_ < 0) return IOErrorFromErrno("open " + path_);
  return Status::OK();
}

void TableBuilder::Add(std::string_view key, uint64_t seq, RecordType type, std::string_view value) {
  assert(num_entries_ == 0 || key > std::string_view(last_key_));
  if (!status_.ok()) return;
  if (num_entries_ == 0) smallest_.assign(key);

  PutLengthPrefixed(&data_block_, key);
  PutFixed64(&data_block_, PackTag(seq, type));
  PutLengthPrefixed(&data_block_, value);
  bloom_.AddKey(key);
  last_key_.assign(key);
  num_entries_++;

  if (data_block_.size() >= options_.block_size) status_ = FlushDataBlock();
}

Status TableBuilder::WriteBlock(const std::string& contents, uint64_t* offset, uint64_t* size) {
  std::string trailer;
  PutFixed32(&trailer, crc32c::Value(contents.data(), contents.size()));
  Status s = WriteAll(fd_, contents.data(), contents.size(), path_);
  if (s.ok()) s = WriteAll(fd_, trailer.data(), trailer.size(), path_);
  if (!s.ok()) return s;
  *offset = offset_;
  *size = contents.size();
  offset_ += contents.size() + trailer.size();
  return Status::OK();
}

Status TableBuilder::FlushDataBlock() {
  if (data_block_.empty()) return Status::OK();
  uint64_t offset, size;
  Status s = WriteBlock(data_block_, &offset, &size);
  if (!s.ok()) return s;
  PutLengthPrefixed(&index_block_, last_key_);
  PutVarint64(&index_block_, offset);
  PutVarint64(&index_block_, size);
  data_block_.clear();
  return Status::OK();
}

Status TableBuilder::Finish() {
  if (status_.ok()) status_ = FlushDataBlock();
  if (!status_.ok()) return status_;

  uint64_t filter_offset, filter_size, index_offset, index_size;
  Status s = WriteBlock(bloom_.Finish(), &filter_offset, &filter_size);
  if (s.ok()) s = WriteBlock(index_block_, &index_offset, &index_size);
  if (!s.ok()) return status_ = s;

  std::string footer;
  PutFixed64(&footer, filter_offset);
  PutFixed64(&footer, filter_size);
  PutFixed64(&footer, index_offset);
  PutFixed64(&footer, index_size);
  PutFixed64(&footer, kTableMagic);
  s = WriteAll(fd_, footer.data(), footer.size(), path_);
  if (s.ok()) {
    offset_ += footer.size();
    s = SyncFd(fd_, path_);
  }
  if (s.ok() && ::close(fd_) != 0) s = IOErrorFromErrno("close " + path_);
  fd_ = -1;
  return status_ = s;
}

void TableBuilder::Abandon() {
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
  ::unlink(path_.c_str());
}

}  // namespace ledgerkv
