#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "common.h"

namespace mvd {

enum class StmtKind {
  CREATE_TABLE,
  CREATE_INDEX,
  INSERT,
  SELECT,
  UPDATE,
  DELETE,
  BEGIN,
  COMMIT,
  ROLLBACK,
  UNSUPPORTED,
};

struct ColumnDef {
  std::string name;
  ColType type = ColType::BIGINT;
  int64_t len = 0;
  bool pk = false;
};

struct Literal {
  enum Kind { INT, STR } kind = INT;
  int64_t i = 0;
  // Points into the statement text (or Stmt::arena for unescaped literals);
  // the statement string must outlive the Stmt.
  std::string_view s;
};

enum class CmpOp { EQ, NE, LT, LE, GT, GE };

struct Compare {
  std::string col;
  CmpOp op = CmpOp::EQ;
  Literal lit;
};

struct SelectItem {
  bool is_distance = false;
  std::string col;         // projected column or vector column for distance
  std::string_view vec_text;  // distance query vector literal
  std::string alias;       // output name
};

struct SelectStmt {
  std::string table;
  bool star = false;
  std::vector<SelectItem> items;
  std::vector<Compare> where;
  enum OrderKind { ORDER_NONE, ORDER_COLUMN, ORDER_DISTANCE } order_kind = ORDER_NONE;
  std::string order_col;   // column name, or select alias
  std::string_view order_vec;  // vector literal when ordering by a distance expression
  std::string order_vec_col;  // vector column for ORDER BY L2_DISTANCE(...)
  bool order_desc = false;
  bool has_limit = false;
  int64_t limit = 0;
};

struct SetItem {
  std::string col;
  Literal lit;
};

struct InsertStmt {
  std::string table;
  std::vector<std::string> columns;
  std::vector<std::vector<Literal>> rows;
};

struct UpdateStmt {
  std::string table;
  std::vector<SetItem> sets;
  std::vector<Compare> where;
};

struct DeleteStmt {
  std::string table;
  std::vector<Compare> where;
};

struct Stmt {
  StmtKind kind = StmtKind::UNSUPPORTED;
  std::string table;
  std::vector<ColumnDef> columns;
  std::string index_name;
  std::string index_col;
  InsertStmt insert;
  UpdateStmt update;
  DeleteStmt del;
  SelectStmt select;
  // Owns the unescaped bytes referenced by string literals that contained
  // doubled quotes; views into it stay valid as long as this Stmt lives.
  std::vector<char> arena;
};

Stmt parse_statement(const std::string &sql);

}  // namespace mvd
