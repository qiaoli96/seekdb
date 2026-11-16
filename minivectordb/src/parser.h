#pragma once

#include <cstdint>
#include <string>
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
  std::string s;
};

enum class CmpOp { EQ, LT, LE, GT, GE };

struct Compare {
  std::string col;
  CmpOp op = CmpOp::EQ;
  Literal lit;
};

struct SelectItem {
  bool is_distance = false;
  std::string col;       // projected column or vector column for distance
  std::string alias;     // output name
  std::string vec_text;  // distance query vector literal
};

struct SelectStmt {
  std::string table;
  bool star = false;
  std::vector<SelectItem> items;
  std::vector<Compare> where;
  enum OrderKind { ORDER_NONE, ORDER_COLUMN, ORDER_DISTANCE } order_kind = ORDER_NONE;
  std::string order_col;   // column name, or select alias
  std::string order_vec;   // vector literal when ordering by a distance expression
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
};

Stmt parse_statement(const std::string &sql);

}  // namespace mvd
