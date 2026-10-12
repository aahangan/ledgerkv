// Fuzzes WriteBatch decoding. Arbitrary bytes must decode or be rejected
// without crashing, and a decoded batch rebuilt through the public API must
// decode to the same operations.

#include <string>
#include <tuple>
#include <vector>

#include "fuzz.h"
#include "ledgerkv/write_batch.h"
#include "write_batch_internal.h"

using namespace ledgerkv;

std::vector<std::string> FuzzSeeds() { return fuzz::WriteBatchSeeds(); }

using Ops = std::vector<std::tuple<RecordType, std::string, std::string>>;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  std::string_view rep(reinterpret_cast<const char*>(data), size);
  Ops ops;
  bool ok = WriteBatchInternal::Iterate(rep, [&](RecordType type, std::string_view key, std::string_view value) {
    ops.emplace_back(type, std::string(key), std::string(value));
  });
  if (!ok) return 0;

  WriteBatch rebuilt;
  for (const auto& [type, key, value] : ops) {
    if (type == RecordType::kPut) {
      rebuilt.Put(key, value);
    } else {
      rebuilt.Delete(key);
    }
  }
  FUZZ_CHECK(rebuilt.Count() == ops.size());
  Ops again;
  FUZZ_CHECK(WriteBatchInternal::Iterate(WriteBatchInternal::Contents(rebuilt),
                                         [&](RecordType type, std::string_view key, std::string_view value) {
                                           again.emplace_back(type, std::string(key), std::string(value));
                                         }));
  FUZZ_CHECK(ops == again);
  return 0;
}
