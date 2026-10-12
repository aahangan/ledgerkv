#include "version_edit.h"

#include "coding.h"

namespace ledgerkv {
namespace {

enum Tag : uint32_t {
  kLogNumber = 1,
  kNextFileNumber = 2,
  kLastSequence = 3,
  kAddedTable = 4,
  kDeletedTable = 5,
};

}  // namespace

void VersionEdit::EncodeTo(std::string* dst) const {
  if (log_number) {
    PutVarint32(dst, kLogNumber);
    PutVarint64(dst, *log_number);
  }
  if (next_file_number) {
    PutVarint32(dst, kNextFileNumber);
    PutVarint64(dst, *next_file_number);
  }
  if (last_sequence) {
    PutVarint32(dst, kLastSequence);
    PutVarint64(dst, *last_sequence);
  }
  for (const TableMeta& t : added_tables) {
    PutVarint32(dst, kAddedTable);
    PutVarint64(dst, t.number);
    PutVarint64(dst, t.file_size);
    PutLengthPrefixed(dst, t.smallest);
    PutLengthPrefixed(dst, t.largest);
  }
  for (uint64_t number : deleted_tables) {
    PutVarint32(dst, kDeletedTable);
    PutVarint64(dst, number);
  }
}

bool VersionEdit::DecodeFrom(std::string_view in) {
  *this = VersionEdit();
  while (!in.empty()) {
    uint32_t tag;
    uint64_t v;
    if (!GetVarint32(&in, &tag)) return false;
    switch (tag) {
      case kLogNumber:
        if (!GetVarint64(&in, &v)) return false;
        log_number = v;
        break;
      case kNextFileNumber:
        if (!GetVarint64(&in, &v)) return false;
        next_file_number = v;
        break;
      case kLastSequence:
        if (!GetVarint64(&in, &v)) return false;
        last_sequence = v;
        break;
      case kAddedTable: {
        TableMeta t;
        std::string_view smallest, largest;
        if (!GetVarint64(&in, &t.number) || !GetVarint64(&in, &t.file_size) ||
            !GetLengthPrefixed(&in, &smallest) || !GetLengthPrefixed(&in, &largest)) {
          return false;
        }
        t.smallest.assign(smallest);
        t.largest.assign(largest);
        added_tables.push_back(std::move(t));
        break;
      }
      case kDeletedTable:
        if (!GetVarint64(&in, &v)) return false;
        deleted_tables.push_back(v);
        break;
      default:
        return false;
    }
  }
  return true;
}

}  // namespace ledgerkv
