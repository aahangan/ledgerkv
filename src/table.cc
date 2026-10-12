#include "table.h"

#include <algorithm>

#include "bloom.h"
#include "coding.h"
#include "crc32c.h"
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

Status Table::ReadBlock(uint64_t offset, uint64_t size, std::string* out) const {
  Status s = file_->Read(offset, size + kBlockTrailerSize, out);
  if (!s.ok()) return s;
  if (out->size() != size + kBlockTrailerSize) return Status::Corruption("truncated block in " + path_);
  uint32_t expected = DecodeFixed32(out->data() + size);
  if (crc32c::Value(out->data(), size) != expected) {
    return Status::Corruption("block checksum mismatch in " + path_);
  }
  out->resize(size);
  return Status::OK();
}

Status Table::Open(const std::string& path, uint64_t file_size, std::shared_ptr<Table>* out, Env* env) {
  std::unique_ptr<RandomAccessFile> file;
  Status fs = env->NewRandomAccessFile(path, &file);
  if (!fs.ok()) return fs;
  std::shared_ptr<Table> t(new Table(path, std::move(file)));
  if (file_size < kFooterSize) return Status::Corruption("table too small: " + path);

  std::string footer;
  fs = t->file_->Read(file_size - kFooterSize, kFooterSize, &footer);
  if (!fs.ok()) return fs;
  if (footer.size() != kFooterSize) return Status::Corruption("cannot read footer of " + path);
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
    if (!GetLengthPrefixed(&in, &last_key) || in.size() < 8) return Status::Corruption("bad index block in " + path);
    e.last_seq = TagSequence(DecodeFixed64(in.data()));
    in.remove_prefix(8);
    if (!GetVarint64(&in, &e.offset) || !GetVarint64(&in, &e.size)) {
      return Status::Corruption("bad index block in " + path);
    }
    e.last_key.assign(last_key);
    t->index_.push_back(std::move(e));
  }
  *out = std::move(t);
  return Status::OK();
}

Status Table::Get(std::string_view key, uint64_t snapshot, std::string* value, LookupResult* result) const {
  *result = LookupResult::kMissing;
  if (!BloomMayContain(filter_, key)) return Status::OK();

  // The first entry at or after (key, snapshot) is the newest version of key
  // visible at snapshot, if any version is.
  Iterator it(this);
  it.SeekInternal(key, snapshot);
  if (it.Valid() && it.key() == key) {
    if (it.type() == RecordType::kDelete) {
      *result = LookupResult::kDeleted;
    } else {
      value->assign(it.value());
      *result = LookupResult::kFound;
    }
  }
  return it.status();
}

bool Table::Iterator::LoadBlock(size_t index) {
  block_index_ = index;
  if (index >= table_->index_.size()) return false;
  const IndexEntry& ie = table_->index_[index];
  status_ = table_->ReadBlock(ie.offset, ie.size, &block_);
  if (!status_.ok()) return false;
  rest_ = block_;
  return true;
}

void Table::Iterator::ParseNextEntry() {
  while (rest_.empty()) {
    if (!LoadBlock(block_index_ + 1)) {
      valid_ = false;
      return;
    }
  }
  Entry e;
  if (!ParseEntry(&rest_, &e)) {
    status_ = Status::Corruption("bad data block in " + table_->path_);
    valid_ = false;
    return;
  }
  key_ = e.key;
  seq_ = TagSequence(e.tag);
  type_ = TagType(e.tag);
  value_ = e.value;
  valid_ = true;
}

void Table::Iterator::SeekToFirst() {
  status_ = Status::OK();
  valid_ = false;
  if (!LoadBlock(0)) return;
  ParseNextEntry();
}

void Table::Iterator::SeekInternal(std::string_view target, uint64_t seq) {
  status_ = Status::OK();
  valid_ = false;
  // The first block whose last entry is >= the target holds the first entry
  // >= the target.
  const auto& index = table_->index_;
  auto it = std::lower_bound(index.begin(), index.end(), 0, [&](const IndexEntry& e, int) {
    return CompareInternal(e.last_key, e.last_seq, target, seq) < 0;
  });
  if (it == index.end() || !LoadBlock(static_cast<size_t>(it - index.begin()))) return;
  do {
    ParseNextEntry();
  } while (valid_ && CompareInternal(key_, seq_, target, seq) < 0);
}

Status Table::ForEach(const Visitor& fn) const {
  Iterator it(this);
  for (it.SeekToFirst(); it.Valid(); it.Next()) {
    if (!fn(it.key(), it.seq(), it.type(), it.value())) break;
  }
  return it.status();
}

}  // namespace ledgerkv
