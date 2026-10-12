// Fuzzes the table reader: opening, scanning, and point lookups must either
// work or fail with Corruption/IOError. Never crash, hang, or read out of
// bounds.
//
// Two modes, picked by the first byte:
//   raw:          the input is the whole file. This exercises the footer and
//                 checksum checks, but random bytes almost never carry a
//                 valid block checksum, so block contents are rarely parsed.
//   checksummed:  the input supplies the *contents* of data, filter, and
//                 index blocks, and the harness wraps them in a well-formed
//                 file with correct checksums and footer. This gets fuzzed
//                 bytes past the checksums and into the entry and index
//                 parsers.

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "coding.h"
#include "crc32c.h"
#include "fuzz.h"
#include "ledgerkv/env.h"
#include "table.h"
#include "table_format.h"

using namespace ledgerkv;

std::vector<std::string> FuzzSeeds() { return fuzz::TableSeeds(); }

namespace {

// Splits input into the contents of up to four data blocks, a filter block,
// and an index block whose entries point at the real data blocks but take
// their keys from the input, then assembles a checksummed table.
std::string BuildChecksummedTable(std::string_view in) {
  auto take = [&](size_t max) {
    size_t n = in.empty() ? 0 : std::min<size_t>(static_cast<uint8_t>(in[0]) % (max + 1), in.size() - 1);
    std::string_view piece = in.empty() ? std::string_view() : in.substr(1, n);
    in.remove_prefix(in.empty() ? 0 : 1 + n);
    return piece;
  };
  std::string file, index;
  auto add_block = [&](std::string_view contents) {
    uint64_t offset = file.size();
    file.append(contents);
    PutFixed32(&file, crc32c::Value(contents.data(), contents.size()));
    return offset;
  };
  int blocks = in.empty() ? 1 : 1 + static_cast<uint8_t>(in[0]) % 4;
  for (int i = 0; i < blocks; i++) {
    std::string_view contents = take(200);
    uint64_t offset = add_block(contents);
    PutLengthPrefixed(&index, take(16));                         // last key
    PutFixed64(&index, (static_cast<uint64_t>(i) << 8) | 1);     // last tag
    PutVarint64(&index, offset);
    PutVarint64(&index, contents.size());
  }
  std::string_view filter = take(64);
  uint64_t filter_offset = add_block(filter);
  // Sometimes use fuzzed bytes as the raw index instead.
  std::string raw_index(in);
  const std::string& index_contents = (!raw_index.empty() && raw_index[0] % 3 == 0) ? raw_index : index;
  uint64_t index_offset = add_block(index_contents);
  PutFixed64(&file, filter_offset);
  PutFixed64(&file, filter.size());
  PutFixed64(&file, index_offset);
  PutFixed64(&file, index_contents.size());
  PutFixed64(&file, kTableMagic);
  return file;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  static const std::string path = fuzz::ScratchPath("input.sst");
  Env* env = Env::Default();
  std::string_view input(reinterpret_cast<const char*>(data), size);
  std::string contents;
  if (!input.empty() && input[0] % 2 == 1) {
    contents = BuildChecksummedTable(input.substr(1));
  } else {
    contents.assign(input);
  }
  {
    std::unique_ptr<WritableFile> f;
    FUZZ_CHECK(env->NewWritableFile(path, &f).ok());
    FUZZ_CHECK(f->Append(contents).ok());
    FUZZ_CHECK(f->Close().ok());
  }

  std::shared_ptr<Table> table;
  Status s = Table::Open(path, contents.size(), &table);
  if (!s.ok()) {
    FUZZ_CHECK(s.IsCorruption() || s.IsIOError());
    return 0;
  }

  std::vector<std::string> keys;
  Table::Iterator it(table.get());
  for (it.SeekToFirst(); it.Valid() && keys.size() < 2000; it.Next()) keys.emplace_back(it.key());
  FUZZ_CHECK(it.status().ok() || it.status().IsCorruption());

  for (const std::string& key : keys) {
    std::string value;
    LookupResult result;
    Status gs = table->Get(key, kMaxSequence, &value, &result);
    FUZZ_CHECK(gs.ok() || gs.IsCorruption());
    Table::Iterator seek(table.get());
    seek.Seek(key);
    FUZZ_CHECK(seek.status().ok() || seek.status().IsCorruption());
  }
  std::string value;
  LookupResult result;
  Status gs = table->Get("absent-key", 5, &value, &result);
  FUZZ_CHECK(gs.ok() || gs.IsCorruption());
  return 0;
}
