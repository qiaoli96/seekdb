#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "value.h"

// 字面量保持原始形态，由执行器按目标列类型强制转换（'[1,0]' -> VECTOR）。
struct Literal {
  enum class K { Int, Float, Str } k = K::Int;
  int64_t i = 0;
  double d = 0;
  std::string s;
};

enum class StmtKind {
  CreateTable, CreateVectorIndex, Insert, Select, Update, Delete,
  Begin, Commit, Rollback, Unsupported, Invalid
};

struct SelectItem {
  enum class K { Star, Column, Distance } k = K::Column;
  std::string col;
  std::string alias;
  bool has_alias = false;
  std::vector<float> query;
};

struct Predicate { std::string col; std::string op; Literal value; };
struct OrderKey { std::string name; bool asc = true; };

struct Statement {
  StmtKind kind = StmtKind::Invalid;
  std::string table;
  std::vector<Column> columns;
  std::vector<std::vector<Literal>> rows;
  std::vector<SelectItem> items;
  std::vector<Predicate> where;
  std::vector<OrderKey> order;
  int64_t limit = -1;
  std::vector<std::pair<std::string, Literal>> set_pairs;
  std::string index_name;
  std::string index_col;
  std::string error_code;
  std::string error_message;
};

Statement parse_sql(const std::string& sql);
