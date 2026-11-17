#include "parser.h"

#include <charconv>
#include <cfloat>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace {

enum class Tok { End, Ident, Int, Float, Str, Punct };

// 字符分类查表：把逐字符的 ctype 调用换成一次查表（语义与 C locale 下的 isspace/isalpha/isalnum 一致）。
struct CharTables {
  unsigned char space[256];
  unsigned char ident_start[256];
  unsigned char ident_char[256];
  unsigned char digit[256];
  unsigned char lower[256];

  constexpr CharTables()
      : space{}, ident_start{}, ident_char{}, digit{}, lower{} {
    for (int i = 0; i < 256; ++i) {
      space[i] = (i == ' ' || i == '\t' || i == '\n' || i == '\v' || i == '\f' || i == '\r') ? 1 : 0;
      digit[i] = (i >= '0' && i <= '9') ? 1 : 0;
      ident_start[i] = ((i >= 'a' && i <= 'z') || (i >= 'A' && i <= 'Z') || i == '_') ? 1 : 0;
      ident_char[i] = (ident_start[i] || digit[i]) ? 1 : 0;
      lower[i] = (i >= 'A' && i <= 'Z') ? static_cast<unsigned char>(i - 'A' + 'a')
                                        : static_cast<unsigned char>(i);
    }
  }
};
constexpr CharTables kCt{};

// token 载荷一律是指向源缓冲（或 lexer 转义缓冲）的视图，切词阶段不做堆分配。
struct Token {
  Tok t = Tok::End;
  const char* b = nullptr;
  uint32_t n = 0;
  int64_t i = 0;
  double d = 0;
  char p2[2] = {0, 0};   // Punct 文本（1~2 字符）

  std::string_view view() const {
    if (t == Tok::Punct) return std::string_view(p2, n);
    return std::string_view(b ? b : "", n);
  }
};

struct Lexer {
  const std::string& s;
  const char* base;
  size_t p = 0;
  size_t n;
  std::string err;
  std::string scratch;   // 含 '' 转义的字符串字面量载荷

  explicit Lexer(const std::string& text) : s(text), base(text.data()), n(text.size()) {}

  bool next(Token& out) {
    while (p < n && kCt.space[static_cast<unsigned char>(base[p])]) ++p;
    out = Token{};
    if (p >= n) { out.t = Tok::End; return true; }
    const char c = base[p];
    const unsigned char uc = static_cast<unsigned char>(c);
    if (kCt.ident_start[uc]) {
      const size_t start = p;
      while (p < n && kCt.ident_char[static_cast<unsigned char>(base[p])]) ++p;
      out.t = Tok::Ident;
      out.b = base + start;
      out.n = static_cast<uint32_t>(p - start);
      return true;
    }
    if (kCt.digit[uc] || (c == '-' && p + 1 < n && kCt.digit[static_cast<unsigned char>(base[p + 1])])) {
      const size_t start = p;
      if (base[p] == '-') ++p;
      bool is_float = false;
      while (p < n && kCt.digit[static_cast<unsigned char>(base[p])]) ++p;
      if (p < n && base[p] == '.') { is_float = true; ++p; while (p < n && kCt.digit[static_cast<unsigned char>(base[p])]) ++p; }
      if (p < n && (base[p] == 'e' || base[p] == 'E')) {
        is_float = true; ++p;
        if (p < n && (base[p] == '+' || base[p] == '-')) ++p;
        while (p < n && kCt.digit[static_cast<unsigned char>(base[p])]) ++p;
      }
      const size_t len = p - start;
      char buf[64];
      const char* digits = base + start;
      if (len >= sizeof(buf)) {
        // 超长数字极罕见：交给 std::string 兜底，语义与短路径一致。
        scratch.assign(base + start, len);
        digits = scratch.data();
      } else {
        std::memcpy(buf, base + start, len);
        buf[len] = '\0';
        digits = buf;
      }
      if (is_float) {
        out.t = Tok::Float;
        out.d = std::strtod(digits, nullptr);
      } else {
        out.t = Tok::Int;
        errno = 0;
        const long long v = std::strtoll(digits, nullptr, 10);
        if (errno == ERANGE) { err = "integer out of range"; return false; }
        out.i = static_cast<int64_t>(v);
      }
      out.b = base + start;
      out.n = static_cast<uint32_t>(len);
      return true;
    }
    if (c == '\'') {
      ++p;
      const size_t start = p;
      size_t scan = p;
      bool escaped = false;
      while (scan < n) {
        if (base[scan] == '\'') {
          if (scan + 1 < n && base[scan + 1] == '\'') { escaped = true; scan += 2; continue; }
          break;
        }
        ++scan;
      }
      if (scan >= n) { err = "unterminated string literal"; return false; }
      if (!escaped) {
        out.t = Tok::Str;
        out.b = base + start;
        out.n = static_cast<uint32_t>(scan - start);
        p = scan + 1;
        return true;
      }
      scratch.clear();
      for (size_t i = start; i < scan;) {
        if (base[i] == '\'' && i + 1 < scan && base[i + 1] == '\'') { scratch.push_back('\''); i += 2; }
        else scratch.push_back(base[i++]);
      }
      out.t = Tok::Str;
      out.b = scratch.data();
      out.n = static_cast<uint32_t>(scratch.size());
      p = scan + 1;
      return true;
    }
    out.t = Tok::Punct;
    if (p + 1 < n) {
      const char c0 = base[p];
      const char c1 = base[p + 1];
      if ((c0 == '<' && (c1 == '=' || c1 == '>')) || (c0 == '>' && c1 == '=') ||
          (c0 == '!' && c1 == '=')) {
        out.p2[0] = c0;
        out.p2[1] = c1;
        out.n = 2;
        p += 2;
        return true;
      }
    }
    out.p2[0] = c;
    out.n = 1;
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
    const std::string_view v = cur.view();
    size_t k = 0;
    for (; kw[k]; ++k) {
      if (k >= v.size()) return false;
      if (kCt.lower[static_cast<unsigned char>(v[k])] != kCt.lower[static_cast<unsigned char>(kw[k])]) return false;
    }
    return k == v.size();
  }

  bool accept_kw(const char* kw) { if (is_kw(kw)) { advance(); return true; } return false; }

  bool accept_punct(const char* p) {
    if (cur.t != Tok::Punct) return false;
    if (cur.view() != std::string_view(p)) return false;
    advance();
    return true;
  }

  bool fail(const char* code, const std::string& msg) {
    st.kind = StmtKind::Invalid;
    st.error_code = code;
    st.error_message = msg;
    return false;
  }

  bool expect_punct(const char* p) {
    if (accept_punct(p)) return true;
    std::string msg = "expected ')'. got: ";
    const std::string_view v = cur.view();
    msg.append(v.data(), v.size());
    return fail("syntax_error", msg);
  }

  bool ident(std::string& out) {
    if (cur.t != Tok::Ident) return fail("syntax_error", "expected identifier");
    const std::string_view v = cur.view();
    out.assign(v.data(), v.size());
    advance();
    return true;
  }

  bool literal(Literal& out) {
    if (cur.t == Tok::Int) { out.k = Literal::K::Int; out.i = cur.i; advance(); return true; }
    if (cur.t == Tok::Float) { out.k = Literal::K::Float; out.d = cur.d; advance(); return true; }
    if (cur.t == Tok::Str) {
      out.k = Literal::K::Str;
      const std::string_view v = cur.view();
      out.s.assign(v.data(), v.size());
      advance();
      return true;
    }
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
        const int64_t len = cur.i;
        advance();
        if (!expect_punct(")")) return false;
        if (len < 1 || len > 65535) return fail("invalid_schema", "varchar length must be 1..65535");
        col.varchar_len = static_cast<int32_t>(len);
      } else if (accept_kw("VECTOR")) {
        col.type = ColType::Vector;
        if (!expect_punct("(")) return false;
        if (cur.t != Tok::Int) return fail("syntax_error", "expected vector dim");
        const int64_t dim = cur.i;
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
      if (cur.t != Tok::Punct) return fail("syntax_error", "expected comparison operator");
      const std::string_view op = cur.view();
      if (op != "=" && op != "<" && op != "<=" && op != ">" && op != ">=" && op != "<>" && op != "!=")
        return fail("syntax_error", "expected comparison operator");
      pred.op.assign(op.data(), op.size());
      if (pred.op == "!=") pred.op = "<>";
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
        if (!parse_float_list(cur.view(), item.query)) return false;
        advance();
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
  bool parse_float_list(std::string_view raw, std::vector<float>& out) {
    out.clear();
    out.reserve(raw.size() / 8 + 8);   // 逐元素 push_back 的重分配对 128 维查询可见，按字符数粗估容量
    const char* p = raw.data();
    const char* const end = p + raw.size();
    auto skip_ws = [&p, end]() {
      while (p < end && kCt.space[static_cast<unsigned char>(*p)]) ++p;
    };
    auto at_digit = [&p, end]() { return p < end && kCt.digit[static_cast<unsigned char>(*p)] != 0; };

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
      const size_t len = static_cast<size_t>(p - start);
      double v = 0;
      if (len < 64) {
        char buf[64];
        std::memcpy(buf, start, len);
        buf[len] = '\0';
        // 快路径 from_chars：与 strtod 同为正确舍入，值逐位一致；不接受的写法（前导 '+'、越界、
        // 下溢/上溢）一律回落到 strtod，保证与旧实现的取值和报错完全一致。
        const auto fc = std::from_chars(buf, buf + len, v, std::chars_format::general);
        if (fc.ec != std::errc() || fc.ptr != buf + len) {
          char* endp = nullptr;
          v = std::strtod(buf, &endp);
          if (endp == buf || *endp != '\0')
            return fail("invalid_vector", std::string("malformed number: ").append(start, len));
        }
      } else {
        char* endp = nullptr;
        const std::string num(start, len);
        v = std::strtod(num.c_str(), &endp);
        if (endp == num.c_str() || *endp != '\0')
          return fail("invalid_vector", "malformed number: " + num);
      }
      if (!std::isfinite(v) || std::fabs(v) > static_cast<double>(FLT_MAX)) {
        return fail("invalid_vector", std::string("vector element not representable as finite float32: ").append(start, len));
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
    std::string msg = "unsupported statement: ";
    const std::string_view v = cur.view();
    msg.append(v.data(), v.size());
    return fail("unsupported_statement", msg);
  }
};

}  // namespace

Statement parse_sql(const std::string& sql) {
  Parser parser(sql);
  if (!parser.parse()) return parser.st;
  if (parser.cur.t != Tok::End) {
    if (!(parser.cur.t == Tok::Punct && parser.cur.view() == std::string_view(";"))) {
      parser.st.kind = StmtKind::Invalid;
      parser.st.error_code = "syntax_error";
      parser.st.error_message = "trailing tokens after statement";
    }
  }
  return parser.st;
}
