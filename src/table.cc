#include "table.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>

#include "bloom.h"
#include "coding.h"
#include "crc32c.h"
#include "file_util.h"
#include "table_format.h"

namespace ledgerkv {
namespace {

struct Entry {
  std::string_view key;
  uint64_t tag;
  std::string_view value;
};

bool ParseEntry(std::string_view* in, Entry* e) {
  if (!GetLengthPrefixed(in, &e->key) || in->size() < 8) return false;
  e->tag = DecodeFixed64(in->data());
  in->remove_prefix(8);
  return GetLengthPrefixed(in, &e->value);
}

}  // namespace

Table::~Table() { ::close(fd_); }

Status Table::ReadBlock(uint64_t offset, uint64_t size, std::string* out) const {
  out->resize(size + kBlockTrailerSize);
  size_t done = 0;
  while (done < out->size()) {
    ssize_t r = ::pread(fd_, out->data() + done, out->size() - done, static_cast<off_t>(offset + done));
    if (r < 0) {
      if (errno == EINTR) continue;
      return IOErrorFromErrno("pread " + path_);
    }
    if (r == 0) return Status::Corruption("truncated block in " + path_);
    done += static_cast<size_t>(r);
  }
  uint32_t expected = DecodeFixed32(out->data() + size);
  if (crc32c::Value(out->data(), size) != expected) {
    return Status::Corruption("block checksum mismatch in " + path_);
  }
  out->resize(size);
  return Status::OK();
}

Status Table::Open(const std::string& path, uint64_t file_size, std::shared_ptr<Table>* out) {
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return IOErrorFromErrno("open " + path);
  std::shared_ptr<Table> t(new Table(path, fd));
  if (file_size < kFooterSize) return Status::Corruption("table too small: " + path);

  std::string footer(kFooterSize, '\0');
  if (::pread(fd, footer.data(), kFooterSize, static_cast<off_t>(file_size - kFooterSize)) !=
      static_cast<ssize_t>(kFooterSize)) {
    return Status::Corruption("cannot read footer of " + path);
  }
  const char* f = footer.data();
  uint64_t filter_offset = DecodeFixed64(f), filter_size = DecodeFixed64(f + 8);
  uint64_t index_offset = DecodeFixed64(f + 16), index_size = DecodeFixed64(f + 24);
  if (DecodeFixed64(f + 32) != kTableMagic) return Status::Corruption("bad magic in " + path);
  if (index_offset + index_size + kBlockTrailerSize + kFooterSize != file_size ||
      filter_offset + filter_size + kBlockTrailerSize != index_offset) {
    return Status::Corruption("bad footer in " + path);
  }

  Status s = t->ReadBlock(filter_offset, filter_size, &t->filter_);
  if (!s.ok()) return s;
  std::string index;
  s = t->ReadBlock(index_offset, index_size, &index);
  if (!s.ok()) return s;

  std::string_view in = index;
  while (!in.empty()) {
    std::string_view last_key;
    IndexEntry e;
    if (!GetLengthPrefixed(&in, &last_key) || !GetVarint64(&in, &e.offset) || !GetVarint64(&in, &e.size)) {
      return Status::Corruption("bad index block in " + path);
    }
    e.last_key.assign(last_key);
    t->index_.push_back(std::move(e));
  }
  *out = std::move(t);
  return Status::OK();
}

Status Table::Get(std::string_view key, std::string* value, LookupResult* result) const {
  *result = LookupResult::kMissing;
  if (!BloomMayContain(filter_, key)) return Status::OK();

  // The first block whose last key is >= key is the only one that can hold it.
  auto it = std::lower_bound(index_.begin(), index_.end(), key,
                             [](const IndexEntry& e, std::string_view k) { return e.last_key < k; });
  if (it == index_.end()) return Status::OK();

  std::string block;
  Status s = ReadBlock(it->offset, it->size, &block);
  if (!s.ok()) return s;
  std::string_view in = block;
  while (!in.empty()) {
    Entry e;
    if (!ParseEntry(&in, &e)) return Status::Corruption("bad data block in " + path_);
    if (e.key < key) continue;
    if (e.key == key) {
      if (TagType(e.tag) == RecordType::kDelete) {
        *result = LookupResult::kDeleted;
      } else {
        value->assign(e.value);
        *result = LookupResult::kFound;
      }
    }
    break;
  }
  return Status::OK();
}

Status Table::ForEach(const Visitor& fn) const {
  std::string block;
  for (const IndexEntry& ie : index_) {
    Status s = ReadBlock(ie.offset, ie.size, &block);
    if (!s.ok()) return s;
    std::string_view in = block;
    while (!in.empty()) {
      Entry e;
      if (!ParseEntry(&in, &e)) return Status::Corruption("bad data block in " + path_);
      if (!fn(e.key, TagSequence(e.tag), TagType(e.tag), e.value)) return Status::OK();
    }
  }
  return Status::OK();
}

}  // namespace ledgerkv
