#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "common.h"

namespace mvd {

inline uint32_t crc32(const void *data, size_t n) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      }
      table[i] = c;
    }
    init = true;
  }
  const unsigned char *p = static_cast<const unsigned char *>(data);
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) {
    c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  }
  return c ^ 0xFFFFFFFFu;
}

class Buf {
 public:
  std::string data;

  void clear() { data.clear(); }
  size_t size() const { return data.size(); }
  const char *ptr() const { return data.data(); }

  void u8(uint8_t v) { data.push_back(static_cast<char>(v)); }
  void u32(uint32_t v) {
    char tmp[4];
    memcpy(tmp, &v, 4);
    data.append(tmp, 4);
  }
  void u64(uint64_t v) {
    char tmp[8];
    memcpy(tmp, &v, 8);
    data.append(tmp, 8);
  }
  void i64(int64_t v) { u64(static_cast<uint64_t>(v)); }
  void bytes(const void *p, size_t n) { data.append(static_cast<const char *>(p), n); }
  void str(const std::string &s) {
    u32(static_cast<uint32_t>(s.size()));
    data.append(s);
  }
};

class Reader {
 public:
  Reader(const char *p, size_t n) : p_(p), n_(n) {}

  uint8_t u8() {
    need(1);
    return static_cast<uint8_t>(p_[pos_++]);
  }
  uint32_t u32() {
    need(4);
    uint32_t v;
    memcpy(&v, p_ + pos_, 4);
    pos_ += 4;
    return v;
  }
  uint64_t u64() {
    need(8);
    uint64_t v;
    memcpy(&v, p_ + pos_, 8);
    pos_ += 8;
    return v;
  }
  int64_t i64() { return static_cast<int64_t>(u64()); }
  const char *bytes(size_t n) {
    need(n);
    const char *r = p_ + pos_;
    pos_ += n;
    return r;
  }
  std::string str() {
    uint32_t n = u32();
    need(n);
    std::string r(p_ + pos_, n);
    pos_ += n;
    return r;
  }
  size_t remaining() const { return n_ - pos_; }
  bool exhausted() const { return pos_ == n_; }

 private:
  void need(size_t n) {
    if (pos_ + n > n_) {
      raise("storage_error", "corrupt record");
    }
  }
  const char *p_;
  size_t n_;
  size_t pos_ = 0;
};

inline std::string lower_ascii(const std::string &s) {
  std::string out = s;
  for (char &c : out) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return out;
}

}  // namespace mvd
