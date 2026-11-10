#include "json.h"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

const JsonValue* JsonValue::find(const std::string& key) const {
  if (t != T::Obj) return nullptr;
  for (const auto& kv : obj)
    if (kv.first == key) return &kv.second;
  return nullptr;
}

namespace {

struct Parser {
  const char* p;
  const char* end;
  std::string err;

  void ws() {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
  }

  bool fail(const char* msg) {
    if (err.empty()) err = msg;
    return false;
  }

  static void put_utf8(std::string& out, uint32_t cp) {
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

  bool hex4(uint32_t& out) {
    if (end - p < 4) return fail("short \\u escape");
    out = 0;
    for (int k = 0; k < 4; ++k) {
      char c = *p++;
      uint32_t d;
      if (c >= '0' && c <= '9') d = static_cast<uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f') d = static_cast<uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') d = static_cast<uint32_t>(c - 'A' + 10);
      else return fail("bad hex digit");
      out = out * 16 + d;
    }
    return true;
  }

  bool string(std::string& out) {
    if (p >= end || *p != '"') return fail("expected string");
    ++p;
    out.clear();
    while (p < end) {
      char c = *p++;
      if (c == '"') return true;
      if (c != '\\') { out.push_back(c); continue; }
      if (p >= end) return fail("bad escape");
      char e = *p++;
      switch (e) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          uint32_t cp = 0;
          if (!hex4(cp)) return false;
          if (cp >= 0xD800 && cp <= 0xDBFF && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
            const char* save = p;
            p += 2;
            uint32_t lo = 0;
            if (hex4(lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            } else {
              p = save;
            }
          }
          put_utf8(out, cp);
          break;
        }
        default: return fail("unknown escape");
      }
    }
    return fail("unterminated string");
  }

  bool number(JsonValue& v) {
    const char* start = p;
    bool is_double = false;
    if (p < end && (*p == '-' || *p == '+')) ++p;
    while (p < end && *p >= '0' && *p <= '9') ++p;
    if (p < end && *p == '.') { is_double = true; ++p; while (p < end && *p >= '0' && *p <= '9') ++p; }
    if (p < end && (*p == 'e' || *p == 'E')) {
      is_double = true; ++p;
      if (p < end && (*p == '-' || *p == '+')) ++p;
      while (p < end && *p >= '0' && *p <= '9') ++p;
    }
    std::string tmp(start, p);
    if (tmp.empty()) return fail("bad number");
    if (is_double) {
      v.t = JsonValue::T::Double;
      v.d = std::strtod(tmp.c_str(), nullptr);
      if (!std::isfinite(v.d)) return fail("non-finite number");
    } else {
      v.t = JsonValue::T::Int;
      auto res = std::from_chars(tmp.data(), tmp.data() + tmp.size(), v.i);
      if (res.ec != std::errc()) return fail("integer overflow");
    }
    return true;
  }

  bool value(JsonValue& v, int depth) {
    if (depth > 64) return fail("nesting too deep");
    ws();
    if (p >= end) return fail("unexpected end");
    char c = *p;
    if (c == '{') {
      ++p; v.t = JsonValue::T::Obj; ws();
      if (p < end && *p == '}') { ++p; return true; }
      while (true) {
        ws();
        std::string key;
        if (!string(key)) return false;
        ws();
        if (p >= end || *p != ':') return fail("expected colon");
        ++p;
        JsonValue child;
        if (!value(child, depth + 1)) return false;
        v.obj.emplace_back(std::move(key), std::move(child));
        ws();
        if (p < end && *p == ',') { ++p; continue; }
        if (p < end && *p == '}') { ++p; return true; }
        return fail("expected , or }");
      }
    }
    if (c == '[') {
      ++p; v.t = JsonValue::T::Array; ws();
      if (p < end && *p == ']') { ++p; return true; }
      while (true) {
        JsonValue child;
        if (!value(child, depth + 1)) return false;
        v.arr.push_back(std::move(child));
        ws();
        if (p < end && *p == ',') { ++p; continue; }
        if (p < end && *p == ']') { ++p; return true; }
        return fail("expected , or ]");
      }
    }
    if (c == '"') { v.t = JsonValue::T::Str; return string(v.s); }
    if (end - p >= 4 && std::strncmp(p, "true", 4) == 0) { p += 4; v.t = JsonValue::T::Bool; v.b = true; return true; }
    if (end - p >= 5 && std::strncmp(p, "false", 5) == 0) { p += 5; v.t = JsonValue::T::Bool; v.b = false; return true; }
    if (end - p >= 4 && std::strncmp(p, "null", 4) == 0) { p += 4; v.t = JsonValue::T::Null; return true; }
    return number(v);
  }
};

}  // namespace

bool json_parse(const std::string& text, JsonValue& out, std::string& err) {
  Parser ps{text.data(), text.data() + text.size(), {}};
  out = JsonValue{};
  if (!ps.value(out, 0)) { err = ps.err; return false; }
  ps.ws();
  if (ps.p != ps.end) { err = "trailing characters"; return false; }
  return true;
}

void json_escape_into(std::string& out, const std::string& s) {
  static const char* hex = "0123456789abcdef";
  out.push_back('"');
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        if (c < 0x20) {
          out += "\\u00";
          out.push_back(hex[(c >> 4) & 0xF]);
          out.push_back(hex[c & 0xF]);
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  out.push_back('"');
}

void json_write_double(std::string& out, double v) {
  if (!std::isfinite(v)) { out += "0"; return; }
  char buf[64];
  auto res = std::to_chars(buf, buf + sizeof(buf), v);
  if (res.ec != std::errc()) { out += "0"; return; }
  std::string_view sv(buf, static_cast<size_t>(res.ptr - buf));
  out.append(sv);
}

