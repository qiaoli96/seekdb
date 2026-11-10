#include "parser.h"

#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace {

enum class Tok { End, Ident, Int, Float, Str, Punct };

struct Token {
  Tok t = Tok::End;
  std::string text;   // ident/punct text or string payload
  int64_t i = 0;
  double d = 0;
};

struct Lexer {
  const std::string& s;
  size_t p = 0;
  size_t n;
  std::string err;

  explicit Lexer(const std::string& text) : s(text), n(text.size()) {}

  static bool ident_start(unsigned char c) { return std::isalpha(c) || c == '_'; }
  static bool ident_char(unsigned char c) { return std::isalnum(c) || c == '_'; }

  bool next(Token& out) {
    while (p < n && std::isspace(static_cast<unsigned char>(s[p]))) ++p;
    out = Token{};
    if (p >= n) { out.t = Tok::End; return true; }
    char c = s[p];
    if (ident_start(static_cast<unsigned char>(c))) {
      size_t start = p;
      while (p < n && ident_char(static_cast<unsigned char>(s[p]))) ++p;
      out.t = Tok::Ident;
      out.text = s.substr(start, p - start);
      return true;
    }
    if (std::isdigit(static_cast<unsigned char>(c)) ||
        (c == '-' && p + 1 < n && std::isdigit(static_cast<unsigned char>(s[p + 1])))) {
      size_t start = p;
      if (s[p] == '-') ++p;
      bool is_float = false;
      while (p < n && std::isdigit(static_cast<unsigned char>(s[p]))) ++p;
      if (p < n && s[p] == '.') { is_float = true; ++p; while (p < n && std::isdigit(static_cast<unsigned char>(s[p]))) ++p; }
      if (p < n && (s[p] == 'e' || s[p] == 'E')) {
        is_float = true; ++p;
        if (p < n && (s[p] == '+' || s[p] == '-')) ++p;
        while (p < n && std::isdigit(static_cast<unsigned char>(s[p]))) ++p;
      }
      std::string tmp = s.substr(start, p - start);
      if (is_float) { out.t = Tok::Float; out.d = std::strtod(tmp.c_str(), nullptr); }
      else {
        out.t = Tok::Int;
        errno = 0;
        long long v = std::strtoll(tmp.c_str(), nullptr, 10);
        if (errno == ERANGE) { err = "integer out of range"; return false; }
        out.i = static_cast<int64_t>(v);
      }
      out.text = tmp;
      return true;
    }
    if (c == '\'') {
      ++p;
      std::string val;
      while (p < n) {
        if (s[p] == '\'') {
          if (p + 1 < n && s[p + 1] == '\'') { val.push_back('\''); p += 2; continue; }
          ++p;
          out.t = Tok::Str;
          out.text = std::move(val);
          return true;
        }
        val.push_back(s[p++]);
      }
      err = "unterminated string literal";
      return false;
    }
    out.t = Tok::Punct;
    if (p + 1 < n) {
      std::string two = s.substr(p, 2);
      if (two == "<=" || two == ">=" || two == "<>" || two == "!=") { out.text = two; p += 2; return true; }
    }
    out.text = std::string(1, c);
    ++p;
    return true;
  }
};

struct Parser {
  Lexer lex;
  Token cur;
  Statement st;

  explicit Parser(const std::string& sql) : lex(sql) { advance(); }

  void advance() {
    if (!lex.next(cur)) {
      st.kind = StmtKind::Invalid;
      st.error_code = "syntax_error";
      st.error_message = lex.err;
    }
  }

  bool is_kw(const char* kw) const {
    if (cur.t != Tok::Ident) return false;
    const char* a = cur.text.c_str();
    size_t k = 0;
    for (; kw[k]; ++k) {
      if (a[k] == 0) return false;
      if (std::tolower(static_cast<unsigned char>(a[k])) != std::tolower(static_cast<unsigned char>(kw[k]))) return false;
    }
    return a[k] == 0;
  }

  bool accept_kw(const char* kw) { if (is_kw(kw)) { advance(); return true; } return false; }
  bool accept_punct(const char* p) {
    if (cur.t == Tok::Punct && cur.text == p) { advance(); return true; }
    return false;
  }

  bool fail(const char* code, const std::string& msg) {
    st.kind = StmtKind::Invalid;
    st.error_code = code;
    st.error_message = msg;
    return false;
  }

  bool expect_punct(const char* p) {
    if (accept_punct(p)) return true;
    return fail("syntax_error", std::string("expected ')'. got: ").append(cur.text));
  }

  bool ident(std::string& out) {
    if (cur.t != Tok::Ident) return fail("syntax_error", "expected identifier");
    out = cur.text;
    advance();
    return true;
  }

  bool literal(Literal& out) {
    if (cur.t == Tok::Int) { out.k = Literal::K::Int; out.i = cur.i; advance(); return true; }
    if (cur.t == Tok::Float) { out.k = Literal::K::Float; out.d = cur.d; advance(); return true; }
    if (cur.t == Tok::Str) { out.k = Literal::K::Str; out.s = cur.text; advance(); return true; }
    return fail("syntax_error", "expected literal");
  }

  bool parse_columns() {
    if (!expect_punct("(")) return false;
    while (true) {
      Column col;
      if (!ident(col.name)) return false;
      if (accept_kw("BIGINT")) {
        col.type = ColType::BigInt;
      } else if (accept_kw("VARCHAR")) {
        col.type = ColType::VarChar;
        if (!expect_punct("(")) return false;
        if (cur.t != Tok::Int) return fail("syntax_error", "expected varchar length");
        int64_t len = cur.i;
        advance();
        if (!expect_punct(")")) return false;
        if (len < 1 || len > 65535) return fail("invalid_schema", "varchar length must be 1..65535");
        col.varchar_len = static_cast<int32_t>(len);
      } else if (accept_kw("VECTOR")) {
        col.type = ColType::Vector;
        if (!expect_punct("(")) return false;
        if (cur.t != Tok::Int) return fail("syntax_error", "expected vector dim");
        int64_t dim = cur.i;
        advance();
        if (!expect_punct(")")) return false;
        if (dim < 1 || dim > 4096) return fail("invalid_schema", "vector dimension must be 1..4096");
        col.dim = static_cast<int32_t>(dim);
      } else {
        return fail("syntax_error", "unsupported column type");
      }
      if (accept_kw("PRIMARY")) {
        if (!accept_kw("KEY")) return fail("syntax_error", "expected KEY");
        col.primary = true;
      }
      st.columns.push_back(col);
      if (accept_punct(",")) continue;
      if (accept_punct(")")) break;
      return fail("syntax_error", "expected , or ) in column list");
    }
    return true;
  }

  bool parse_where() {
    while (true) {
      Predicate pred;
      if (!ident(pred.col)) return false;
      if (cur.t != Tok::Punct || (cur.text != "=" && cur.text != "<" && cur.text != "<=" &&
                                 cur.text != ">" && cur.text != ">=" && cur.text != "<>" && cur.text != "!="))
        return fail("syntax_error", "expected comparison operator");
      pred.op = cur.text == "!=" ? "<>" : cur.text;
      advance();
      if (!literal(pred.value)) return false;
      st.where.push_back(std::move(pred));
      if (accept_kw("AND")) continue;
      return true;
    }
  }

  bool parse_select_items() {
    while (true) {
      if (accept_punct("*")) {
        SelectItem item; item.k = SelectItem::K::Star;
        st.items.push_back(item);
      } else if (is_kw("L2_DISTANCE")) {
        advance();
        if (!expect_punct("(")) return false;
        SelectItem item; item.k = SelectItem::K::Distance;
        if (!ident(item.col)) return false;
        if (!accept_punct(",")) return fail("syntax_error", "expected , in L2_DISTANCE");
        if (cur.t != Tok::Str) return fail("syntax_error", "expected vector literal in L2_DISTANCE");
        std::string raw = cur.text;
        advance();
        if (!parse_float_list(raw, item.query)) return false;
        if (!expect_punct(")")) return false;
        if (accept_kw("AS")) {
          if (!ident(item.alias)) return false;
          item.has_alias = true;
        }
        st.items.push_back(std::move(item));
      } else {
        SelectItem item; item.k = SelectItem::K::Column;
        if (!ident(item.col)) return false;
        if (accept_kw("AS")) {
          if (!ident(item.alias)) return false;
          item.has_alias = true;
        }
        st.items.push_back(std::move(item));
      }
      if (accept_punct(",")) continue;
      return true;
    }
  }

  // 严格解析 "[1, 0]" 形态的向量字面量：必须 [ 开头 ] 结尾，元素间恰好一个逗号，
  // 每个元素必须能无损落入 float32 且为有限值。空向量返回空 out，维度由调用方校验。
  bool parse_float_list(const std::string& raw, std::vector<float>& out) {
    out.clear();
    const char* p = raw.c_str();
    const char* end = p + raw.size();
    auto skip_ws = [&p, end]() {
      while (p < end && std::isspace(static_cast<unsigned char>(*p))) ++p;
    };
    auto at_digit = [&p, end]() { return p < end && std::isdigit(static_cast<unsigned char>(*p)) != 0; };

    skip_ws();
    if (p >= end || *p != '[') return fail("invalid_vector", "vector literal must start with '['");
    ++p;
    skip_ws();
    if (p < end && *p == ']') {
      ++p;
      skip_ws();
      if (p != end) return fail("invalid_vector", "trailing characters after vector literal");
      return true;
    }
    while (true) {
      const char* start = p;
      if (p < end && (*p == '+' || *p == '-')) ++p;
      bool digits = false;
      while (at_digit()) { ++p; digits = true; }
      if (p < end && *p == '.') {
        ++p;
        while (at_digit()) { ++p; digits = true; }
      }
      if (!digits) return fail("invalid_vector", "expected number in vector literal");
      if (p < end && (*p == 'e' || *p == 'E')) {
        ++p;
        if (p < end && (*p == '+' || *p == '-')) ++p;
        if (!at_digit()) return fail("invalid_vector", "malformed exponent in vector literal");
        while (at_digit()) ++p;
      }
      std::string num(start, static_cast<size_t>(p - start));
      char* endp = nullptr;
      double v = std::strtod(num.c_str(), &endp);
      if (endp == num.c_str() || *endp != '\0') return fail("invalid_vector", "malformed number: " + num);
      if (!std::isfinite(v) || std::fabs(v) > static_cast<double>(FLT_MAX)) {
        return fail("invalid_vector", "vector element not representable as finite float32: " + num);
      }
      out.push_back(static_cast<float>(v));
      skip_ws();
      if (p < end && *p == ',') {
        ++p;
        skip_ws();
        if (p < end && *p == ']') return fail("invalid_vector", "trailing comma in vector literal");
        continue;
      }
      if (p < end && *p == ']') {
        ++p;
        skip_ws();
        if (p != end) return fail("invalid_vector", "trailing characters after vector literal");
        return true;
      }
      if (p >= end) return fail("invalid_vector", "unterminated vector literal");
      return fail("invalid_vector", "expected ',' or ']' in vector literal");
    }
  }

  bool parse() {
    if (cur.t == Tok::End) return fail("syntax_error", "empty statement");
    if (accept_kw("CREATE")) {
      if (accept_kw("TABLE")) {
        st.kind = StmtKind::CreateTable;
        if (!ident(st.table)) return false;
        return parse_columns();
      }
      if (accept_kw("VECTOR")) {
        st.kind = StmtKind::CreateVectorIndex;
        if (!accept_kw("INDEX")) return fail("syntax_error", "expected INDEX");
        if (!ident(st.index_name)) return false;
        if (!accept_kw("ON")) return fail("syntax_error", "expected ON");
        if (!ident(st.table)) return false;
        if (!expect_punct("(")) return false;
        if (!ident(st.index_col)) return false;
        return expect_punct(")");
      }
      return fail("unsupported_statement", "unsupported CREATE form");
    }
    if (accept_kw("INSERT")) {
      st.kind = StmtKind::Insert;
      if (!accept_kw("INTO")) return fail("syntax_error", "expected INTO");
      if (!ident(st.table)) return false;
      if (!accept_kw("VALUES")) return fail("syntax_error", "expected VALUES");
      while (true) {
        if (!expect_punct("(")) return false;
        std::vector<Literal> row;
        while (true) {
          Literal lit;
          if (!literal(lit)) return false;
          row.push_back(std::move(lit));
          if (accept_punct(",")) continue;
          if (accept_punct(")")) break;
          return fail("syntax_error", "expected , or ) in VALUES");
        }
        st.rows.push_back(std::move(row));
        if (accept_punct(",")) continue;
        break;
      }
      return true;
    }
    if (accept_kw("SELECT")) {
      st.kind = StmtKind::Select;
      if (!parse_select_items()) return false;
      if (!accept_kw("FROM")) return fail("syntax_error", "expected FROM");
      if (!ident(st.table)) return false;
      if (accept_kw("WHERE")) { if (!parse_where()) return false; }
      if (accept_kw("ORDER")) {
        if (!accept_kw("BY")) return fail("syntax_error", "expected BY");
        while (true) {
          OrderKey key;
          if (!ident(key.name)) return false;
          if (accept_kw("DESC")) key.asc = false;
          else accept_kw("ASC");
          st.order.push_back(std::move(key));
          if (accept_punct(",")) continue;
          break;
        }
      }
      if (accept_kw("LIMIT")) {
        if (cur.t != Tok::Int) return fail("syntax_error", "expected integer LIMIT");
        st.limit = cur.i;
        advance();
      }
      return true;
    }
    if (accept_kw("UPDATE")) {
      st.kind = StmtKind::Update;
      if (!ident(st.table)) return false;
      if (!accept_kw("SET")) return fail("syntax_error", "expected SET");
      while (true) {
        std::string col;
        if (!ident(col)) return false;
        if (!accept_punct("=")) return fail("syntax_error", "expected = in SET");
        Literal lit;
        if (!literal(lit)) return false;
        st.set_pairs.emplace_back(std::move(col), std::move(lit));
        if (accept_punct(",")) continue;
        break;
      }
      if (accept_kw("WHERE")) { if (!parse_where()) return false; }
      return true;
    }
    if (accept_kw("DELETE")) {
      st.kind = StmtKind::Delete;
      if (!accept_kw("FROM")) return fail("syntax_error", "expected FROM");
      if (!ident(st.table)) return false;
      if (accept_kw("WHERE")) { if (!parse_where()) return false; }
      return true;
    }
    if (accept_kw("BEGIN")) { st.kind = StmtKind::Begin; return true; }
    if (accept_kw("COMMIT")) { st.kind = StmtKind::Commit; return true; }
    if (accept_kw("ROLLBACK")) { st.kind = StmtKind::Rollback; return true; }
    return fail("unsupported_statement", "unsupported statement: " + cur.text);
  }
};

}  // namespace

Statement parse_sql(const std::string& sql) {
  Parser parser(sql);
  if (!parser.parse()) return parser.st;
  if (parser.cur.t != Tok::End) {
    if (!(parser.cur.t == Tok::Punct && parser.cur.text == ";")) {
      parser.st.kind = StmtKind::Invalid;
      parser.st.error_code = "syntax_error";
      parser.st.error_message = "trailing tokens after statement";
    }
  }
  return parser.st;
}
