#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace ledgerkv {

inline void PutFixed32(std::string* dst, uint32_t v) {
  char buf[4];
  for (int i = 0; i < 4; i++) buf[i] = static_cast<char>((v >> (8 * i)) & 0xFF);
  dst->append(buf, 4);
}

inline uint32_t DecodeFixed32(const char* p) {
  uint32_t v = 0;
  for (int i = 0; i < 4; i++) v |= static_cast<uint32_t>(static_cast<uint8_t>(p[i])) << (8 * i);
  return v;
}

inline void PutVarint32(std::string* dst, uint32_t v) {
  while (v >= 0x80) {
    dst->push_back(static_cast<char>((v & 0x7F) | 0x80));
    v >>= 7;
  }
  dst->push_back(static_cast<char>(v));
}

// Parses a varint32 from the front of *in and advances it. Returns false on
// truncated or overlong input.
inline bool GetVarint32(std::string_view* in, uint32_t* v) {
  uint32_t result = 0;
  for (int shift = 0; shift <= 28 && !in->empty(); shift += 7) {
    auto byte = static_cast<uint8_t>(in->front());
    in->remove_prefix(1);
    result |= static_cast<uint32_t>(byte & 0x7F) << shift;
    if (!(byte & 0x80)) {
      *v = result;
      return true;
    }
  }
  return false;
}

inline void PutLengthPrefixed(std::string* dst, std::string_view s) {
  PutVarint32(dst, static_cast<uint32_t>(s.size()));
  dst->append(s);
}

inline bool GetLengthPrefixed(std::string_view* in, std::string_view* out) {
  uint32_t len;
  if (!GetVarint32(in, &len) || in->size() < len) return false;
  *out = in->substr(0, len);
  in->remove_prefix(len);
  return true;
}

}  // namespace ledgerkv

namespace ledgerkv {

inline void PutFixed64(std::string* dst, uint64_t v) {
  PutFixed32(dst, static_cast<uint32_t>(v));
  PutFixed32(dst, static_cast<uint32_t>(v >> 32));
}

inline uint64_t DecodeFixed64(const char* p) {
  return static_cast<uint64_t>(DecodeFixed32(p)) | (static_cast<uint64_t>(DecodeFixed32(p + 4)) << 32);
}

}  // namespace ledgerkv

namespace ledgerkv {

inline void PutVarint64(std::string* dst, uint64_t v) {
  while (v >= 0x80) {
    dst->push_back(static_cast<char>((v & 0x7F) | 0x80));
    v >>= 7;
  }
  dst->push_back(static_cast<char>(v));
}

inline bool GetVarint64(std::string_view* in, uint64_t* v) {
  uint64_t result = 0;
  for (int shift = 0; shift <= 63 && !in->empty(); shift += 7) {
    auto byte = static_cast<uint8_t>(in->front());
    in->remove_prefix(1);
    result |= static_cast<uint64_t>(byte & 0x7F) << shift;
    if (!(byte & 0x80)) {
      *v = result;
      return true;
    }
  }
  return false;
}

}  // namespace ledgerkv
