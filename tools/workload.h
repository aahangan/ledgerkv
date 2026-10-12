#pragma once

// The deterministic workload shared by the crash and power-loss tests, and
// the model of what the database must contain after a prefix of it.
//
// Per writer thread, operation n is:
//   n % 10 == 9: delete of key n - 5
//   n % 10 == 7: batch putting key n and its mirror key, both to Value(n)
//   otherwise:   put of key n

#include <cstdint>
#include <cstdio>
#include <string>

#include "ledgerkv/db.h"

namespace ledgerkv::workload {

inline uint64_t Mix(uint64_t x) {  // splitmix64
  x += 0x9E3779B97F4A7C15ULL;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
  return x ^ (x >> 31);
}

inline bool IsDelete(uint64_t n) { return n % 10 == 9; }
inline bool IsBatch(uint64_t n) { return n % 10 == 7; }
inline uint64_t TargetKey(uint64_t n) { return IsDelete(n) ? n - 5 : n; }

// A batch's mirror key sorts right after its main key. Keeping them adjacent
// matters: if mirrors lived in a separate key range, every small table would
// span the whole key space, every level-0 compaction would rewrite all of
// level 1, and compactions would rarely finish between crashes.
inline std::string Key(int thread, uint64_t n, bool mirror = false) {
  char buf[48];
  std::snprintf(buf, sizeof(buf), "t%02d-key%012llu%s", thread, static_cast<unsigned long long>(n),
                mirror ? "~mirror" : "");
  return buf;
}

inline std::string Value(int thread, uint64_t n) {
  uint64_t h = Mix(n * 131 + static_cast<uint64_t>(thread));
  std::string v(1 + h % 128, '\0');
  for (size_t i = 0; i < v.size(); i++) v[i] = static_cast<char>('a' + (Mix(h + i) % 26));
  return v;
}

inline Status Apply(DB* db, int thread, uint64_t n) {
  if (IsDelete(n)) return db->Delete(Key(thread, TargetKey(n)));
  if (IsBatch(n)) {
    WriteBatch b;
    b.Put(Key(thread, n), Value(thread, n));
    b.Put(Key(thread, n, /*mirror=*/true), Value(thread, n));
    return db->Write(b);
  }
  return db->Put(Key(thread, n), Value(thread, n));
}

// Whether key k (and its mirror, for batch keys) is present after
// operations [1, last] have been applied.
inline bool ExpectPresent(uint64_t k, uint64_t last) {
  if (k > last || IsDelete(k)) return false;
  return !(k % 10 == 4 && k + 5 <= last);
}

}  // namespace ledgerkv::workload
