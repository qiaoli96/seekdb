#pragma once
#include <cstdint>
#include <string>
#include <vector>

enum class ColType { BigInt, VarChar, Vector };

struct Column {
  std::string name;
  ColType type = ColType::BigInt;
  int32_t varchar_len = 0;
  int32_t dim = 0;
  bool primary = false;
};

struct Schema {
  std::string table;
  std::vector<Column> cols;
  int pk_col = -1;
  int vec_col = -1;

  int find_col(const std::string& name) const {
    for (size_t k = 0; k < cols.size(); ++k)
      if (cols[k].name == name) return static_cast<int>(k);
    return -1;
  }
};

struct Value {
  enum class K { Null, Int, Float, Str, Vec } k = K::Null;
  int64_t i = 0;
  double d = 0;
  std::string s;
  std::vector<float> v;

  static Value Int(int64_t x) { Value r; r.k = K::Int; r.i = x; return r; }
  static Value Float(double x) { Value r; r.k = K::Float; r.d = x; return r; }
  static Value Str(std::string x) { Value r; r.k = K::Str; r.s = std::move(x); return r; }
  static Value Vec(std::vector<float> x) { Value r; r.k = K::Vec; r.v = std::move(x); return r; }
  bool is_null() const { return k == K::Null; }
};

using Row = std::vector<Value>;

struct Result {
  bool ok = true;
  bool is_query = false;
  std::vector<std::string> columns;
  std::vector<Row> rows;
  int64_t affected = -1;
  std::string error_code;
  std::string error_message;

  static Result Ok() { return Result{}; }
  static Result Query() { Result r; r.is_query = true; return r; }
  static Result Affected(int64_t n) { Result r; r.affected = n; return r; }
  static Result Fail(std::string code, std::string msg) {
    Result r; r.ok = false; r.error_code = std::move(code); r.error_message = std::move(msg); return r;
  }
};
