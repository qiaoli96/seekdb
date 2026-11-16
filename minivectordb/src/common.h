#pragma once

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace mvd {

enum class ColType : uint8_t { BIGINT = 1, VARCHAR = 2, VECTOR = 3 };

struct Column {
  std::string name;
  ColType type = ColType::BIGINT;
  int64_t len = 0;  // varchar: max code points, vector: dimension
  bool pk = false;
};

struct Value {
  ColType type = ColType::BIGINT;
  int64_t i = 0;
  std::string s;
  std::vector<float> v;
};

struct Error {
  std::string code;
  std::string message;
};

[[noreturn]] inline void raise(const char *code, const std::string &message = std::string()) {
  throw Error{code, message};
}

inline int64_t utf8_length(const std::string &s) {
  int64_t n = 0;
  for (unsigned char c : s) {
    if ((c & 0xC0) != 0x80) {
      ++n;
    }
  }
  return n;
}

inline std::string json_escape(const std::string &s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  return out;
}

inline std::string format_double(double d) {
  char buf[64];
  auto res = std::to_chars(buf, buf + sizeof(buf), d);
  return std::string(buf, res.ptr);
}

inline std::string format_float(float f) {
  char buf[64];
  auto res = std::to_chars(buf, buf + sizeof(buf), f);
  return std::string(buf, res.ptr);
}

inline std::string format_vector(const std::vector<float> &v) {
  std::string out = "[";
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) {
      out += ",";
    }
    out += format_float(v[i]);
  }
  out += "]";
  return out;
}

}  // namespace mvd
