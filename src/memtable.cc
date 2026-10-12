#include "memtable.h"

#include <cstring>

#include "coding.h"

namespace ledgerkv {
namespace {

// Entries come from our own arena, so their varints are trusted.
const char* DecodeVarint32(const char* p, uint32_t* v) {
  uint32_t result = 0;
  for (int shift = 0; shift <= 28; shift += 7) {
    uint32_t byte = static_cast<uint8_t>(*p++);
    result |= (byte & 0x7F) << shift;
    if (!(byte & 0x80)) break;
  }
  *v = result;
  return p;
}

struct Decoded {
  std::string_view key;
  uint64_t tag;
  std::string_view value;
};

Decoded DecodeEntry(const char* p) {
  Decoded d;
  uint32_t klen, vlen;
  p = DecodeVarint32(p, &klen);
  d.key = std::string_view(p, klen);
  p += klen;
  d.tag = DecodeFixed64(p);
  p += 8;
  p = DecodeVarint32(p, &vlen);
  d.value = std::string_view(p, vlen);
  return d;
}

// Encodes a search key: same layout as an entry, with an empty value.
std::string EncodeLookup(std::string_view key, uint64_t seq) {
  std::string buf;
  PutLengthPrefixed(&buf, key);
  PutFixed64(&buf, PackTag(seq, RecordType::kPut));
  PutVarint32(&buf, 0);
  return buf;
}

}  // namespace

// The comparator runs O(log n) times per insert and lookup, so it decodes
// only the key and tag, not the value.
inline void DecodeKeyAndSeq(const char* p, std::string_view* key, uint64_t* seq) {
  uint32_t klen;
  p = DecodeVarint32(p, &klen);
  *key = std::string_view(p, klen);
  *seq = TagSequence(DecodeFixed64(p + klen));
}

int MemTable::KeyComparator::operator()(const char* a, const char* b) const {
  std::string_view ka, kb;
  uint64_t sa, sb;
  DecodeKeyAndSeq(a, &ka, &sa);
  DecodeKeyAndSeq(b, &kb, &sb);
  return CompareInternal(ka, sa, kb, sb);
}

namespace {

char* EncodeVarint32(char* p, uint32_t v) {
  while (v >= 0x80) {
    *p++ = static_cast<char>((v & 0x7F) | 0x80);
    v >>= 7;
  }
  *p++ = static_cast<char>(v);
  return p;
}

size_t VarintLength(uint32_t v) {
  size_t n = 1;
  while (v >= 0x80) {
    v >>= 7;
    n++;
  }
  return n;
}

}  // namespace

void MemTable::Add(uint64_t seq, RecordType type, std::string_view key, std::string_view value) {
  const auto klen = static_cast<uint32_t>(key.size());
  const auto vlen = static_cast<uint32_t>(value.size());
  const size_t size = VarintLength(klen) + klen + 8 + VarintLength(vlen) + vlen;
  char* buf = arena_.Allocate(size);
  char* p = EncodeVarint32(buf, klen);
  std::memcpy(p, key.data(), klen);
  p += klen;
  const uint64_t tag = PackTag(seq, type);
  for (int i = 0; i < 8; i++) *p++ = static_cast<char>((tag >> (8 * i)) & 0xFF);
  p = EncodeVarint32(p, vlen);
  std::memcpy(p, value.data(), vlen);

  table_.Insert(buf);
  count_.fetch_add(1, std::memory_order_release);
  if (seq > max_seq_.load(std::memory_order_relaxed)) max_seq_.store(seq, std::memory_order_release);
}

LookupResult MemTable::Get(std::string_view key, uint64_t snapshot, std::string* value) const {
  std::string lookup = EncodeLookup(key, snapshot);
  Table::Iterator it(&table_);
  // The first entry at or after (key, snapshot) is the newest version of key
  // visible at snapshot, if any version is.
  it.Seek(lookup.data());
  if (!it.Valid()) return LookupResult::kMissing;
  Decoded d = DecodeEntry(it.key());
  if (d.key != key) return LookupResult::kMissing;
  if (TagType(d.tag) == RecordType::kDelete) return LookupResult::kDeleted;
  value->assign(d.value);
  return LookupResult::kFound;
}

class MemTable::Iter final : public InternalIterator {
 public:
  explicit Iter(const Table* table) : it_(table) {}

  void SeekToFirst() override {
    it_.SeekToFirst();
    Decode();
  }
  void Seek(std::string_view target) override {
    std::string lookup = EncodeLookup(target, kMaxSequence);
    it_.Seek(lookup.data());
    Decode();
  }
  void Next() override {
    it_.Next();
    Decode();
  }
  bool Valid() const override { return it_.Valid(); }
  std::string_view key() const override { return current_.key; }
  uint64_t seq() const override { return TagSequence(current_.tag); }
  RecordType type() const override { return TagType(current_.tag); }
  std::string_view value() const override { return current_.value; }
  Status status() const override { return Status::OK(); }

 private:
  void Decode() {
    if (it_.Valid()) current_ = DecodeEntry(it_.key());
  }

  Table::Iterator it_;
  Decoded current_{};
};

std::unique_ptr<InternalIterator> MemTable::NewIterator() const { return std::make_unique<Iter>(&table_); }

}  // namespace ledgerkv
