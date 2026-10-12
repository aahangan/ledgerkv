#include <unistd.h>

#include <filesystem>

#include "fuzz.h"
#include "ledgerkv/env.h"
#include "ledgerkv/write_batch.h"
#include "log.h"
#include "table_builder.h"
#include "version_edit.h"
#include "wal.h"
#include "write_batch_internal.h"

namespace ledgerkv::fuzz {

std::string ScratchPath(const std::string& name) {
  return (std::filesystem::temp_directory_path() / ("ledgerkv-fuzz-" + std::to_string(::getpid()) + "-" + name))
      .string();
}

std::vector<std::string> LogSeeds() {
  std::string wal;
  for (uint64_t i = 1; i <= 5; i++) {
    std::string payload;
    EncodeWalPayload(i % 3 ? RecordType::kPut : RecordType::kDelete, i, "key" + std::to_string(i),
                     std::string(i * 3, 'v'), &payload);
    AppendLogRecord(payload, &wal);
  }
  WriteBatch b;
  b.Put("a", "1");
  b.Delete("b");
  std::string payload;
  EncodeWalBatchPayload(10, WriteBatchInternal::Contents(b), &payload);
  AppendLogRecord(payload, &wal);

  std::string manifest;
  VersionEdit e;
  e.log_number = 7;
  e.next_file_number = 9;
  e.last_sequence = 100;
  e.added_tables.push_back({0, 5, 4096, "aaa", "zzz"});
  e.added_tables.push_back({2, 6, 100, "b", "c"});
  e.deleted_tables = {3};
  std::string record;
  e.EncodeTo(&record);
  AppendLogRecord(record, &manifest);
  return {wal, manifest, ""};
}

std::vector<std::string> WriteBatchSeeds() {
  WriteBatch a, b;
  a.Put("key", "value");
  a.Delete("gone");
  a.Put("", "");
  for (int i = 0; i < 20; i++) b.Put("k" + std::to_string(i), std::string(i, 'x'));
  return {std::string(WriteBatchInternal::Contents(a)), std::string(WriteBatchInternal::Contents(b)),
          std::string(WriteBatchInternal::Contents(WriteBatch()))};
}

std::vector<std::string> TableSeeds() {
  std::vector<std::string> seeds;
  for (int n : {1, 30, 400}) {
    std::string path = ScratchPath("seed.sst");
    TableBuilder::Options o;
    o.block_size = 256;  // Small blocks so a short seed still has several.
    TableBuilder builder(o, path);
    if (!builder.Open().ok()) continue;
    for (int i = 0; i < n; i++) {
      char key[16];
      std::snprintf(key, sizeof(key), "k%05d", i);
      // Two versions of every fifth key, as snapshots produce.
      if (i % 5 == 0) builder.Add(key, static_cast<uint64_t>(1000 + i), RecordType::kDelete, "");
      builder.Add(key, static_cast<uint64_t>(i + 1), RecordType::kPut, std::string(i % 40, 'v'));
    }
    if (!builder.Finish().ok()) continue;
    std::string data;
    Env::Default()->ReadFileToString(path, &data);
    Env::Default()->RemoveFile(path);
    seeds.push_back(data);
  }
  return seeds;
}

std::vector<std::string> DBSeeds() {
  // Byte streams decoded by fuzz_db.cc into operations.
  std::string mixed;
  for (int i = 0; i < 64; i++) mixed += static_cast<char>(i * 37);
  std::string puts;
  for (int i = 0; i < 40; i++) puts += std::string{0, static_cast<char>(i), 20};
  puts += std::string{4, 5, 7, 2, 3, 2, 10};
  return {mixed, puts};
}

}  // namespace ledgerkv::fuzz
