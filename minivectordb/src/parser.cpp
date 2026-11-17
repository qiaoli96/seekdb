#include "parser.h"

#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace mvd {

namespace {

enum class TokKind { END, IDENT, INT, STR, PUNCT };

struct Tok {
  TokKind kind = TokKind::END;
  // Identifiers and string bodies point into the statement text (or into the
  // unescape arena for doubled quotes); tokens never own characters.
  std::string_view text;
  int64_t iv = 0;
  char punct = 0;
};

bool iequals(const std::string &a, const std::string &b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); ++i) {
    if (toupper(static_cast<unsigned char>(a[i])) != toupper(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

struct TokenList {
  std::vector<Tok> toks;
  std::vector<char> arena;  // owns unescaped string bodies referenced by toks
};

TokenList tokenize(const std::string &sql) {
  TokenList out;
  out.toks.reserve(sql.size() / 8 + 8);
  out.arena.reserve(sql.size() + 16);
  size_t i = 0;
  while (i < sql.size()) {
    unsigned char c = static_cast<unsigned char>(sql[i]);
    if (isspace(c)) {
      ++i;
      continue;
    }
    if (isalpha(c) || c == '_') {
      size_t start = i;
      while (i < sql.size() &&
             (isalnum(static_cast<unsigned char>(sql[i])) || sql[i] == '_')) {
        ++i;
      }
      Tok t;
      t.kind = TokKind::IDENT;
      t.text = std::string_view(sql.data() + start, i - start);
      out.toks.push_back(t);
      continue;
    }
    if (isdigit(c) || (c == '-' && i + 1 < sql.size() &&
                       isdigit(static_cast<unsigned char>(sql[i + 1])))) {
      size_t start = i;
      if (sql[i] == '-') {
        ++i;
      }
      while (i < sql.size() && isdigit(static_cast<unsigned char>(sql[i]))) {
        ++i;
      }
      long long v = 0;
      auto res = std::from_chars(sql.data() + start, sql.data() + i, v);
      if (res.ec != std::errc() || res.ptr != sql.data() + i) {
        raise("value_out_of_range");
      }
      Tok t;
      t.kind = TokKind::INT;
      t.text = std::string_view(sql.data() + start, i - start);
      t.iv = static_cast<int64_t>(v);
      out.toks.push_back(t);
      continue;
    }
    if (c == '\'') {
      ++i;
      size_t start = i;
      bool escaped = false;
      while (true) {
        const char *q = static_cast<const char *>(memchr(sql.data() + i, '\'', sql.size() - i));
        if (q == nullptr) {
          i = sql.size();
          break;
        }
        i = static_cast<size_t>(q - sql.data());
        if (i + 1 < sql.size() && sql[i + 1] == '\'') {
          escaped = true;
          i += 2;
          continue;
        }
        break;
      }
      if (i >= sql.size()) {
        raise("syntax_error");
      }
      Tok t;
      t.kind = TokKind::STR;
      if (!escaped) {
        t.text = std::string_view(sql.data() + start, i - start);
      } else {
        size_t base = out.arena.size();
        size_t j = start;
        while (j < i) {
          if (sql[j] == '\'' && j + 1 < i && sql[j + 1] == '\'') {
            out.arena.push_back('\'');
            j += 2;
          } else {
            out.arena.push_back(sql[j++]);
          }
        }
        t.text = std::string_view(out.arena.data() + base, out.arena.size() - base);
      }
      ++i;  // closing quote
      out.toks.push_back(t);
      continue;
    }
    if (c == '(' || c == ')' || c == ',' || c == '=' || c == '*' || c == ';' ||
        c == '<' || c == '>' || c == '+' || c == '-' || c == '!') {
      Tok t;
      t.kind = TokKind::PUNCT;
      t.punct = static_cast<char>(c);
      out.toks.push_back(t);
      ++i;
      continue;
    }
    raise("syntax_error");
  }
  Tok end;
  end.kind = TokKind::END;
  out.toks.push_back(end);
  return out;
}

class Parser {
 public:
  explicit Parser(TokenList toks) : toks_(std::move(toks.toks)), arena_(std::move(toks.arena)) {}

  std::vector<char> &arena() { return arena_; }

  Stmt parse() {
    Stmt stmt;
    if (peek().kind == TokKind::END) {
      raise("syntax_error");
    }
    if (peek_kw("CREATE")) {
      next();
      if (peek_kw("TABLE")) {
        stmt.kind = StmtKind::CREATE_TABLE;
        parse_create_table(stmt);
      } else if (peek_kw("VECTOR")) {
        next();
        if (!peek_kw("INDEX")) {
          stmt.kind = StmtKind::UNSUPPORTED;
          return stmt;
        }
        next();
        stmt.kind = StmtKind::CREATE_INDEX;
        stmt.index_name = expect_ident();
        expect_kw("ON");
        stmt.table = expect_ident();
        expect_punct('(');
        stmt.index_col = expect_ident();
        expect_punct(')');
      } else {
        stmt.kind = StmtKind::UNSUPPORTED;
        return stmt;
      }
    } else if (peek_kw("INSERT")) {
      next();
      stmt.kind = StmtKind::INSERT;
      parse_insert(stmt.insert);
    } else if (peek_kw("SELECT")) {
      next();
      stmt.kind = StmtKind::SELECT;
      parse_select(stmt.select);
    } else if (peek_kw("UPDATE")) {
      next();
      stmt.kind = StmtKind::UPDATE;
      parse_update(stmt.update);
    } else if (peek_kw("DELETE")) {
      next();
      stmt.kind = StmtKind::DELETE;
      parse_delete(stmt.del);
    } else if (peek_kw("BEGIN")) {
      next();
      if (peek_kw("WORK") || peek_kw("TRANSACTION")) {
        next();
      }
      stmt.kind = StmtKind::BEGIN;
    } else if (peek_kw("START")) {
      next();
      expect_kw("TRANSACTION");
      stmt.kind = StmtKind::BEGIN;
    } else if (peek_kw("COMMIT")) {
      next();
      if (peek_kw("WORK")) {
        next();
      }
      stmt.kind = StmtKind::COMMIT;
    } else if (peek_kw("ROLLBACK")) {
      next();
      if (peek_kw("WORK")) {
        next();
      }
      stmt.kind = StmtKind::ROLLBACK;
    } else {
      stmt.kind = StmtKind::UNSUPPORTED;
      return stmt;
    }

    if (peek().kind == TokKind::PUNCT && peek().punct == ';') {
      next();
    }
    if (peek().kind != TokKind::END) {
      raise("syntax_error");
    }
    return stmt;
  }

 private:
  const Tok &peek(size_t off = 0) const { return toks_[pos_ + off]; }
  const Tok &next() { return toks_[pos_++]; }

  bool peek_kw(const char *kw, size_t off = 0) const {
    const Tok &t = peek(off);
    if (t.kind != TokKind::IDENT) {
      return false;
    }
    size_t n = strlen(kw);
    if (t.text.size() != n) {
      return false;
    }
    for (size_t i = 0; i < n; ++i) {
      if (toupper(static_cast<unsigned char>(t.text[i])) != kw[i]) {
        return false;
      }
    }
    return true;
  }

  void expect_kw(const char *kw) {
    if (!peek_kw(kw)) {
      raise("syntax_error");
    }
    next();
  }

  void expect_punct(char p) {
    if (peek().kind != TokKind::PUNCT || peek().punct != p) {
      raise("syntax_error");
    }
    next();
  }

  std::string expect_ident() {
    if (peek().kind != TokKind::IDENT) {
      raise("syntax_error");
    }
    return std::string(next().text);
  }

  int64_t expect_int() {
    if (peek().kind != TokKind::INT) {
      raise("syntax_error");
    }
    return next().iv;
  }

  Literal parse_literal() {
    Literal lit;
    if (peek().kind == TokKind::INT) {
      lit.kind = Literal::INT;
      lit.i = next().iv;
    } else if (peek().kind == TokKind::STR) {
      lit.kind = Literal::STR;
      lit.s = next().text;
    } else {
      raise("syntax_error");
    }
    return lit;
  }

  CmpOp parse_op() {
    if (peek().kind != TokKind::PUNCT) {
      raise("syntax_error");
    }
    char c = next().punct;
    if (c == '=') {
      return CmpOp::EQ;
    }
    if (c == '<') {
      if (peek().kind == TokKind::PUNCT && peek().punct == '=') {
        next();
        return CmpOp::LE;
      }
      if (peek().kind == TokKind::PUNCT && peek().punct == '>') {
        next();
        return CmpOp::NE;
      }
      return CmpOp::LT;
    }
    if (c == '>') {
      if (peek().kind == TokKind::PUNCT && peek().punct == '=') {
        next();
        return CmpOp::GE;
      }
      return CmpOp::GT;
    }
    if (c == '!') {
      if (peek().kind == TokKind::PUNCT && peek().punct == '=') {
        next();
        return CmpOp::NE;
      }
    }
    raise("syntax_error");
  }

  void parse_where(std::vector<Compare> &where) {
    if (!peek_kw("WHERE")) {
      return;
    }
    next();
    while (true) {
      Compare cmp;
      cmp.col = expect_ident();
      cmp.op = parse_op();
      cmp.lit = parse_literal();
      where.push_back(cmp);
      if (peek_kw("AND")) {
        next();
        continue;
      }
      break;
    }
  }

  void parse_create_table(Stmt &stmt) {
    next();  // TABLE
    stmt.table = expect_ident();
    expect_punct('(');
    std::string table_pk;
    while (true) {
      if (peek_kw("PRIMARY")) {
        next();
        expect_kw("KEY");
        expect_punct('(');
        table_pk = expect_ident();
        expect_punct(')');
      } else {
        ColumnDef def;
        def.name = expect_ident();
        if (peek_kw("BIGINT")) {
          next();
          def.type = ColType::BIGINT;
        } else if (peek_kw("VARCHAR")) {
          next();
          expect_punct('(');
          def.type = ColType::VARCHAR;
          def.len = expect_int();
          expect_punct(')');
        } else if (peek_kw("VECTOR")) {
          next();
          expect_punct('(');
          def.type = ColType::VECTOR;
          def.len = expect_int();
          expect_punct(')');
        } else {
          raise("syntax_error");
        }
        if (peek_kw("PRIMARY")) {
          next();
          expect_kw("KEY");
          def.pk = true;
        }
        stmt.columns.push_back(def);
      }
      if (peek().kind == TokKind::PUNCT && peek().punct == ',') {
        next();
        continue;
      }
      break;
    }
    expect_punct(')');
    if (!table_pk.empty()) {
      bool found = false;
      for (ColumnDef &def : stmt.columns) {
        if (iequals(def.name, table_pk)) {
          def.pk = true;
          found = true;
        }
      }
      if (!found) {
        raise("syntax_error");
      }
    }
  }

  void parse_insert(InsertStmt &ins) {
    expect_kw("INTO");
    ins.table = expect_ident();
    if (peek().kind == TokKind::PUNCT && peek().punct == '(') {
      next();
      while (true) {
        ins.columns.push_back(expect_ident());
        if (peek().kind == TokKind::PUNCT && peek().punct == ',') {
          next();
          continue;
        }
        break;
      }
      expect_punct(')');
    }
    expect_kw("VALUES");
    while (true) {
      expect_punct('(');
      std::vector<Literal> row;
      while (true) {
        row.push_back(parse_literal());
        if (peek().kind == TokKind::PUNCT && peek().punct == ',') {
          next();
          continue;
        }
        break;
      }
      expect_punct(')');
      ins.rows.push_back(std::move(row));
      if (peek().kind == TokKind::PUNCT && peek().punct == ',') {
        next();
        continue;
      }
      break;
    }
  }

  void parse_distance(SelectItem &item) {
    next();  // L2_DISTANCE
    expect_punct('(');
    item.is_distance = true;
    item.col = expect_ident();
    expect_punct(',');
    if (peek().kind != TokKind::STR) {
      raise("syntax_error");
    }
    item.vec_text = next().text;
    expect_punct(')');
  }

  void parse_select(SelectStmt &sel) {
    if (peek().kind == TokKind::PUNCT && peek().punct == '*') {
      next();
      sel.star = true;
    } else {
      while (true) {
        SelectItem item;
        if (peek_kw("L2_DISTANCE")) {
          parse_distance(item);
          if (peek_kw("AS")) {
            next();
            item.alias = expect_ident();
          } else {
            item.alias = "distance";
          }
        } else {
          item.col = expect_ident();
          if (peek_kw("AS")) {
            next();
            item.alias = expect_ident();
          } else {
            item.alias = item.col;
          }
        }
        sel.items.push_back(std::move(item));
        if (peek().kind == TokKind::PUNCT && peek().punct == ',') {
          next();
          continue;
        }
        break;
      }
    }
    expect_kw("FROM");
    sel.table = expect_ident();
    parse_where(sel.where);
    if (peek_kw("ORDER")) {
      next();
      expect_kw("BY");
      if (peek_kw("L2_DISTANCE")) {
        SelectItem item;
        parse_distance(item);
        sel.order_kind = SelectStmt::ORDER_DISTANCE;
        sel.order_col = "distance";
        sel.order_vec = item.vec_text;
        sel.order_vec_col = item.col;
      } else {
        sel.order_kind = SelectStmt::ORDER_COLUMN;
        sel.order_col = expect_ident();
      }
      if (peek_kw("ASC")) {
        next();
      } else if (peek_kw("DESC")) {
        next();
        sel.order_desc = true;
      }
    }
    if (peek_kw("LIMIT")) {
      next();
      sel.has_limit = true;
      sel.limit = expect_int();
      if (sel.limit < 0) {
        raise("syntax_error");
      }
    }
  }

  void parse_update(UpdateStmt &upd) {
    upd.table = expect_ident();
    expect_kw("SET");
    while (true) {
      SetItem item;
      item.col = expect_ident();
      expect_punct('=');
      item.lit = parse_literal();
      upd.sets.push_back(std::move(item));
      if (peek().kind == TokKind::PUNCT && peek().punct == ',') {
        next();
        continue;
      }
      break;
    }
    parse_where(upd.where);
  }

  void parse_delete(DeleteStmt &del) {
    expect_kw("FROM");
    del.table = expect_ident();
    parse_where(del.where);
  }

  std::vector<Tok> toks_;
  std::vector<char> arena_;
  size_t pos_ = 0;
};

}  // namespace

Stmt parse_statement(const std::string &sql) {
  TokenList toks = tokenize(sql);
  Parser p(std::move(toks));
  Stmt stmt = p.parse();
  // The vector move keeps the buffer address, so every view into it stays valid.
  stmt.arena = std::move(p.arena());
  return stmt;
}

}  // namespace mvd
