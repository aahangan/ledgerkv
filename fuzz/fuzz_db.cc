// Differential fuzzing of the whole database against a std::map model. The
// input is decoded into a sequence of operations: puts, deletes, batches,
// gets, flushes, full compactions, snapshots (checked later, after more
// writes and compactions), iterator scans and seeks, and reopens. Tiny
// memtable and level sizes make flushes and compactions frequent, and a key
// space of 24 keys makes overwrites, multiple versions, and tombstones
// common. Every read must match the model exactly, and Verify() must pass.

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "fault_env.h"
#include "fuzz.h"
#include "ledgerkv/db.h"

using namespace ledgerkv;

std::vector<std::string> FuzzSeeds() { return fuzz::DBSeeds(); }

namespace {

using Model = std::map<std::string, std::string>;

class Input {
 public:
  Input(const uint8_t* data, size_t size) : data_(data), size_(size) {}
  bool empty() const { return pos_ >= size_; }
  uint8_t Next() { return pos_ < size_ ? data_[pos_++] : 0; }

 private:
  const uint8_t* data_;
  size_t size_;
  size_t pos_ = 0;
};

std::string Key(uint8_t b) { return "key" + std::to_string(b % 24); }
std::string Value(uint8_t len, int op) { return std::to_string(op) + std::string(len % 48, 'v'); }

void CheckGet(DB* db, const ReadOptions& ro, const Model& model, const std::string& key) {
  std::string v;
  Status s = db->Get(ro, key, &v);
  auto it = model.find(key);
  if (it == model.end()) {
    FUZZ_CHECK(s.IsNotFound());
  } else {
    FUZZ_CHECK(s.ok());
    FUZZ_CHECK(v == it->second);
  }
}

void CheckScan(DB* db, const ReadOptions& ro, const Model& model) {
  auto it = db->NewIterator(ro);
  auto want = model.begin();
  for (it->SeekToFirst(); it->Valid(); it->Next(), ++want) {
    FUZZ_CHECK(want != model.end());
    FUZZ_CHECK(it->key() == want->first);
    FUZZ_CHECK(it->value() == want->second);
  }
  FUZZ_CHECK(it->status().ok());
  FUZZ_CHECK(want == model.end());
}

void CheckSeek(DB* db, const Model& model, const std::string& target) {
  auto it = db->NewIterator();
  it->Seek(target);
  auto want = model.lower_bound(target);
  FUZZ_CHECK(it->Valid() == (want != model.end()));
  if (it->Valid()) FUZZ_CHECK(it->key() == want->first);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  static const std::string dir = fuzz::ScratchPath("db");
  std::filesystem::remove_all(dir);
  FaultInjectionEnv env;  // Skips real fsyncs, which keeps each input fast.
  Options options;
  options.env = &env;
  options.sync = false;
  options.write_buffer_size = 2 << 10;
  options.max_bytes_for_level_base = 8 << 10;
  options.target_file_size = 2 << 10;
  options.block_size = 256;

  std::unique_ptr<DB> db;
  FUZZ_CHECK(DB::Open(options, dir, &db).ok());
  Model model;
  struct Snap {
    const Snapshot* snapshot;
    Model model;
  };
  std::vector<Snap> snaps;

  Input in(data, size);
  for (int op = 0; op < 300 && !in.empty(); op++) {
    switch (in.Next() % 10) {
      case 0: {
        std::string k = Key(in.Next()), v = Value(in.Next(), op);
        FUZZ_CHECK(db->Put(k, v).ok());
        model[k] = v;
        break;
      }
      case 1: {
        std::string k = Key(in.Next());
        FUZZ_CHECK(db->Delete(k).ok());
        model.erase(k);
        break;
      }
      case 2:
        CheckGet(db.get(), ReadOptions(), model, Key(in.Next()));
        break;
      case 3: {
        WriteBatch batch;
        int n = 1 + in.Next() % 4;
        for (int i = 0; i < n; i++) {
          std::string k = Key(in.Next());
          if (in.Next() % 3 == 0) {
            batch.Delete(k);
            model.erase(k);
          } else {
            std::string v = Value(in.Next(), op);
            batch.Put(k, v);
            model[k] = v;
          }
        }
        FUZZ_CHECK(db->Write(batch).ok());
        break;
      }
      case 4:
        FUZZ_CHECK(db->Flush().ok());
        break;
      case 5:
        FUZZ_CHECK(db->CompactAll().ok());
        break;
      case 6:
        if (snaps.size() < 3) snaps.push_back({db->GetSnapshot(), model});
        break;
      case 7:
        if (!snaps.empty()) {
          size_t i = in.Next() % snaps.size();
          ReadOptions ro;
          ro.snapshot = snaps[i].snapshot;
          for (int k = 0; k < 24; k++) CheckGet(db.get(), ro, snaps[i].model, Key(static_cast<uint8_t>(k)));
          CheckScan(db.get(), ro, snaps[i].model);
          if (in.Next() % 2) {
            db->ReleaseSnapshot(snaps[i].snapshot);
            snaps.erase(snaps.begin() + static_cast<long>(i));
          }
        }
        break;
      case 8:
        CheckScan(db.get(), ReadOptions(), model);
        CheckSeek(db.get(), model, Key(in.Next()) + (in.Next() % 2 ? "" : "~"));
        break;
      case 9:
        for (auto& s : snaps) db->ReleaseSnapshot(s.snapshot);
        snaps.clear();
        db.reset();
        FUZZ_CHECK(DB::Open(options, dir, &db).ok());
        break;
    }
  }
  CheckScan(db.get(), ReadOptions(), model);
  FUZZ_CHECK(db->Verify().ok());
  for (auto& s : snaps) db->ReleaseSnapshot(s.snapshot);
  db.reset();
  std::filesystem::remove_all(dir);
  return 0;
}
