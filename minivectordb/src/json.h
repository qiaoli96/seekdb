#pragma once

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "common.h"

namespace mvd {

struct JsonValue {
  enum Type { NUL, BOOL, INT, DOUBLE, STR, ARR, OBJ } type = NUL;
  bool b = false;
  int64_t i = 0;
  double d = 0;
  std::string s;
  std::vector<JsonValue> arr;
  std::vector<std::pair<std::string, JsonValue>> obj;

  const JsonValue *get(const std::string &key) const {
    for (const auto &kv : obj) {
      if (kv.first == key) {
        return &kv.second;
      }
    }
    return nullptr;
  }
};

namespace json_detail {

inline void append_utf8(std::string &out, uint32_t cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

class JsonParser {
 public:
  explicit JsonParser(const std::string &text) : p_(text.data()), n_(text.size()) {}

  JsonValue parse() {
    skip_ws();
    JsonValue v = parse_value();
    skip_ws();
    if (pos_ != n_) {
      bad();
    }
    return v;
  }

 private:
  [[noreturn]] void bad() { raise("invalid_request", "malformed JSON request"); }

  void skip_ws() {
    while (pos_ < n_ && (p_[pos_] == ' ' || p_[pos_] == '\t' || p_[pos_] == '\n' ||
                         p_[pos_] == '\r')) {
      ++pos_;
    }
  }

  char peek() {
    if (pos_ >= n_) {
      bad();
    }
    return p_[pos_];
  }

  void expect(char c) {
    if (pos_ >= n_ || p_[pos_] != c) {
      bad();
    }
    ++pos_;
  }

  bool consume_lit(const char *lit) {
    size_t len = strlen(lit);
    if (pos_ + len > n_ || memcmp(p_ + pos_, lit, len) != 0) {
      return false;
    }
    pos_ += len;
    return true;
  }

  JsonValue parse_value() {
    skip_ws();
    if (depth_ > 32) {
      bad();
    }
    char c = peek();
    JsonValue v;
    if (c == '{') {
      ++pos_;
      ++depth_;
      skip_ws();
      if (peek() == '}') {
        ++pos_;
        --depth_;
        v.type = JsonValue::OBJ;
        return v;
      }
      v.type = JsonValue::OBJ;
      while (true) {
        skip_ws();
        JsonValue key;
        if (peek() != '"') {
          bad();
        }
        key = parse_string();
        skip_ws();
        expect(':');
        v.obj.emplace_back(key.s, parse_value());
        skip_ws();
        char d = peek();
        if (d == ',') {
          ++pos_;
          continue;
        }
        if (d == '}') {
          ++pos_;
          break;
        }
        bad();
      }
      --depth_;
      return v;
    }
    if (c == '[') {
      ++pos_;
      ++depth_;
      skip_ws();
      if (peek() == ']') {
        ++pos_;
        --depth_;
        v.type = JsonValue::ARR;
        return v;
      }
      v.type = JsonValue::ARR;
      while (true) {
        v.arr.push_back(parse_value());
        skip_ws();
        char d = peek();
        if (d == ',') {
          ++pos_;
          continue;
        }
        if (d == ']') {
          ++pos_;
          break;
        }
        bad();
      }
      --depth_;
      return v;
    }
    if (c == '"') {
      return parse_string();
    }
    if (consume_lit("true")) {
      v.type = JsonValue::BOOL;
      v.b = true;
      return v;
    }
    if (consume_lit("false")) {
      v.type = JsonValue::BOOL;
      v.b = false;
      return v;
    }
    if (consume_lit("null")) {
      v.type = JsonValue::NUL;
      return v;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
      return parse_number();
    }
    bad();
  }

  JsonValue parse_string() {
    expect('"');
    JsonValue v;
    v.type = JsonValue::STR;
    while (true) {
      if (pos_ >= n_) {
        bad();
      }
      // Copy whole runs of plain characters; escaping is the uncommon case and
      // the request bodies are dominated by large SQL literals.
      const char *q = static_cast<const char *>(memchr(p_ + pos_, '"', n_ - pos_));
      if (q == nullptr) {
        bad();
      }
      size_t quote = static_cast<size_t>(q - p_);
      const char *b = static_cast<const char *>(memchr(p_ + pos_, '\\', quote - pos_));
      size_t bs = b != nullptr ? static_cast<size_t>(b - p_) : quote;
      v.s.append(p_ + pos_, bs - pos_);
      pos_ = bs;
      if (bs == quote) {
        pos_ = quote + 1;
        break;
      }
      ++pos_;  // backslash
      if (pos_ >= n_) {
        bad();
      }
      char e = p_[pos_++];
      switch (e) {
        case '"': v.s.push_back('"'); break;
        case '\\': v.s.push_back('\\'); break;
        case '/': v.s.push_back('/'); break;
        case 'b': v.s.push_back('\b'); break;
        case 'f': v.s.push_back('\f'); break;
        case 'n': v.s.push_back('\n'); break;
        case 'r': v.s.push_back('\r'); break;
        case 't': v.s.push_back('\t'); break;
        case 'u': {
          uint32_t cp = parse_hex4();
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (pos_ + 1 < n_ && p_[pos_] == '\\' && p_[pos_ + 1] == 'u') {
              pos_ += 2;
              uint32_t low = parse_hex4();
              if (low >= 0xDC00 && low <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
              } else {
                cp = 0xFFFD;
              }
            } else {
              cp = 0xFFFD;
            }
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            cp = 0xFFFD;
          }
          append_utf8(v.s, cp);
          break;
        }
        default:
          bad();
      }
    }
    return v;
  }

  uint32_t parse_hex4() {
    if (pos_ + 4 > n_) {
      bad();
    }
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      char c = p_[pos_++];
      v <<= 4;
      if (c >= '0' && c <= '9') {
        v |= static_cast<uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        v |= static_cast<uint32_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        v |= static_cast<uint32_t>(c - 'A' + 10);
      } else {
        bad();
      }
    }
    return v;
  }

  JsonValue parse_number() {
    JsonValue v;
    size_t start = pos_;
    if (p_[pos_] == '-') {
      ++pos_;
    }
    while (pos_ < n_ && p_[pos_] >= '0' && p_[pos_] <= '9') {
      ++pos_;
    }
    bool is_double = false;
    if (pos_ < n_ && p_[pos_] == '.') {
      is_double = true;
      ++pos_;
      while (pos_ < n_ && p_[pos_] >= '0' && p_[pos_] <= '9') {
        ++pos_;
      }
    }
    if (pos_ < n_ && (p_[pos_] == 'e' || p_[pos_] == 'E')) {
      is_double = true;
      ++pos_;
      if (pos_ < n_ && (p_[pos_] == '+' || p_[pos_] == '-')) {
        ++pos_;
      }
      while (pos_ < n_ && p_[pos_] >= '0' && p_[pos_] <= '9') {
        ++pos_;
      }
    }
    std::string text(p_ + start, pos_ - start);
    if (text.empty() || text == "-") {
      bad();
    }
    if (is_double) {
      v.type = JsonValue::DOUBLE;
      v.d = strtod(text.c_str(), nullptr);
      if (!std::isfinite(v.d)) {
        bad();
      }
    } else {
      errno = 0;
      char *end = nullptr;
      long long iv = strtoll(text.c_str(), &end, 10);
      if (errno == ERANGE || end == nullptr || *end != '\0') {
        v.type = JsonValue::DOUBLE;
        v.d = strtod(text.c_str(), nullptr);
        if (!std::isfinite(v.d)) {
          bad();
        }
      } else {
        v.type = JsonValue::INT;
        v.i = static_cast<int64_t>(iv);
      }
    }
    return v;
  }

  const char *p_;
  size_t n_;
  size_t pos_ = 0;
  int depth_ = 0;
};

}  // namespace json_detail

inline JsonValue parse_json(const std::string &text) {
  json_detail::JsonParser parser(text);
  return parser.parse();
}

inline void json_append_string(std::string &out, const std::string &s) {
  out.push_back('"');
  out += json_escape(s);
  out.push_back('"');
}

inline void json_append_value(std::string &out, const JsonValue &v) {
  switch (v.type) {
    case JsonValue::NUL: out += "null"; break;
    case JsonValue::BOOL: out += v.b ? "true" : "false"; break;
    case JsonValue::INT: out += std::to_string(v.i); break;
    case JsonValue::DOUBLE: out += format_double(v.d); break;
    case JsonValue::STR: json_append_string(out, v.s); break;
    case JsonValue::ARR: {
      out.push_back('[');
      for (size_t i = 0; i < v.arr.size(); ++i) {
        if (i) {
          out.push_back(',');
        }
        json_append_value(out, v.arr[i]);
      }
      out.push_back(']');
      break;
    }
    case JsonValue::OBJ: {
      out.push_back('{');
      for (size_t i = 0; i < v.obj.size(); ++i) {
        if (i) {
          out.push_back(',');
        }
        json_append_string(out, v.obj[i].first);
        out.push_back(':');
        json_append_value(out, v.obj[i].second);
      }
      out.push_back('}');
      break;
    }
  }
}

}  // namespace mvd
