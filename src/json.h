#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// 最小 JSON 实现：协议层专用，必须精确处理 BIGINT（不经 double）与 \u 转义。
struct JsonValue {
  enum class T { Null, Bool, Int, Double, Str, Array, Obj };
  T t = T::Null;
  bool b = false;
  int64_t i = 0;
  double d = 0;
  std::string s;
  std::vector<JsonValue> arr;
  std::vector<std::pair<std::string, JsonValue>> obj;

  const JsonValue* find(const std::string& key) const;
  bool is_int() const { return t == T::Int; }
  bool is_str() const { return t == T::Str; }
  bool is_arr() const { return t == T::Array; }
  bool is_obj() const { return t == T::Obj; }
};

bool json_parse(const std::string& text, JsonValue& out, std::string& err);
void json_escape_into(std::string& out, const std::string& s);
void json_write_double(std::string& out, double v);
