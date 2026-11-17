// storage 模块单测：main() + assert，不引 gtest。
// 每次运行在临时目录（POSIX 下即 /tmp）下用独立子目录，跑完清理。
#ifdef NDEBUG
#undef NDEBUG  // Release 构建里 assert 会被编掉，这里强制打开
#endif

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "storage.h"

namespace fs = std::filesystem;

static int g_checks = 0;
#define CHECK(cond)          \
  do {                       \
    ++g_checks;              \
    assert(cond);            \
  } while (0)

// ---------------------------------------------------------------- 构造工具
static Mutation mk(const std::string& table, Mutation::Op op, std::vector<Value> row) {
  Mutation m;
  m.op = op;
  m.table = table;
  m.row = std::move(row);
  return m;
}

static bool same_value(const Value& a, const Value& b) {
  if (a.k != b.k) return false;
  switch (a.k) {
    case Value::K::Null:
      return true;
    case Value::K::Int:
      return a.i == b.i;
    case Value::K::Float:
      return std::memcmp(&a.d, &b.d, sizeof(double)) == 0;
    case Value::K::Str:
      return a.s == b.s;
    case Value::K::Vec:
      return a.v.size() == b.v.size() &&
             std::memcmp(a.v.data(), b.v.data(), a.v.size() * sizeof(float)) == 0;
  }
  return false;
}

static bool same_mut(const Mutation& a, const Mutation& b) {
  if (a.op != b.op) return false;
  if (a.table != b.table) return false;
  if (a.row.size() != b.row.size()) return false;
  for (size_t i = 0; i < a.row.size(); ++i)
    if (!same_value(a.row[i], b.row[i])) return false;
  return true;
}

static void check_seq(const std::vector<Mutation>& got, const std::vector<Mutation>& want) {
  CHECK(got.size() == want.size());
  for (size_t i = 0; i < want.size() && i < got.size(); ++i) {
    if (!same_mut(got[i], want[i])) {
      std::fprintf(stderr, "mismatch at mutation %zu\n", i);
      CHECK(false);
    }
  }
}

static bool commit(Storage& s, const std::vector<Mutation>& muts) {
  std::string err;
  bool ok = s.commit_batch(muts, err);
  if (!ok) std::fprintf(stderr, "commit_batch failed: %s\n", err.c_str());
  CHECK(ok);
  return ok;
}

static fs::path temp_root() {
  std::error_code ec;
  fs::path p = fs::temp_directory_path(ec);
  if (ec || p.empty() || !fs::is_directory(p, ec)) p = fs::current_path(ec);
  return p;
}

// ---------------------------------------------------------------- 用例
// 1) 两批提交 → 重新 open + replay 得到相同记录序列
static void test_two_batches(const fs::path& dir) {
  std::vector<Mutation> batch_a{
      mk("users", Mutation::Op::Insert, {Value::Int(1), Value::Str("amy")}),
      mk("users", Mutation::Op::Insert, {Value::Int(2), Value::Str("bob")})};
  std::vector<Mutation> batch_b{
      mk("users", Mutation::Op::Update, {Value::Int(1), Value::Str("amy2")}),
      mk("items", Mutation::Op::Delete, {Value::Int(9)})};

  {
    Storage s;
    std::string err;
    CHECK(s.open(dir.string(), err));
    commit(s, batch_a);
    commit(s, batch_b);
    CHECK(s.bytes_written() > 0);
    s.close();
  }

  std::vector<Mutation> got;
  {
    Storage s;
    std::string err;
    bool ok = s.open(dir.string(), err);
    if (!ok) std::fprintf(stderr, "open failed: %s\n", err.c_str());
    CHECK(ok);
    ok = s.replay(got, err);
    if (!ok) std::fprintf(stderr, "replay failed: %s\n", err.c_str());
    CHECK(ok);
    s.close();
  }

  std::vector<Mutation> want = batch_a;
  want.insert(want.end(), batch_b.begin(), batch_b.end());
  check_seq(got, want);
  std::printf("[ok] two batches -> reopen+replay identical (%zu mutations)\n", got.size());
}

// 2) 尾部截断：丢 COMMIT、丢大半条记录，都不能崩，未提交事务整段丢弃
static void test_tail_truncation(const fs::path& base) {
  // 1 列 BIGINT、表名 "t" 的单条记录：
  //   payload = u32(4) + "t"(1) + u32(4) + tag(1) + i64(8) = 18
  //   record  = 17 + 18 = 35 ; COMMIT = 17 ; 每事务 52 ; 文件头 16
  const size_t kRec = 35, kCommit = 17, kHdr = 16;
  const size_t kBatch = kRec + kCommit;

  for (int variant = 0; variant < 2; ++variant) {
    fs::path dir = base / (variant == 0 ? "t2a" : "t2b");
    {
      Storage s;
      std::string err;
      CHECK(s.open(dir.string(), err));
      commit(s, {mk("t", Mutation::Op::Insert, {Value::Int(7)})});
      commit(s, {mk("t", Mutation::Op::Insert, {Value::Int(8)})});
      s.close();
    }
    fs::path wal = dir / "wal.log";
    CHECK(fs::file_size(wal) == kHdr + 2 * kBatch);

    // variant 0：砍掉 COMMIT 记录的最后 7 字节
    // variant 1：砍进第二批的第一条记录（只剩 5 字节尾巴）
    size_t keep = variant == 0 ? (kHdr + 2 * kBatch - 7) : (kHdr + kBatch + 5);
    fs::resize_file(wal, keep);
    CHECK(fs::file_size(wal) == keep);

    std::vector<Mutation> got;
    Storage s;
    std::string err;
    bool ok = s.open(dir.string(), err);
    if (!ok) std::fprintf(stderr, "open failed: %s\n", err.c_str());
    CHECK(ok);
    ok = s.replay(got, err);
    if (!ok) std::fprintf(stderr, "replay failed: %s\n", err.c_str());
    CHECK(ok);
    s.close();

    // 第二批没有 COMMIT → 整段丢弃，只剩第一批
    CHECK(got.size() == 1);
    CHECK(same_mut(got[0], mk("t", Mutation::Op::Insert, {Value::Int(7)})));
    std::printf("[ok] truncated tail (variant %d, keep=%zu) -> %zu committed mutation(s), no crash\n",
                variant, keep, got.size());
  }
}

// 3) 破坏某条记录的 CRC → replay 在该处停止（之前已提交的保留）
static void test_crc_corruption(const fs::path& dir) {
  const size_t kRec = 35, kCommit = 17, kHdr = 16, kBatch = kRec + kCommit;
  {
    Storage s;
    std::string err;
    CHECK(s.open(dir.string(), err));
    commit(s, {mk("t", Mutation::Op::Insert, {Value::Int(11)})});
    commit(s, {mk("t", Mutation::Op::Insert, {Value::Int(22)})});
    s.close();
  }
  fs::path wal = dir / "wal.log";
  CHECK(fs::file_size(wal) == kHdr + 2 * kBatch);

  // 第二批第一条记录的 payload 起始 = kHdr + kBatch + 17；往里翻一个字节
  size_t victim = kHdr + kBatch + 17 + 2;
  CHECK(victim > kHdr + kBatch + 17);
  CHECK(victim < kHdr + kBatch + kRec);
  {
    std::FILE* f = std::fopen(wal.string().c_str(), "r+b");
    CHECK(f != nullptr);
    CHECK(std::fseek(f, static_cast<long>(victim), SEEK_SET) == 0);
    int c = std::fgetc(f);
    CHECK(c != EOF);
    CHECK(std::fseek(f, static_cast<long>(victim), SEEK_SET) == 0);
    CHECK(std::fputc(c ^ 0xFF, f) == (c ^ 0xFF));
    std::fflush(f);
    std::fclose(f);
  }

  std::vector<Mutation> got;
  Storage s;
  std::string err;
  bool ok = s.open(dir.string(), err);
  if (!ok) std::fprintf(stderr, "open failed: %s\n", err.c_str());
  CHECK(ok);
  ok = s.replay(got, err);
  if (!ok) std::fprintf(stderr, "replay failed: %s\n", err.c_str());
  CHECK(ok);
  s.close();

  CHECK(got.size() == 1);  // 停在坏记录处，第二批一条都没进来
  CHECK(same_mut(got[0], mk("t", Mutation::Op::Insert, {Value::Int(11)})));
  std::printf("[ok] corrupted crc at byte %zu -> replay stopped, %zu mutation(s) kept\n", victim,
              got.size());
}

// 4) 值保真：VARCHAR 中文、BIGINT 超 2^53、VECTOR float32 原样
static void test_value_fidelity(const fs::path& dir) {
  const int64_t kBig = 9007199254740993LL;  // 2^53 + 1，double 存不下，必须走 int64
  CHECK(static_cast<int64_t>(static_cast<double>(kBig)) != kBig);
  const std::vector<float> vec{1.5f, -2.25f, 0.142857142857f, 0.0f, -0.0f, 3.4028234663852886e38f};
  std::vector<Value> row{Value::Int(kBig), Value::Str("向量数据库·中文测试"),
                         Value::Vec(vec),     Value::Float(3.141592653589793),
                         Value{},             Value::Str("")};

  {
    Storage s;
    std::string err;
    CHECK(s.open(dir.string(), err));
    commit(s, {mk("向量表", Mutation::Op::Insert, row)});
    s.close();
  }

  std::vector<Mutation> got;
  {
    Storage s;
    std::string err;
    CHECK(s.open(dir.string(), err));
    bool ok = s.replay(got, err);
    if (!ok) std::fprintf(stderr, "replay failed: %s\n", err.c_str());
    CHECK(ok);
    s.close();
  }
  CHECK(got.size() == 1);
  CHECK(got[0].table == "向量表");
  CHECK(got[0].row.size() == row.size());
  for (size_t i = 0; i < row.size(); ++i) {
    if (!same_value(got[0].row[i], row[i])) {
      std::fprintf(stderr, "value %zu lost fidelity\n", i);
      CHECK(false);
    }
  }
  CHECK(got[0].row[0].i == kBig);
  CHECK(got[0].row[1].s == "向量数据库·中文测试");
  CHECK(got[0].row[2].v.size() == vec.size());
  CHECK(std::memcmp(got[0].row[2].v.data(), vec.data(), vec.size() * sizeof(float)) == 0);
  std::printf("[ok] value fidelity: bigint=%lld varchar=%zu bytes vector=%zu dims\n",
              static_cast<long long>(got[0].row[0].i), got[0].row[1].s.size(),
              got[0].row[2].v.size());
}

// 5) 连续 100 批提交后 replay 顺序正确
static void test_hundred_batches(const fs::path& dir) {
  const int kN = 100;
  {
    Storage s;
    std::string err;
    CHECK(s.open(dir.string(), err));
    for (int i = 0; i < kN; ++i)
      commit(s, {mk("seq", Mutation::Op::Insert, {Value::Int(i), Value::Str("t" + std::to_string(i))})});
    s.close();
  }
  std::vector<Mutation> got;
  {
    Storage s;
    std::string err;
    CHECK(s.open(dir.string(), err));
    bool ok = s.replay(got, err);
    if (!ok) std::fprintf(stderr, "replay failed: %s\n", err.c_str());
    CHECK(ok);
    s.close();
  }
  CHECK(got.size() == static_cast<size_t>(kN));
  for (int i = 0; i < kN && i < static_cast<int>(got.size()); ++i) {
    CHECK(got[static_cast<size_t>(i)].row[0].i == i);
    CHECK(got[static_cast<size_t>(i)].row[1].s == "t" + std::to_string(i));
  }
  std::printf("[ok] 100 batches -> replay order 0..%d intact\n", static_cast<int>(got.size()) - 1);
}

// 6) 空 WAL：replay 空集、bytes_written 为 0；重放后继续提交不会与旧事务撞号
static void test_empty_wal_and_resume(const fs::path& dir) {
  std::string err;
  {
    Storage s;
    CHECK(s.open(dir.string(), err));
    std::vector<Mutation> got;
    CHECK(s.replay(got, err));
    CHECK(got.empty());
    CHECK(s.bytes_written() == 0);
    CHECK(fs::file_size(dir / "wal.log") == 16);  // 只有文件头
    s.close();
  }
  {
    Storage s;
    CHECK(s.open(dir.string(), err));
    commit(s, {mk("t", Mutation::Op::Insert, {Value::Int(1)})});
    s.close();
  }
  {
    Storage s;
    CHECK(s.open(dir.string(), err));
    std::vector<Mutation> got;
    CHECK(s.replay(got, err));
    CHECK(got.size() == 1);
    // 重放过旧事务后继续写入：新事务必须排在后面，且整段可再次重放
    commit(s, {mk("t", Mutation::Op::Insert, {Value::Int(2)})});
    s.close();
  }
  {
    Storage s;
    CHECK(s.open(dir.string(), err));
    std::vector<Mutation> got;
    CHECK(s.replay(got, err));
    CHECK(got.size() == 2);
    CHECK(got[0].row[0].i == 1);
    CHECK(got[1].row[0].i == 2);
    s.close();
  }
  std::printf("[ok] empty wal replay + append after replay keeps order (hdr=%zu bytes)\n",
              static_cast<size_t>(16));
}

// 6) catalog 往返（表定义持久化，含 magic/version/crc）
static void test_catalog_roundtrip(const fs::path& dir) {
  Schema a;
  a.table = "users";
  a.cols = {Column{"id", ColType::BigInt, 0, 0, true},
            Column{"name", ColType::VarChar, 64, 0, false},
            Column{"emb", ColType::Vector, 0, 4, false}};
  a.pk_col = 0;
  a.vec_col = 2;
  Schema b;
  b.table = "中文表";
  b.cols = {Column{"k", ColType::BigInt, 0, 0, true}};
  b.pk_col = 0;
  b.vec_col = -1;

  {
    Storage s;
    std::string err;
    CHECK(s.open(dir.string(), err));
    std::vector<Schema> none;
    bool ok = s.load_catalog(none, err);  // 还没有 catalog：不报错，返回空
    CHECK(ok);
    CHECK(none.empty());
    ok = s.save_catalog({a, b}, err);
    if (!ok) std::fprintf(stderr, "save_catalog failed: %s\n", err.c_str());
    CHECK(ok);
    s.close();
  }
  {
    Storage s;
    std::string err;
    CHECK(s.open(dir.string(), err));
    std::vector<Schema> got;
    bool ok = s.load_catalog(got, err);
    if (!ok) std::fprintf(stderr, "load_catalog failed: %s\n", err.c_str());
    CHECK(ok);
    s.close();
    CHECK(got.size() == 2);
    if (got.size() == 2) {
      CHECK(got[0].table == a.table);
      CHECK(got[0].cols.size() == 3);
      CHECK(got[0].cols[1].type == ColType::VarChar && got[0].cols[1].varchar_len == 64);
      CHECK(got[0].cols[2].type == ColType::Vector && got[0].cols[2].dim == 4);
      CHECK(got[0].pk_col == 0 && got[0].vec_col == 2);
      CHECK(got[1].table == "中文表");
      CHECK(got[1].vec_col == -1);
    }
  }
  // 故意破坏 catalog 的 CRC
  fs::path cat = dir / "catalog.dat";
  {
    std::FILE* f = std::fopen(cat.string().c_str(), "r+b");
    CHECK(f != nullptr);
    CHECK(std::fseek(f, 24 + 3, SEEK_SET) == 0);
    int c = std::fgetc(f);
    CHECK(c != EOF);
    CHECK(std::fseek(f, 24 + 3, SEEK_SET) == 0);
    CHECK(std::fputc(c ^ 0xFF, f) == (c ^ 0xFF));
    std::fflush(f);
    std::fclose(f);
  }
  {
    Storage s;
    std::string err;
    CHECK(s.open(dir.string(), err));
    std::vector<Schema> got;
    bool ok = s.load_catalog(got, err);
    CHECK(!ok);            // crc 不匹配必须被拒绝
    CHECK(!err.empty());
    s.close();
  }
  std::printf("[ok] catalog roundtrip (magic+version+crc), corrupted catalog rejected\n");
}

int main() {
  fs::path root = temp_root() / "mvdb_storage_test";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root, ec);
  CHECK(fs::is_directory(root));

  std::printf("storage_test root: %s\n", root.string().c_str());

  test_two_batches(root / "t1");
  test_tail_truncation(root);
  test_crc_corruption(root / "t3");
  test_value_fidelity(root / "t4");
  test_hundred_batches(root / "t5");
  test_empty_wal_and_resume(root / "t6");
  test_catalog_roundtrip(root / "t7");

  std::printf("\nSTORAGE TEST PASSED (%d assertions)\n", g_checks);
  fs::remove_all(root, ec);
  return 0;
}
