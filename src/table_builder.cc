#include "table_builder.h"

#include <cassert>

#include "coding.h"
#include "crc32c.h"
#include "table_format.h"

namespace ledgerkv {

TableBuilder::TableBuilder(const Options& options, std::string path)
    : options_(options), path_(std::move(path)), bloom_(options.bloom_bits_per_key) {}

Status TableBuilder::Open() { return options_.env->NewWritableFile(path_, &file_); }

void TableBuilder::Add(std::string_view key, uint64_t seq, RecordType type, std::string_view value) {
  assert(num_entries_ == 0 || CompareInternal(key, seq, last_key_, last_seq_) > 0);
  if (!status_.ok()) return;
  if (num_entries_ == 0) smallest_.assign(key);

  PutLengthPrefixed(&data_block_, key);
  PutFixed64(&data_block_, PackTag(seq, type));
  PutLengthPrefixed(&data_block_, value);
  // Versions of a key are adjacent, so this adds each user key once.
  if (num_entries_ == 0 || key != std::string_view(last_key_)) bloom_.AddKey(key);
  last_key_.assign(key);
  last_seq_ = seq;
  num_entries_++;

  if (data_block_.size() >= options_.block_size) status_ = FlushDataBlock();
}

Status TableBuilder::WriteBlock(const std::string& contents, uint64_t* offset, uint64_t* size) {
  std::string trailer;
  PutFixed32(&trailer, crc32c::Value(contents.data(), contents.size()));
  Status s = file_->Append(contents);
  if (s.ok()) s = file_->Append(trailer);
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
  PutFixed64(&index_block_, PackTag(last_seq_, RecordType::kPut));
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
  s = file_->Append(footer);
  if (s.ok()) {
    offset_ += footer.size();
    s = file_->Sync();
  }
  if (s.ok()) s = file_->Close();
  file_.reset();
  return status_ = s;
}

void TableBuilder::Abandon() {
  file_.reset();
  options_.env->RemoveFile(path_);  // Best effort; a leftover is an orphan, removed on the next open.
}

}  // namespace ledgerkv
