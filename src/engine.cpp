#include "engine.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <charconv>
#include <cmath>
#include <system_error>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "vector.h"

namespace {

constexpr size_t kChunkMutations = 256;   // 单条 UPDATE/DELETE 每批落地/应用的行数

// 行缓存上限：MVDB_ROW_CACHE_MB 优先；否则按 MVDB_MEM_LIMIT_MB 的 1/16（限幅 8~64 MiB）；
// 都没设时 64 MiB —— 2 GiB 容器下这一项不会把进程顶到 OOM。
size_t row_cache_budget_from_env() {
  auto read_mb = [](const char* name, long long dflt) -> long long {
    const char* s = std::getenv(name);
    if (!s || !*s) return dflt;
    char* end = nullptr;
    const long long v = std::strtoll(s, &end, 10);
    if (end == s || v < 0) return dflt;
    return v;
  };
  const long long explicit_mb = read_mb("MVDB_ROW_CACHE_MB", -1);
  if (explicit_mb >= 0) return static_cast<size_t>(explicit_mb) << 20;
  const long long limit_mb = read_mb("MVDB_MEM_LIMIT_MB", 0);
  if (limit_mb > 0) {
    long long c = limit_mb / 16;
    if (c < 8) c = 8;
    if (c > 64) c = 64;
    return static_cast<size_t>(c) << 20;
  }
  return size_t{64} << 20;
}

// 向量行镜像 arena 的预算：MVDB_VEC_ARENA_MB（默认 64 MiB，0 = 关闭）。
// 默认值按"外存可扩展性优先"取：流式路径下的常驻 RSS 已约 77~109 MiB（1.5M 行实测），
// 再加 64 MiB arena 仍远低于 200 MiB 上限；整表行负载 + 索引估计超过预算时 arena
// 整体不启用，查询照旧走 WAL 流式扫描。
size_t vec_arena_budget_from_env() {
  const size_t dflt = size_t{64} << 20;
  const char* s = std::getenv("MVDB_VEC_ARENA_MB");
  if (!s || !*s) return dflt;
  char* end = nullptr;
  const long long v = std::strtoll(s, &end, 10);
  if (end == s || v < 0) return dflt;
  return static_cast<size_t>(v) << 20;
}

size_t utf8_length(const std::string& s) {
  size_t n = 0;
  for (unsigned char c : s)
    if ((c & 0xC0) != 0x80) ++n;
  return n;
}

// 与 parser.cpp 的 parse_float_list 同一套严格文法：必须 '[' 开头 ']' 结尾，元素间恰好一个逗号，
// 元素必须有限且可无损落入 float32。连续逗号/尾随逗号/缺逗号/未闭合/']' 之后有字符一律拒绝。
// 允许首尾与分隔符两侧空白；"[]" 得到空向量（维度由调用方判 dimension_mismatch）。
bool parse_vector_text(const std::string& raw, std::vector<float>& out, std::string& code) {
  out.clear();
  const char* p = raw.c_str();
  const char* end = p + raw.size();
  auto skip_ws = [&p, end]() {
    while (p < end && std::isspace(static_cast<unsigned char>(*p))) ++p;
  };
  auto at_digit = [&p, end]() { return p < end && std::isdigit(static_cast<unsigned char>(*p)) != 0; };
  auto bad = [&code]() { code = "invalid_vector"; return false; };

  skip_ws();
  if (p >= end || *p != '[') return bad();
  ++p;
  skip_ws();
  if (p < end && *p == ']') {
    ++p;
    skip_ws();
    return p == end ? true : bad();
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
    if (!digits) return bad();
    if (p < end && (*p == 'e' || *p == 'E')) {
      ++p;
      if (p < end && (*p == '+' || *p == '-')) ++p;
      if (!at_digit()) return bad();
      while (at_digit()) ++p;
    }
    // 文法已在上面限死，这里只用等价的无分配数字解析：
    // from_chars 不接受前导 '+'，先跳过；解析失败/越界（1e400 之类）退回 strtod 复核，
    // 保证与原先 strtod + isfinite + |v|<=FLT_MAX 完全一致（含下溢到 0 的极小值）。
    const char* num = *start == '+' ? start + 1 : start;
    double v = 0;
    const auto fc = std::from_chars(num, p, v, std::chars_format::general);
    if (fc.ec != std::errc() || fc.ptr != p) {
      const std::string slow(start, static_cast<size_t>(p - start));
      char* num_end = nullptr;
      v = std::strtod(slow.c_str(), &num_end);
      if (num_end == slow.c_str() || *num_end != '\0') return bad();
    }
    if (!std::isfinite(v) || std::fabs(v) > static_cast<double>(FLT_MAX)) return bad();
    out.push_back(static_cast<float>(v));
    skip_ws();
    if (p < end && *p == ',') {
      ++p;
      skip_ws();
      if (p < end && *p == ']') return bad();        // 尾随逗号
      continue;
    }
    if (p < end && *p == ']') {
      ++p;
      skip_ws();
      return p == end ? true : bad();                // ']' 之后不允许有字符
    }
    return bad();                                    // 未闭合，或元素间缺逗号
  }
}

bool coerce(const Literal& lit, const Column& col, Value& out, std::string& code) {
  // 浮点字面量必须先有限：1e400 这类会以 inf 进入比较/写入语义，与任何列类型都不成立。
  if (lit.k == Literal::K::Float && !std::isfinite(lit.d)) {
    code = "invalid_number";
    return false;
  }
  switch (col.type) {
    case ColType::BigInt: {
      if (lit.k == Literal::K::Int) { out = Value::Int(lit.i); return true; }
      code = "type_mismatch";
      return false;
    }
    case ColType::VarChar: {
      if (lit.k != Literal::K::Str) { code = "type_mismatch"; return false; }
      if (col.varchar_len > 0 && utf8_length(lit.s) > static_cast<size_t>(col.varchar_len)) {
        code = "value_too_long";
        return false;
      }
      out = Value::Str(lit.s);
      return true;
    }
    case ColType::Vector: {
      if (lit.k != Literal::K::Str) { code = "type_mismatch"; return false; }
      std::vector<float> vec;
      std::string err;
      if (!parse_vector_text(lit.s, vec, err)) { code = err; return false; }
      if (static_cast<int32_t>(vec.size()) != col.dim) { code = "dimension_mismatch"; return false; }
      out = Value::Vec(std::move(vec));
      return true;
    }
  }
  code = "type_mismatch";
  return false;
}

int compare_scalar(const Value& a, const Value& b) {
  if (a.k != b.k) return a.k == Value::K::Int ? -1 : 1;
  switch (a.k) {
    case Value::K::Int: return a.i < b.i ? -1 : (a.i > b.i ? 1 : 0);
    case Value::K::Float: return a.d < b.d ? -1 : (a.d > b.d ? 1 : 0);
    case Value::K::Str: {
      int c = a.s.compare(b.s);
      return c < 0 ? -1 : (c > 0 ? 1 : 0);
    }
    default: return 0;
  }
}

// 行负载视图 → Value（需要物化时才调用，会分配）
Value value_of_view(const ValueView& v) {
  switch (v.tag) {
    case 1:   // int
      return Value::Int(view_int(v));
    case 2: {  // float
      double d = 0;
      std::memcpy(&d, v.data, sizeof(d));
      return Value::Float(d);
    }
    case 3:   // str
      return Value::Str(std::string(reinterpret_cast<const char*>(v.data), v.n));
    case 4: {  // vec
      std::vector<float> xs(v.n);
      if (v.n) std::memcpy(xs.data(), v.data, static_cast<size_t>(v.n) * sizeof(float));
      return Value::Vec(std::move(xs));
    }
    default:
      return Value{};
  }
}

// 与 compare_scalar 同语义（tag 与 Value::K 的序号一致：0 Null/1 Int/2 Float/3 Str/4 Vec），
// 左值直接取自行负载视图（不分配）
int compare_view_scalar(const ValueView& v, const Value& rhs) {
  if (v.tag != static_cast<uint8_t>(rhs.k)) return v.tag == 1 ? -1 : 1;
  switch (v.tag) {
    case 1: {
      const int64_t x = view_int(v);
      return x < rhs.i ? -1 : (x > rhs.i ? 1 : 0);
    }
    case 2: {
      double d = 0;
      std::memcpy(&d, v.data, sizeof(d));
      return d < rhs.d ? -1 : (d > rhs.d ? 1 : 0);
    }
    case 3: {
      const size_t n = v.n < rhs.s.size() ? v.n : rhs.s.size();
      int c = n ? std::memcmp(v.data, rhs.s.data(), n) : 0;
      if (c == 0) c = v.n < rhs.s.size() ? -1 : (v.n > rhs.s.size() ? 1 : 0);
      return c < 0 ? -1 : (c > 0 ? 1 : 0);
    }
    default:
      return 0;   // Null / Vec：compare_scalar 只可能得到相等
  }
}

// 谓词算符在扫描前一次性编成枚举：热路径里不再逐行比较 std::string。
enum class PredOp : uint8_t { Eq = 0, Ne, Lt, Le, Gt, Ge, None };

PredOp pred_op_of(const std::string& op) {
  if (op == "=") return PredOp::Eq;
  if (op == "<>") return PredOp::Ne;
  if (op == "<") return PredOp::Lt;
  if (op == "<=") return PredOp::Le;
  if (op == ">") return PredOp::Gt;
  if (op == ">=") return PredOp::Ge;
  return PredOp::None;
}

inline bool eval_pred_op(const ValueView& v, PredOp op, const Value& rhs) {
  const int c = compare_view_scalar(v, rhs);
  switch (op) {
    case PredOp::Eq: return c == 0;
    case PredOp::Ne: return c != 0;
    case PredOp::Lt: return c < 0;
    case PredOp::Le: return c <= 0;
    case PredOp::Gt: return c > 0;
    case PredOp::Ge: return c >= 0;
    default: return false;
  }
}

}  // namespace

// ==================================================================== 恢复
// 只重建 pk→槽位 与存活位：行内容一律不回读，恢复期内存不随表大小线性膨胀。
class Engine::RecoverySink : public Storage::GroupSink {
 public:
  explicit RecoverySink(Engine& e) : e_(e) {
    for (auto& kv : e_.tables_) by_name_.push_back(Entry{kv.first, &kv.second});
  }

  void begin_group(uint64_t) override { journal_.clear(); }

  void record(const Storage::View& v) override {
    PayloadView pv;
    if (!Storage::decode_payload(v.payload, v.payload_len, pv)) return;
    Table* t = lookup(pv.table, pv.table_len);
    if (!t) return;
    const Schema& s = t->schema;
    if (s.pk_col < 0 || pv.cols.size() != s.cols.size()) return;
    const ValueView& pkv = pv.cols[static_cast<size_t>(s.pk_col)];
    if (pkv.tag != 1) return;
    const int64_t pk = view_int(pkv);
    uint32_t rowid = 0;
    const bool exists = t->store.find(pk, rowid);
    Journal j;
    j.t = t;
    j.pk = pk;
    j.exists = exists;
    if (exists) {
      j.rowid = rowid;
      j.off = t->store.offset(rowid);
      j.len = t->store.length(rowid);
      j.alive = t->store.alive(rowid);
    }
    journal_.push_back(j);
    if (v.type == 3) {                       // Delete
      if (exists) {
        t->store.set_alive(rowid, false);
        t->store.unbind(pk);
      }
      return;
    }
    if (exists) {                            // Update：同一个行号换新版本
      t->store.set_slot(rowid, v.offset, v.len, true);
      return;
    }
    const uint32_t rid = t->store.append_slot(v.offset, v.len, true);
    t->store.bind(pk, rid);
  }

  void commit_group() override { journal_.clear(); }

  // 未提交的尾部整组回退：恢复期只在内存里改过元数据，回退即复原
  void abort_group() override {
    for (size_t i = journal_.size(); i > 0; --i) {
      const Journal& j = journal_[i - 1];
      if (j.exists) {
        j.t->store.set_slot(j.rowid, j.off, j.len, j.alive);
        j.t->store.bind(j.pk, j.rowid);
      } else {
        j.t->store.unbind(j.pk);
        const uint32_t n = j.t->store.slot_count();
        if (n > 0) j.t->store.truncate_slots(n - 1);
      }
    }
    journal_.clear();
  }

 private:
  struct Entry {
    std::string name;
    Table* t;
  };
  struct Journal {
    Table* t = nullptr;
    uint64_t off = 0;
    int64_t pk = 0;
    uint32_t rowid = 0;
    uint32_t len = 0;
    bool exists = false;
    bool alive = false;
  };

  Table* lookup(const char* name, uint32_t len) {
    for (Entry& e : by_name_)
      if (e.name.size() == len && std::memcmp(e.name.data(), name, len) == 0) return e.t;
    return nullptr;
  }

  Engine& e_;
  std::vector<Entry> by_name_;
  std::vector<Journal> journal_;
};

// ==================================================================== 生命周期
bool Engine::open(const std::string& data_dir, std::string& err) {
  err.clear();
  if (!storage_.open(data_dir, err)) return false;
  storage_open_ = true;

  std::vector<Schema> schemas;
  if (!storage_.load_catalog(schemas, err)) {
    close();
    return false;
  }
  tables_.clear();
  cache_budget_ = row_cache_budget_from_env();
  vec_arena_budget_ = vec_arena_budget_from_env();
  for (const Schema& s : schemas) {
    Table t;
    t.schema = s;
    t.store.init(&storage_);
    t.store.set_cache_budget(cache_budget_);
    t.store.configure_arena(static_cast<int>(s.cols.size()), s.vec_col,
                             s.vec_col >= 0 ? s.cols[static_cast<size_t>(s.vec_col)].dim : 0,
                             vec_arena_budget_);
    tables_.emplace(s.table, std::move(t));
  }

  // 流式重放：只重建索引，不把整表读进内存；未提交尾部整组丢弃。
  RecoverySink sink(*this);
  uint64_t committed_end = 0;
  if (!storage_.replay_groups(sink, &committed_end, err)) {
    close();
    return false;
  }
  // 截到最后一个提交边界：运行期「已落地即已提交」，偏移不会被未提交数据复用。
  if (!storage_.truncate(committed_end, err)) {
    close();
    return false;
  }
  return true;
}

void Engine::close() {
  if (!storage_open_) return;
  storage_.close();
  storage_open_ = false;
}

bool Engine::persist_catalog(std::string& err) {
  std::vector<Schema> schemas;
  schemas.reserve(tables_.size());
  for (const auto& kv : tables_) schemas.push_back(kv.second.schema);
  return storage_.save_catalog(schemas, err);
}

Engine::Table* Engine::table(const std::string& name) {
  auto it = tables_.find(name);
  return it == tables_.end() ? nullptr : &it->second;
}

// ==================================================================== 语句落地协议
Engine::StmtScope Engine::begin_stmt() {
  StmtScope sc;
  sc.file = storage_.file_end();
  sc.undo = undo_.size();
  sc.txn = in_txn_ ? txn_id_ : storage_.begin_txn();
  sc.touched = false;
  return sc;
}

Result Engine::append_apply(Table& t, std::vector<Mutation>& muts, StmtScope& sc) {
  if (muts.empty()) return Result::Affected(0);
  std::vector<RecordLoc> locs;
  std::string err;
  if (!storage_.append(muts, sc.txn, &locs, err)) return Result::Fail("storage_error", err);
  // 先落地再看内存：行内容必须可被扫描到，内存态才允许生效
  if (!storage_.flush(err)) return Result::Fail("storage_error", err);
  for (size_t k = 0; k < muts.size(); ++k) {
    const Mutation& m = muts[k];
    const int64_t pk = m.row[static_cast<size_t>(t.schema.pk_col)].i;
    switch (m.op) {
      case Mutation::Op::Insert: apply_insert(t, pk, locs[k], &m.row); break;
      case Mutation::Op::Update: apply_update(t, pk, locs[k], &m.row); break;
      case Mutation::Op::Delete: apply_delete(t, pk); break;
    }
  }
  sc.touched = true;
  if (in_txn_) txn_dirty_ = true;
  return Result::Affected(static_cast<int64_t>(muts.size()));
}

Result Engine::end_stmt(StmtScope& sc, int64_t affected) {
  if (in_txn_) return Result::Affected(affected);   // 事务内：记录已在刷出的日志里，COMMIT 时再落盘提交
  if (sc.touched) {
    std::string err;
    if (!storage_.commit(sc.txn, err)) {
      undo_to(sc.undo);
      std::string ignore;
      storage_.rollback_to(sc.file, ignore);
      for (auto& kv : tables_) kv.second.store.forget_cache();
      return Result::Fail("storage_error", err);
    }
  }
  undo_.resize(sc.undo);
  return Result::Affected(affected);
}

Result Engine::abort_stmt(StmtScope& sc, const Result& r) {
  undo_to(sc.undo);
  std::string ignore;
  storage_.rollback_to(sc.file, ignore);
  for (auto& kv : tables_) kv.second.store.forget_cache();   // 偏移会被复用，按偏移索引的缓存必须作废
  return r;
}

// ==================================================================== 内存态变更 + undo
void Engine::apply_insert(Table& t, int64_t pk, const RecordLoc& loc, const Row* row) {
  const uint32_t rowid = t.store.append_slot(loc.offset, loc.len, true);
  UndoEntry e;
  e.k = static_cast<uint8_t>(UndoEntry::K::Insert);
  e.t = &t;
  e.pk = pk;
  e.rowid = rowid;
  undo_.push_back(e);
  t.store.bind(pk, rowid);
  if (row) t.store.arena_put(rowid, *row);   // arena 未就绪时是空操作（下次构建覆盖）
}

void Engine::apply_update(Table& t, int64_t pk, const RecordLoc& loc, const Row* row) {
  uint32_t rowid = 0;
  if (!t.store.find(pk, rowid)) return;
  UndoEntry e;
  e.k = static_cast<uint8_t>(UndoEntry::K::Update);
  e.t = &t;
  e.pk = pk;
  e.rowid = rowid;
  e.prev_off = t.store.offset(rowid);
  e.prev_len = t.store.length(rowid);
  e.prev_alive = t.store.alive(rowid) ? 1 : 0;
  undo_.push_back(e);
  t.store.set_slot(rowid, loc.offset, loc.len, true);
  if (row) t.store.arena_put(rowid, *row);
}

void Engine::apply_delete(Table& t, int64_t pk) {
  uint32_t rowid = 0;
  if (!t.store.find(pk, rowid)) return;
  UndoEntry e;
  e.k = static_cast<uint8_t>(UndoEntry::K::Delete);
  e.t = &t;
  e.pk = pk;
  e.rowid = rowid;
  e.prev_off = t.store.offset(rowid);
  e.prev_len = t.store.length(rowid);
  e.prev_alive = 1;
  undo_.push_back(e);
  t.store.set_alive(rowid, false);
  t.store.unbind(pk);
}

void Engine::undo_to(size_t mark) {
  for (size_t i = undo_.size(); i > mark; --i) {
    const UndoEntry& e = undo_[i - 1];
    RowStore& st = e.t->store;
    switch (static_cast<UndoEntry::K>(e.k)) {
      case UndoEntry::K::Insert:
        st.unbind(e.pk);
        if (e.rowid + 1 == st.slot_count()) st.truncate_slots(e.rowid);
        else st.set_alive(e.rowid, false);
        break;
      case UndoEntry::K::Update:
      case UndoEntry::K::Delete:
        st.set_slot(e.rowid, e.prev_off, e.prev_len, e.prev_alive != 0);
        st.bind(e.pk, e.rowid);
        break;
    }
  }
  if (undo_.size() > mark) undo_.resize(mark);
  // 撤销改回了旧版本/收缩了槽位，arena 里的对应记录已过期：置脏，下次查询按日志重建。
  for (auto& kv : tables_) kv.second.store.arena_invalidate();
}

// ==================================================================== 扫描
template <typename Fn>
bool Engine::scan_table(Table& t, Fn&& fn, std::string& err) {
  const Schema& schema = t.schema;
  const std::string& tname = schema.table;
  const uint64_t scan_end = storage_.file_end();   // 语句开始后追加的版本不属于本次扫描
  PayloadView pv;
  return storage_.scan([&](const Storage::View& v) -> bool {
    if (v.offset >= scan_end) return false;
    if (!Storage::decode_payload(v.payload, v.payload_len, pv)) return false;
    if (pv.table_len != tname.size() ||
        std::memcmp(pv.table, tname.data(), pv.table_len) != 0)
      return true;
    if (pv.cols.size() != schema.cols.size() || schema.pk_col < 0) return true;
    const ValueView& pkv = pv.cols[static_cast<size_t>(schema.pk_col)];
    if (pkv.tag != 1) return true;
    const int64_t pk = view_int(pkv);
    uint32_t rowid = 0;
    if (!t.store.find(pk, rowid)) return true;             // 已删除
    if (t.store.offset(rowid) != v.offset) return true;    // 旧版本（已被更新覆盖）
    return fn(rowid, pk, pv.cols);
  }, err);
}

// 按预算把整表行负载镜像进 arena（一次性顺序扫日志）。只在脏标记下做；
// 估计或实际超预算就永久关闭，后续查询一律走流式扫描。
bool Engine::arena_refresh(Table& t, std::string& err) {
  if (t.schema.vec_col < 0 || t.store.arena_ready() || t.store.arena_disabled()) return true;
  if (!t.store.arena_may_build()) {
    t.store.arena_disable();
    return true;
  }
  const Schema& schema = t.schema;
  const std::string& tname = schema.table;
  const uint64_t scan_end = storage_.file_end();
  bool over_budget = false;
  bool malformed = false;
  PayloadView pv;
  t.store.arena_begin_build();
  const bool ok = storage_.scan([&](const Storage::View& v) -> bool {
    if (v.offset >= scan_end) return false;
    if (!Storage::decode_payload(v.payload, v.payload_len, pv)) {
      malformed = true;
      return false;
    }
    if (pv.table_len != tname.size() || std::memcmp(pv.table, tname.data(), pv.table_len) != 0)
      return true;
    if (pv.cols.size() != schema.cols.size() || schema.pk_col < 0) {
      malformed = true;
      return false;
    }
    const ValueView& pkv = pv.cols[static_cast<size_t>(schema.pk_col)];
    if (pkv.tag != 1) return true;
    const int64_t pk = view_int(pkv);
    uint32_t rowid = 0;
    if (!t.store.find(pk, rowid)) return true;             // 已删除
    if (t.store.offset(rowid) != v.offset) return true;    // 旧版本
    if (!t.store.arena_put_views(rowid, pv.cols)) {
      over_budget = true;
      return false;
    }
    return true;
  }, err);
  t.store.arena_finish_build();
  if (over_budget) return true;      // 已关闭，退回流程式路径
  if (!ok) return false;             // 读错误
  if (malformed) {
    t.store.arena_disable();         // 结构异常：镜像不可信，整个退回（与流式路径同语义）
    return true;
  }
  return true;
}

template <typename Fn>
bool Engine::for_each_hit(Table& t, const std::vector<std::pair<int, Value>>& bound,
                          const std::vector<Predicate>& where, Fn&& fn, std::string& err) {
  const Schema& schema = t.schema;
  bool have_pk = false;
  int64_t pk_eq = 0;
  for (size_t k = 0; k < bound.size(); ++k) {
    if (bound[k].first == schema.pk_col && where[k].op == "=" &&
        bound[k].second.k == Value::K::Int) {
      have_pk = true;
      pk_eq = bound[k].second.i;
    }
  }
  auto pass = [&](const std::vector<ValueView>& cols) {
    for (size_t k = 0; k < bound.size(); ++k) {
      if (!eval_pred_op(cols[static_cast<size_t>(bound[k].first)], pred_op_of(where[k].op),
                        bound[k].second))
        return false;
    }
    return true;
  };
  if (have_pk) {   // 主键等值：走索引点查，不必扫全表
    uint32_t rowid = 0;
    if (!t.store.find(pk_eq, rowid)) return true;
    PayloadView pv;
    if (!t.store.row_views(rowid, pv, err)) return false;
    if (!pass(pv.cols)) return true;
    fn(rowid, pk_eq, pv.cols);
    return true;
  }
  if (!arena_refresh(t, err)) return false;
  // arena 就绪：直接在内存镜像上算距离/谓词，省掉逐查询顺序解析整份行日志
  size_t visited = 0;
  const bool handled = t.store.arena_scan(
      [&](uint32_t rowid, const std::vector<ValueView>& cols) {
        ++visited;
        if (!pass(cols)) return true;
        return fn(rowid, view_int(cols[static_cast<size_t>(schema.pk_col)]), cols);
      });
  if (handled) return true;
  if (visited > 0) {   // 半途解码异常：已部分产出，不能混用流式结果
    err = "storage: vector arena is inconsistent";
    return false;
  }
  return scan_table(t, [&](uint32_t rowid, int64_t pk, const std::vector<ValueView>& cols) {
    if (!pass(cols)) return true;
    return fn(rowid, pk, cols);
  }, err);
}

// ==================================================================== 语句分派
Result Engine::execute(const std::string& sql, const std::string& /*mode*/) {
  Statement st = parse_sql(sql);
  switch (st.kind) {
    case StmtKind::Begin: {
      if (in_txn_) return Result::Fail("nested_transaction", "a transaction is already active");
      undo_.clear();
      created_in_txn_.clear();
      txn_id_ = storage_.begin_txn();
      txn_file_start_ = storage_.file_size();
      txn_dirty_ = false;
      catalog_dirty_ = false;
      in_txn_ = true;
      return Result::Ok();
    }
    case StmtKind::Commit: {
      if (!in_txn_) return Result::Ok();
      std::string err;
      // 事务内建表的 catalog 先落盘，再提交日志：任一失败都保持事务打开，内存态不丢。
      if (catalog_dirty_) {
        if (!persist_catalog(err)) return Result::Fail("storage_error", err);
        catalog_dirty_ = false;
      }
      if (txn_dirty_ && !storage_.commit(txn_id_, err))
        return Result::Fail("storage_error", err);   // 未提交：调用方可重试或回滚
      txn_dirty_ = false;
      undo_.clear();
      created_in_txn_.clear();
      in_txn_ = false;
      return Result::Ok();
    }
    case StmtKind::Rollback: {
      if (!in_txn_) return Result::Ok();
      std::string err;
      if (!rollback_transaction(err)) return Result::Fail("storage_error", err);
      return Result::Ok();
    }
    case StmtKind::CreateTable: return exec_create_table(st);
    case StmtKind::CreateVectorIndex: return exec_create_index(st);
    case StmtKind::Insert: return exec_insert(st);
    case StmtKind::Select: return exec_select(st);
    case StmtKind::Update: return exec_update(st);
    case StmtKind::Delete: return exec_delete(st);
    case StmtKind::Unsupported:
      return Result::Fail(st.error_code.empty() ? "unsupported_statement" : st.error_code, st.error_message);
    case StmtKind::Invalid:
    default:
      return Result::Fail(st.error_code.empty() ? "syntax_error" : st.error_code, st.error_message);
  }
}

void Engine::rollback_on_shutdown() {
  if (!in_txn_) return;
  std::string err;
  rollback_transaction(err);   // 未提交事务从未提交；catalog 若被 DDL 改过一并复原
}

bool Engine::rollback_transaction(std::string& err) {
  err.clear();
  undo_to(0);
  // 事务内新建的表：先撤销行，再删表（ddl 撤销只用于整个事务回滚）
  std::vector<std::string> created;
  created.reserve(created_in_txn_.size());
  for (const std::string* n : created_in_txn_) created.push_back(*n);
  created_in_txn_.clear();
  for (const std::string& n : created) tables_.erase(n);
  for (auto& kv : tables_) kv.second.store.forget_cache();
  undo_.clear();
  in_txn_ = false;
  txn_dirty_ = false;
  const bool ddl = catalog_dirty_;
  catalog_dirty_ = false;
  // 未提交的记录整段丢弃：日志回到事务起点，偏移不会被半途数据占住
  std::string terr;
  const bool ok = storage_.rollback_to(txn_file_start_, terr);
  if (!ok) err = terr;
  if (ddl && !persist_catalog(err)) return false;
  return ok;
}

// ==================================================================== DDL
Result Engine::exec_create_table(const Statement& st) {
  if (tables_.count(st.table)) return Result::Fail("table_exists", "table already exists");
  Schema schema;
  schema.table = st.table;
  schema.cols = st.columns;
  int pk_count = 0;
  int vec_count = 0;
  for (size_t k = 0; k < schema.cols.size(); ++k) {
    if (schema.cols[k].primary) {
      if (schema.cols[k].type != ColType::BigInt) return Result::Fail("invalid_schema", "primary key must be BIGINT");
      if (++pk_count > 1) return Result::Fail("invalid_schema", "at most one primary key");
      schema.pk_col = static_cast<int>(k);
    }
    if (schema.cols[k].type == ColType::Vector) {
      if (++vec_count > 1) return Result::Fail("invalid_schema", "at most one vector column");
      schema.vec_col = static_cast<int>(k);
    }
  }
  if (pk_count != 1) return Result::Fail("invalid_schema", "exactly one primary key is required");
  Table t;
  t.schema = schema;
  t.store.init(&storage_);
  t.store.set_cache_budget(cache_budget_);
  t.store.configure_arena(static_cast<int>(schema.cols.size()), schema.vec_col,
                          schema.vec_col >= 0 ? schema.cols[static_cast<size_t>(schema.vec_col)].dim : 0,
                          vec_arena_budget_);
  auto ins = tables_.emplace(st.table, std::move(t));
  if (in_txn_) {
    catalog_dirty_ = true;                          // COMMIT 时与日志一起落盘，ROLLBACK 时一起撤掉
    created_in_txn_.push_back(&ins.first->first);   // map 键地址稳定
    return Result::Ok();
  }
  std::string err;
  if (!persist_catalog(err)) {
    tables_.erase(ins.first);                       // catalog 写失败：表定义不留在内存里
    return Result::Fail("storage_error", err);
  }
  return Result::Ok();
}

Result Engine::exec_create_index(const Statement& st) {
  Table* t = table(st.table);
  if (!t) return Result::Fail("no_such_table", "unknown table: " + st.table);
  int col = t->schema.find_col(st.index_col);
  if (col < 0) return Result::Fail("no_such_column", "unknown column: " + st.index_col);
  if (t->schema.cols[col].type != ColType::Vector)
    return Result::Fail("invalid_index", "vector index requires a VECTOR column");
  indexed_tables_.insert(st.table);   // exact 语义下索引不改变结果，仅登记
  return Result::Ok();
}

// ==================================================================== INSERT
Result Engine::exec_insert(const Statement& st) {
  Table* t = table(st.table);
  if (!t) return Result::Fail("no_such_table", "unknown table: " + st.table);
  const Schema& schema = t->schema;

  // 整句先校验再落地：任何一行不合法都直接返回，一行都不写内存、也不进日志。
  std::vector<Mutation> muts;
  muts.reserve(st.rows.size());
  std::unordered_set<int64_t> seen;
  for (const auto& raw : st.rows) {
    if (raw.size() != schema.cols.size())
      return Result::Fail("column_count_mismatch", "value count does not match column count");
    Row row;
    row.reserve(raw.size());
    std::string code;
    for (size_t k = 0; k < raw.size(); ++k) {
      Value v;
      if (!coerce(raw[k], schema.cols[k], v, code))
        return Result::Fail(code.empty() ? "type_mismatch" : code, "invalid value for column " + schema.cols[k].name);
      row.push_back(std::move(v));
    }
    const int64_t pk = row[static_cast<size_t>(schema.pk_col)].i;
    uint32_t dummy = 0;
    if (t->store.find(pk, dummy) || seen.count(pk))
      return Result::Fail("duplicate_key", "duplicate primary key");
    seen.insert(pk);
    Mutation m;
    m.op = Mutation::Op::Insert;
    m.table = st.table;
    m.row = std::move(row);
    muts.push_back(std::move(m));
  }
  StmtScope sc = begin_stmt();
  Result r = append_apply(*t, muts, sc);
  if (!r.ok) return abort_stmt(sc, r);
  return end_stmt(sc, static_cast<int64_t>(muts.size()));
}

// ==================================================================== SELECT
Result Engine::exec_select(const Statement& st) {
  Table* t = table(st.table);
  if (!t) return Result::Fail("no_such_table", "unknown table: " + st.table);
  const Schema& schema = t->schema;

  struct OutItem {
    SelectItem::K kind = SelectItem::K::Column;
    int col = -1;
    std::string name;
    std::vector<float> query;
  };
  std::vector<OutItem> items;
  for (const auto& it : st.items) {
    if (it.k == SelectItem::K::Star) {
      for (const auto& c : schema.cols)
        items.push_back({SelectItem::K::Column, schema.find_col(c.name), c.name, {}});
      continue;
    }
    int col = schema.find_col(it.col);
    if (col < 0) return Result::Fail("no_such_column", "unknown column: " + it.col);
    OutItem oi;
    oi.kind = it.k;
    oi.col = col;
    if (it.k == SelectItem::K::Distance) {
      if (schema.cols[col].type != ColType::Vector)
        return Result::Fail("invalid_distance", "L2_DISTANCE requires a VECTOR column");
      if (static_cast<int32_t>(it.query.size()) != schema.cols[col].dim)
        return Result::Fail("dimension_mismatch", "query vector dimension mismatch");
      oi.query = it.query;
      oi.name = it.has_alias ? it.alias : "L2_DISTANCE(" + it.col + ")";
    } else {
      oi.name = it.has_alias ? it.alias : it.col;
    }
    items.push_back(std::move(oi));
  }

  std::vector<Predicate> preds = st.where;
  std::vector<std::pair<int, Value>> bound;
  bound.reserve(preds.size());
  for (const auto& p : preds) {
    int col = schema.find_col(p.col);
    if (col < 0) return Result::Fail("no_such_column", "unknown column: " + p.col);
    if (schema.cols[col].type == ColType::Vector)
      return Result::Fail("unsupported_comparison", "cannot compare vector column");
    Value rhs;
    std::string code;
    if (!coerce(p.value, schema.cols[col], rhs, code))
      return Result::Fail(code.empty() ? "type_mismatch" : code, "invalid comparison value");
    bound.emplace_back(col, std::move(rhs));
  }

  std::vector<int> sort_cols;   // >=0：输出项下标；<0：表列 -1-col
  for (const auto& key : st.order) {
    int idx = -1;
    for (size_t k = 0; k < items.size(); ++k)
      if (items[k].name == key.name) { idx = static_cast<int>(k); break; }
    if (idx >= 0) { sort_cols.push_back(idx); continue; }
    int col = schema.find_col(key.name);
    if (col < 0) return Result::Fail("no_such_column", "unknown order column: " + key.name);
    sort_cols.push_back(-1 - col);
  }

  // 距离按需计算：每行每项最多算一次，算完直接把结果放进排序键/输出
  struct RowEval {
    const std::vector<OutItem>* items = nullptr;
    std::vector<double> dist;
    std::vector<char> have;
    std::vector<float> scratch;

    void init(size_t n) {
      dist.assign(n, 0.0);
      have.assign(n, 0);
    }
    void reset() { std::fill(have.begin(), have.end(), static_cast<char>(0)); }

    double dist_of(size_t idx, const std::vector<ValueView>& cols) {
      if (!have[idx]) {
        const OutItem& it = (*items)[idx];
        const ValueView& v = cols[static_cast<size_t>(it.col)];
        const float* p = reinterpret_cast<const float*>(v.data);
        if ((reinterpret_cast<uintptr_t>(v.data) & (sizeof(float) - 1)) != 0) {
          scratch.resize(v.n);   // 负载里的 float 可能未按 4 字节对齐：复制到对齐缓冲
          if (v.n) std::memcpy(scratch.data(), v.data, static_cast<size_t>(v.n) * sizeof(float));
          p = scratch.data();
        }
        dist[idx] = vec::l2_distance(p, it.query.data(), it.query.size());
        have[idx] = 1;
      }
      return dist[idx];
    }

    void project(const std::vector<ValueView>& cols, Row& out) {
      out.clear();
      out.reserve(items->size());
      for (size_t k = 0; k < items->size(); ++k) {
        const OutItem& it = (*items)[k];
        if (it.kind == SelectItem::K::Distance) out.push_back(Value::Float(dist_of(k, cols)));
        else out.push_back(value_of_view(cols[static_cast<size_t>(it.col)]));
      }
    }
  };
  RowEval ev;
  ev.items = &items;
  ev.init(items.size());

  std::vector<size_t> dist_items;
  for (size_t k = 0; k < items.size(); ++k)
    if (items[k].kind == SelectItem::K::Distance) dist_items.push_back(k);
  auto vec_ok = [&](const std::vector<ValueView>& cols) {
    for (size_t idx : dist_items) {
      const ValueView& v = cols[static_cast<size_t>(items[idx].col)];
      if (v.tag != 4 || v.n != items[idx].query.size()) return false;
    }
    return true;
  };

  Result res = Result::Query();
  for (const auto& it : items) res.columns.push_back(it.name);

  const int64_t lim = st.limit;
  const size_t cap = lim >= 0 ? static_cast<size_t>(lim) : SIZE_MAX;
  const bool bounded = cap != SIZE_MAX;

  // TopK 计划：唯一排序键就是某个 L2_DISTANCE 输出项且升序 → vec::topk_select 有界剪枝
  int topk_item = -1;
  if (st.order.size() == 1 && sort_cols[0] >= 0 && st.order[0].asc &&
      items[static_cast<size_t>(sort_cols[0])].kind == SelectItem::K::Distance)
    topk_item = sort_cols[0];

  struct Cand {
    uint32_t rowid = 0;
    int64_t pk = 0;
    std::vector<Value> keys;
    Row out;
    bool has_out = false;
  };
  std::vector<Cand> cands;
  std::vector<vec::Hit> hits;
  const size_t prune_at = bounded ? std::max<size_t>(cap * 4, 1024) : SIZE_MAX;

  auto row_cmp = [&](const Cand& a, const Cand& b) {
    for (size_t k = 0; k < st.order.size(); ++k) {
      const int c = compare_scalar(a.keys[k], b.keys[k]);
      if (c != 0) return st.order[k].asc ? c < 0 : c > 0;
    }
    return a.pk < b.pk;
  };
  // 有界收集：超过 4k 就按当前排序键裁到 k —— 不是全排序，也不是全量保留
  auto prune_cands = [&]() {
    if (sort_cols.empty())
      std::stable_sort(cands.begin(), cands.end(),
                       [](const Cand& a, const Cand& b) { return a.rowid < b.rowid; });
    else
      std::stable_sort(cands.begin(), cands.end(), row_cmp);
    if (cands.size() > cap) cands.resize(cap);
  };

  std::string err;
  const bool ok = for_each_hit(*t, bound, preds,
                               [&](uint32_t rowid, int64_t pk,
                                   const std::vector<ValueView>& cols) -> bool {
    if (!vec_ok(cols)) return true;
    ev.reset();
    if (topk_item >= 0) {
      vec::Hit h;
      h.dist = ev.dist_of(static_cast<size_t>(topk_item), cols);
      h.pk = pk;
      h.row = rowid;
      hits.push_back(h);
      if (bounded && hits.size() > prune_at) vec::topk_select(hits, cap);
      return true;
    }
    Cand c;
    c.rowid = rowid;
    c.pk = pk;
    c.keys.reserve(sort_cols.size());
    for (int sc : sort_cols) {
      if (sc >= 0) {
        const size_t idx = static_cast<size_t>(sc);
        c.keys.push_back(items[idx].kind == SelectItem::K::Distance
                             ? Value::Float(ev.dist_of(idx, cols))
                             : value_of_view(cols[static_cast<size_t>(items[idx].col)]));
      } else {
        c.keys.push_back(value_of_view(cols[static_cast<size_t>(-1 - sc)]));
      }
    }
    if (!bounded) {              // 无界：边扫边物化，避免二次随机读
      ev.project(cols, c.out);
      c.has_out = true;
    }
    cands.push_back(std::move(c));
    if (bounded && cands.size() > prune_at) prune_cands();
    return true;
  }, err);
  if (!ok) return Result::Fail("storage_error", err);

  if (topk_item >= 0) {
    vec::topk_select(hits, bounded ? cap : hits.size());
    res.rows.reserve(hits.size());
    for (const vec::Hit& h : hits) {
      PayloadView pv;
      if (!t->store.row_views(static_cast<uint32_t>(h.row), pv, err))
        return Result::Fail("storage_error", err);
      ev.reset();
      Row out;
      ev.project(pv.cols, out);
      res.rows.push_back(std::move(out));
    }
    return res;
  }

  if (sort_cols.empty())
    std::stable_sort(cands.begin(), cands.end(),
                     [](const Cand& a, const Cand& b) { return a.rowid < b.rowid; });
  else
    std::stable_sort(cands.begin(), cands.end(), row_cmp);

  size_t n = cands.size();
  if (bounded) n = std::min(n, cap);
  res.rows.reserve(n);
  for (size_t k = 0; k < n; ++k) {
    if (cands[k].has_out) {
      res.rows.push_back(std::move(cands[k].out));
      continue;
    }
    PayloadView pv;
    if (!t->store.row_views(cands[k].rowid, pv, err)) return Result::Fail("storage_error", err);
    ev.reset();
    Row out;
    ev.project(pv.cols, out);
    res.rows.push_back(std::move(out));
  }
  return res;
}

// ==================================================================== UPDATE
Result Engine::exec_update(const Statement& st) {
  Table* t = table(st.table);
  if (!t) return Result::Fail("no_such_table", "unknown table: " + st.table);
  const Schema& schema = t->schema;

  std::vector<std::pair<int, Value>> sets;
  for (const auto& sp : st.set_pairs) {
    int col = schema.find_col(sp.first);
    if (col < 0) return Result::Fail("no_such_column", "unknown column: " + sp.first);
    Value v;
    std::string code;
    if (!coerce(sp.second, schema.cols[col], v, code))
      return Result::Fail(code.empty() ? "type_mismatch" : code, "invalid value for column " + sp.first);
    sets.emplace_back(col, std::move(v));
  }
  // 主键列不允许被 SET 修改：这是语句本身的限制，与命中多少行无关（命中 0 行也必须失败）。
  for (const auto& s : sets)
    if (s.first == schema.pk_col) return Result::Fail("unsupported_update", "primary key update is not supported");

  std::vector<Predicate> preds = st.where;
  std::vector<std::pair<int, Value>> bound;
  bound.reserve(preds.size());
  for (const auto& p : preds) {
    int col = schema.find_col(p.col);
    if (col < 0) return Result::Fail("no_such_column", "unknown column: " + p.col);
    Value rhs;
    std::string code;
    if (!coerce(p.value, schema.cols[col], rhs, code))
      return Result::Fail(code.empty() ? "type_mismatch" : code, "invalid comparison value");
    bound.emplace_back(col, std::move(rhs));
  }

  StmtScope sc = begin_stmt();
  std::vector<Mutation> muts;
  muts.reserve(kChunkMutations);
  int64_t affected = 0;
  Result fail;
  std::string err;
  const bool ok = for_each_hit(*t, bound, preds,
                               [&](uint32_t, int64_t, const std::vector<ValueView>& cols) -> bool {
    Row row;
    if (!decode_cols(cols, row)) {
      fail = Result::Fail("storage_error", "row payload malformed");
      return false;
    }
    for (const auto& s : sets) row[static_cast<size_t>(s.first)] = s.second;
    Mutation m;
    m.op = Mutation::Op::Update;
    m.table = st.table;
    m.row = std::move(row);
    muts.push_back(std::move(m));
    if (muts.size() >= kChunkMutations) {   // 分块落地：内存只与改动量成正比
      Result r = append_apply(*t, muts, sc);
      if (!r.ok) {
        fail = r;
        return false;
      }
      affected += static_cast<int64_t>(muts.size());
      muts.clear();
    }
    return true;
  }, err);

  if (!fail.ok) return abort_stmt(sc, fail);
  if (!ok) return abort_stmt(sc, Result::Fail("storage_error", err));
  if (!muts.empty()) {
    Result r = append_apply(*t, muts, sc);
    if (!r.ok) return abort_stmt(sc, r);
    affected += static_cast<int64_t>(muts.size());
  }
  return end_stmt(sc, affected);
}

// ==================================================================== DELETE
Result Engine::exec_delete(const Statement& st) {
  Table* t = table(st.table);
  if (!t) return Result::Fail("no_such_table", "unknown table: " + st.table);
  const Schema& schema = t->schema;

  std::vector<Predicate> preds = st.where;
  std::vector<std::pair<int, Value>> bound;
  bound.reserve(preds.size());
  for (const auto& p : preds) {
    int col = schema.find_col(p.col);
    if (col < 0) return Result::Fail("no_such_column", "unknown column: " + p.col);
    Value rhs;
    std::string code;
    if (!coerce(p.value, schema.cols[col], rhs, code))
      return Result::Fail(code.empty() ? "type_mismatch" : code, "invalid comparison value");
    bound.emplace_back(col, std::move(rhs));
  }

  StmtScope sc = begin_stmt();
  std::vector<Mutation> muts;
  muts.reserve(kChunkMutations);
  int64_t affected = 0;
  Result fail;
  std::string err;
  const bool ok = for_each_hit(*t, bound, preds,
                               [&](uint32_t, int64_t, const std::vector<ValueView>& cols) -> bool {
    Row row;
    if (!decode_cols(cols, row)) {
      fail = Result::Fail("storage_error", "row payload malformed");
      return false;
    }
    Mutation m;
    m.op = Mutation::Op::Delete;
    m.table = st.table;
    m.row = std::move(row);
    muts.push_back(std::move(m));
    if (muts.size() >= kChunkMutations) {
      Result r = append_apply(*t, muts, sc);
      if (!r.ok) {
        fail = r;
        return false;
      }
      affected += static_cast<int64_t>(muts.size());
      muts.clear();
    }
    return true;
  }, err);

  if (!fail.ok) return abort_stmt(sc, fail);
  if (!ok) return abort_stmt(sc, Result::Fail("storage_error", err));
  if (!muts.empty()) {
    Result r = append_apply(*t, muts, sc);
    if (!r.ok) return abort_stmt(sc, r);
    affected += static_cast<int64_t>(muts.size());
  }
  return end_stmt(sc, affected);
}
