#include "db.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cfloat>
#include <charconv>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <queue>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <sched.h>
#endif

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

#include "json.h"
#include "parser.h"
#include "serialize.h"

namespace mvd {

namespace {

constexpr size_t kMaxColumns = 64;
constexpr int64_t kMaxVarcharLen = 1 << 24;
constexpr int64_t kMaxVectorDim = 65536;
constexpr int64_t kCheckpointWalBytes = 64LL << 20;
constexpr uint32_t kSnapshotMagic = 0x5053564D;  // "MVSP"
constexpr uint32_t kSnapshotVersion = 2;

struct RowRef {
  int64_t pk = 0;
  int64_t off = 0;
  uint32_t len = 0;
  uint32_t vec_off = 0;
  bool alive = false;
};

struct Table {
  std::string name;
  std::vector<Column> cols;
  std::vector<RowRef> slots;
  std::unordered_map<int64_t, uint32_t> pk2slot;
  std::vector<std::string> indexes;
  // Bucket entry remembers the row offset it was indexed from: an unchanged
  // offset means the row bytes are untouched, so the predicate need not be
  // re-checked. Rows rewritten by update get a fresh entry; stale ones fail
  // the offset test and fall back to re-verification.
  struct IndexEntry {
    uint32_t slot = 0;
    int64_t off = 0;
  };
  struct EqIndex {
    std::unordered_map<std::string, std::vector<IndexEntry>> by_str;
    std::unordered_map<int64_t, std::vector<IndexEntry>> by_int;
  };
  // Add-only equality buckets, built on first use. Entries whose row was
  // rewritten since indexing are re-checked against the current row bytes.
  std::unordered_map<int, EqIndex> eq_indexes;
  int pk_idx = -1;
  int vec_idx = -1;
};

class Heap {
 public:
  ~Heap() { close(); }

  void open(const std::string &path) {
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd_ < 0) {
      raise("storage_error", "cannot open heap file");
    }
    off_t end = lseek(fd_, 0, SEEK_END);
    if (end < 0) {
      raise("storage_error", "cannot size heap file");
    }
    size_ = static_cast<int64_t>(end);
    if (size_ > 0) {
      map_to(static_cast<size_t>(size_));
    }
  }

  int64_t size() const { return size_; }

  bool valid(int64_t off, uint32_t len) const {
    return off >= 0 && off <= size_ && static_cast<int64_t>(len) <= size_ - off;
  }

  int64_t append(const char *data, uint32_t n) {
    size_t need = static_cast<size_t>(size_) + 4 + n;
    if (need > map_len_) {
      size_t target = map_len_ ? map_len_ * 2 : (1u << 20);
      while (target < need) {
        target *= 2;
      }
      map_to(target);
    }
    char *dst = map_ + size_;
    uint32_t nlen = n;
    memcpy(dst, &nlen, 4);
    if (n > 0) {
      memcpy(dst + 4, data, n);
    }
    int64_t off = size_ + 4;
    size_ += static_cast<int64_t>(4 + n);
    return off;
  }

  const char *ptr(int64_t off) const { return map_ + off; }

  void sync() {
    if (fd_ >= 0) {
      fsync(fd_);
    }
  }

  void close() {
    if (map_ != nullptr) {
      msync(map_, map_len_, MS_ASYNC);
      munmap(map_, map_len_);
      map_ = nullptr;
      map_len_ = 0;
    }
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

 private:
  void map_to(size_t target) {
    if (target < static_cast<size_t>(size_)) {
      target = static_cast<size_t>(size_);
    }
    if (ftruncate(fd_, static_cast<off_t>(target)) != 0) {
      raise("storage_error", "cannot grow heap file");
    }
    if (map_ != nullptr) {
      munmap(map_, map_len_);
      map_ = nullptr;
    }
    void *m = mmap(nullptr, target, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (m == MAP_FAILED) {
      raise("storage_error", "cannot map heap file");
    }
    map_ = static_cast<char *>(m);
    map_len_ = target;
  }

  int fd_ = -1;
  int64_t size_ = 0;
  char *map_ = nullptr;
  size_t map_len_ = 0;
};

class Wal {
 public:
  ~Wal() { close(); }

  void open(const std::string &path) {
    path_ = path;
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_APPEND, 0644);
    if (fd_ < 0) {
      raise("storage_error", "cannot open wal file");
    }
  }

  int64_t appended() const { return appended_; }

  void append(const std::string &payload) {
    Buf frame;
    frame.u32(static_cast<uint32_t>(payload.size()));
    frame.u32(crc32(payload.data(), payload.size()));
    frame.bytes(payload.data(), payload.size());
    size_t done = 0;
    while (done < frame.data.size()) {
      ssize_t n = ::write(fd_, frame.data.data() + done, frame.data.size() - done);
      if (n <= 0) {
        if (errno == EINTR) {
          continue;
        }
        raise("storage_error", "wal write failed");
      }
      done += static_cast<size_t>(n);
    }
    // The declared crash model is process termination, where the page cache
    // survives; the log is synced at shutdown and before WAL truncation.
    appended_ += static_cast<int64_t>(frame.data.size());
  }

  void sync() {
    if (fd_ >= 0 && fsync(fd_) != 0) {
      raise("storage_error", "wal fsync failed");
    }
  }

  template <typename Fn>
  void replay(Fn &&fn) {
    off_t end = lseek(fd_, 0, SEEK_END);
    if (end < 0) {
      raise("storage_error", "cannot size wal file");
    }
    std::string data;
    data.resize(static_cast<size_t>(end));
    size_t got = 0;
    while (got < data.size()) {
      ssize_t n = pread(fd_, data.data() + got, data.size() - got, static_cast<off_t>(got));
      if (n <= 0) {
        if (n < 0 && errno == EINTR) {
          continue;
        }
        break;
      }
      got += static_cast<size_t>(n);
    }
    data.resize(got);
    size_t pos = 0;
    size_t valid = 0;
    while (pos + 8 <= data.size()) {
      uint32_t len = 0;
      uint32_t crc = 0;
      memcpy(&len, data.data() + pos, 4);
      memcpy(&crc, data.data() + pos + 4, 4);
      if (len > data.size() - pos - 8) {
        break;
      }
      const char *payload = data.data() + pos + 8;
      if (crc32(payload, len) != crc) {
        break;
      }
      fn(payload, len);
      pos += static_cast<size_t>(8) + len;
      valid = pos;
    }
    if (valid < data.size()) {
      if (ftruncate(fd_, static_cast<off_t>(valid)) != 0) {
        raise("storage_error", "cannot truncate wal");
      }
      fsync(fd_);
    }
    appended_ = 0;
  }

  void reset() {
    if (ftruncate(fd_, 0) != 0) {
      raise("storage_error", "cannot reset wal");
    }
    fsync(fd_);
    appended_ = 0;
  }

  void close() {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

 private:
  std::string path_;
  int fd_ = -1;
  int64_t appended_ = 0;
};

struct Undo {
  enum Kind { ERASE_TABLE, ERASE_INDEX, ERASE_SLOT, RESTORE_SLOT, UNDELETE_SLOT };
  Kind kind;
  std::string table;
  std::string name;
  uint32_t slot = 0;
  int64_t old_off = 0;
  uint32_t old_len = 0;
};

struct WalOp {
  uint8_t type = 0;  // 1 create table, 2 create index, 3 insert, 4 update, 5 delete
  std::string table;
  std::string name;
  std::string column;
  std::vector<Column> schema;
  int64_t pk = 0;
  int64_t off = 0;
  uint32_t len = 0;
};

struct Savepoint {
  size_t undo = 0;
  size_t pending = 0;
};

struct Cursor {
  const char *p = nullptr;
  uint32_t len = 0;
  uint32_t offs[kMaxColumns];

  bool init(const Table &t) {
    size_t pos = 0;
    size_t ncols = t.cols.size();
    for (size_t ci = 0; ci < ncols; ++ci) {
      const Column &col = t.cols[ci];
      offs[ci] = static_cast<uint32_t>(pos);
      if (col.type == ColType::BIGINT) {
        pos += 8;
      } else if (col.type == ColType::VARCHAR) {
        if (pos + 4 > len) {
          return false;
        }
        uint32_t slen = 0;
        memcpy(&slen, p + pos, 4);
        pos += 4 + static_cast<size_t>(slen);
      } else {
        pos += static_cast<size_t>(col.len) * 4;
      }
    }
    return pos == len;
  }

  int64_t i64(int ci) const {
    int64_t v = 0;
    memcpy(&v, p + offs[ci], 8);
    return v;
  }

  std::string_view str(int ci) const {
    uint32_t slen = 0;
    memcpy(&slen, p + offs[ci], 4);
    return std::string_view(p + offs[ci] + 4, slen);
  }

  int64_t dim(int ci) const {
    uint32_t slen = 0;
    memcpy(&slen, p + offs[ci], 4);
    return slen;
  }

  const char *vec_bytes(int ci) const { return p + offs[ci]; }
};

struct WherePlan {
  int col = -1;
  CmpOp op = CmpOp::EQ;
  int64_t i = 0;
  std::string s;
};

struct DistCand {
  double d = 0;
  int64_t pk = 0;
  uint32_t slot = 0;
};

struct IntCand {
  int64_t k = 0;
  int64_t pk = 0;
  uint32_t slot = 0;
};

struct StrCand {
  std::string k;
  int64_t pk = 0;
  uint32_t slot = 0;
};

void encode_row_into(const Table &t, const std::vector<Value> &vals, std::string &out,
                     uint32_t *vec_off) {
  out.clear();
  for (size_t i = 0; i < t.cols.size(); ++i) {
    if (t.cols[i].type == ColType::VECTOR && vec_off != nullptr) {
      *vec_off = static_cast<uint32_t>(out.size());
    }
    const Value &v = vals[i];
    switch (v.type) {
      case ColType::BIGINT: {
        out.append(reinterpret_cast<const char *>(&v.i), 8);
        break;
      }
      case ColType::VARCHAR: {
        uint32_t n = static_cast<uint32_t>(v.s.size());
        out.append(reinterpret_cast<const char *>(&n), 4);
        out.append(v.s.data(), v.s.size());
        break;
      }
      case ColType::VECTOR: {
        out.append(reinterpret_cast<const char *>(v.v.data()), v.v.size() * 4);
        break;
      }
    }
  }
}

std::string encode_row(const Table &t, const std::vector<Value> &vals, uint32_t *vec_off) {
  std::string out;
  encode_row_into(t, vals, out, vec_off);
  return out;
}

std::vector<Value> decode_row(const Table &t, const char *p, uint32_t len) {
  Cursor cur;
  cur.p = p;
  cur.len = len;
  if (!cur.init(t)) {
    raise("storage_error", "corrupt row");
  }
  std::vector<Value> out(t.cols.size());
  for (size_t ci = 0; ci < t.cols.size(); ++ci) {
    Value &v = out[ci];
    v.type = t.cols[ci].type;
    if (v.type == ColType::BIGINT) {
      v.i = cur.i64(static_cast<int>(ci));
    } else if (v.type == ColType::VARCHAR) {
      v.s.assign(cur.str(static_cast<int>(ci)));
    } else {
      int64_t dim = t.cols[ci].len;
      v.v.resize(static_cast<size_t>(dim));
      memcpy(v.v.data(), cur.vec_bytes(static_cast<int>(ci)), static_cast<size_t>(dim) * 4);
    }
  }
  return out;
}

uint32_t row_vec_off(const Table &t, const char *p, uint32_t len) {
  if (t.vec_idx < 0) {
    return 0;
  }
  Cursor cur;
  cur.p = p;
  cur.len = len;
  if (!cur.init(t)) {
    raise("storage_error", "corrupt row");
  }
  return cur.offs[t.vec_idx];
}

void parse_vector_literal(const std::string &text, int64_t dim, std::vector<float> &out) {
  out.clear();
  if (out.capacity() < static_cast<size_t>(dim)) {
    out.reserve(static_cast<size_t>(dim));
  }
  size_t i = 0;
  size_t n = text.size();
  auto skip = [&]() {
    while (i < n && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r')) {
      ++i;
    }
  };
  skip();
  if (i >= n || text[i] != '[') {
    raise("invalid_vector", "vector literal must start with '['");
  }
  ++i;
  skip();
  if (i < n && text[i] == ']') {
    ++i;
    skip();
    if (i != n) {
      raise("invalid_vector", "trailing characters after vector");
    }
    if (dim != 0) {
      raise("dimension_mismatch", "vector dimension mismatch");
    }
    return;
  }
  while (true) {
    skip();
    if (i >= n) {
      raise("invalid_vector", "unterminated vector literal");
    }
    const char *start = text.c_str() + i;
    const char *limit = text.c_str() + n;
    const char *num = start;
    if (num < limit && *num == '+') {
      ++num;
    }
    float f = 0.0f;
    std::from_chars_result res = std::from_chars(num, limit, f);
    if (res.ec == std::errc::result_out_of_range) {
      // Keep the exact previous diagnostics for extreme magnitudes.
      char *end = nullptr;
      errno = 0;
      double d = strtod(start, &end);
      if (end == start) {
        raise("invalid_vector", "invalid vector component");
      }
      if (!std::isfinite(d)) {
        raise("invalid_vector", "vector component is not finite");
      }
      if (std::fabs(d) > static_cast<double>(FLT_MAX)) {
        raise("value_out_of_range", "vector component overflows float32");
      }
      f = static_cast<float>(d);
      if (!std::isfinite(f)) {
        raise("value_out_of_range", "vector component overflows float32");
      }
      i = static_cast<size_t>(end - text.c_str());
    } else if (res.ec != std::errc() || res.ptr == num) {
      raise("invalid_vector", "invalid vector component");
    } else if (!std::isfinite(f)) {
      raise("invalid_vector", "vector component is not finite");
    } else {
      i = static_cast<size_t>(res.ptr - text.c_str());
    }
    out.push_back(f);
    skip();
    if (i < n && text[i] == ',') {
      ++i;
      continue;
    }
    if (i < n && text[i] == ']') {
      ++i;
      break;
    }
    raise("invalid_vector", "invalid vector separator");
  }
  skip();
  if (i != n) {
    raise("invalid_vector", "trailing characters after vector");
  }
  if (static_cast<int64_t>(out.size()) != dim) {
    raise("dimension_mismatch", "vector dimension mismatch");
  }
}

Value literal_to_value(const Column &col, const Literal &lit) {
  Value v;
  v.type = col.type;
  if (col.type == ColType::BIGINT) {
    if (lit.kind != Literal::INT) {
      raise("type_mismatch", "expected integer value for " + col.name);
    }
    v.i = lit.i;
  } else if (col.type == ColType::VARCHAR) {
    if (lit.kind != Literal::STR) {
      raise("type_mismatch", "expected string value for " + col.name);
    }
    if (utf8_length(lit.s) > col.len) {
      raise("string_too_long", "value exceeds VARCHAR length");
    }
    v.s = lit.s;
  } else {
    if (lit.kind != Literal::STR) {
      raise("type_mismatch", "expected vector literal for " + col.name);
    }
    parse_vector_literal(lit.s, col.len, v.v);
  }
  return v;
}

bool apply_cmp(int cmp, CmpOp op) {
  switch (op) {
    case CmpOp::EQ: return cmp == 0;
    case CmpOp::NE: return cmp != 0;
    case CmpOp::LT: return cmp < 0;
    case CmpOp::LE: return cmp <= 0;
    case CmpOp::GT: return cmp > 0;
    case CmpOp::GE: return cmp >= 0;
  }
  return false;
}

double l2_sq_scalar(const char *a, const char *b, int64_t dim) {
  double sum = 0;
  const float *fa = reinterpret_cast<const float *>(a);
  const float *fb = reinterpret_cast<const float *>(b);
  for (int64_t i = 0; i < dim; ++i) {
    double d = static_cast<double>(fa[i]) - static_cast<double>(fb[i]);
    sum += d * d;
  }
  return sum;
}

#if defined(__x86_64__) || defined(__i386__)
__attribute__((target("avx2,fma"))) double l2_sq_avx2(const char *a, const char *b, int64_t dim) {
  const float *fa = reinterpret_cast<const float *>(a);
  const float *fb = reinterpret_cast<const float *>(b);
  __m256 s0 = _mm256_setzero_ps();
  __m256 s1 = _mm256_setzero_ps();
  __m256 s2 = _mm256_setzero_ps();
  __m256 s3 = _mm256_setzero_ps();
  int64_t i = 0;
  for (; i + 32 <= dim; i += 32) {
    __m256 d0 = _mm256_sub_ps(_mm256_loadu_ps(fa + i), _mm256_loadu_ps(fb + i));
    __m256 d1 = _mm256_sub_ps(_mm256_loadu_ps(fa + i + 8), _mm256_loadu_ps(fb + i + 8));
    __m256 d2 = _mm256_sub_ps(_mm256_loadu_ps(fa + i + 16), _mm256_loadu_ps(fb + i + 16));
    __m256 d3 = _mm256_sub_ps(_mm256_loadu_ps(fa + i + 24), _mm256_loadu_ps(fb + i + 24));
    s0 = _mm256_fmadd_ps(d0, d0, s0);
    s1 = _mm256_fmadd_ps(d1, d1, s1);
    s2 = _mm256_fmadd_ps(d2, d2, s2);
    s3 = _mm256_fmadd_ps(d3, d3, s3);
  }
  for (; i + 8 <= dim; i += 8) {
    __m256 d0 = _mm256_sub_ps(_mm256_loadu_ps(fa + i), _mm256_loadu_ps(fb + i));
    s0 = _mm256_fmadd_ps(d0, d0, s0);
  }
  __m256 s = _mm256_add_ps(_mm256_add_ps(s0, s1), _mm256_add_ps(s2, s3));
  __m128 h = _mm_add_ps(_mm256_castps256_ps128(s), _mm256_extractf128_ps(s, 1));
  h = _mm_add_ps(h, _mm_movehl_ps(h, h));
  h = _mm_add_ss(h, _mm_shuffle_ps(h, h, 0x55));
  double total = static_cast<double>(_mm_cvtss_f32(h));
  for (; i < dim; ++i) {
    double d = static_cast<double>(fa[i]) - static_cast<double>(fb[i]);
    total += d * d;
  }
  return total;
}

bool have_avx2() {
  static bool ok = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
  return ok;
}
#endif

constexpr uint32_t kFastTopK = 64;

struct ScanCand {
  double d = 0;
  int64_t pk = 0;
  uint32_t slot = 0;
};

#if defined(__x86_64__) || defined(__i386__)
// Ascending ORDER BY distance Top-K over a contiguous row scan. Keeps the
// exact same arithmetic as l2_sq_avx2 so returned distances are unchanged.
__attribute__((target("avx2,fma")))
void scan_topk_avx2(const char *map, const RowRef *slots, const uint32_t *idx, uint32_t n,
                    int64_t dim, const float *q, uint32_t k, ScanCand *top, uint32_t *out_n) {
  uint32_t filled = 0;
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t si = idx != nullptr ? idx[i] : i;
    const RowRef &ref = slots[si];
    if (!ref.alive) {
      continue;
    }
    {
      // Prefetch two lines of the row kPfDist ahead; further lines did not pay
      // off in measurements and the vector walk already overlaps loads.
      constexpr uint32_t kPfDist = 8;
      uint32_t pi = i + kPfDist;
      if (pi < n) {
        uint32_t pj = idx != nullptr ? idx[pi] : pi;
        const RowRef &pf = slots[pj];
        const char *pv = map + pf.off + pf.vec_off;
        __builtin_prefetch(pv);
        __builtin_prefetch(pv + 128);
      }
    }
    const float *fa = reinterpret_cast<const float *>(map + ref.off + ref.vec_off);
    __m256 s0 = _mm256_setzero_ps();
    __m256 s1 = _mm256_setzero_ps();
    __m256 s2 = _mm256_setzero_ps();
    __m256 s3 = _mm256_setzero_ps();
    int64_t p = 0;
    for (; p + 32 <= dim; p += 32) {
      __m256 d0 = _mm256_sub_ps(_mm256_loadu_ps(fa + p), _mm256_loadu_ps(q + p));
      __m256 d1 = _mm256_sub_ps(_mm256_loadu_ps(fa + p + 8), _mm256_loadu_ps(q + p + 8));
      __m256 d2 = _mm256_sub_ps(_mm256_loadu_ps(fa + p + 16), _mm256_loadu_ps(q + p + 16));
      __m256 d3 = _mm256_sub_ps(_mm256_loadu_ps(fa + p + 24), _mm256_loadu_ps(q + p + 24));
      s0 = _mm256_fmadd_ps(d0, d0, s0);
      s1 = _mm256_fmadd_ps(d1, d1, s1);
      s2 = _mm256_fmadd_ps(d2, d2, s2);
      s3 = _mm256_fmadd_ps(d3, d3, s3);
    }
    for (; p + 8 <= dim; p += 8) {
      __m256 d0 = _mm256_sub_ps(_mm256_loadu_ps(fa + p), _mm256_loadu_ps(q + p));
      s0 = _mm256_fmadd_ps(d0, d0, s0);
    }
    __m256 s = _mm256_add_ps(_mm256_add_ps(s0, s1), _mm256_add_ps(s2, s3));
    __m128 h = _mm_add_ps(_mm256_castps256_ps128(s), _mm256_extractf128_ps(s, 1));
    h = _mm_add_ps(h, _mm_movehl_ps(h, h));
    h = _mm_add_ss(h, _mm_shuffle_ps(h, h, 0x55));
    double d = static_cast<double>(_mm_cvtss_f32(h));
    for (; p < dim; ++p) {
      double dd = static_cast<double>(fa[p]) - static_cast<double>(q[p]);
      d += dd * dd;
    }
    if (filled == k && (d > top[k - 1].d || (d == top[k - 1].d && ref.pk > top[k - 1].pk))) {
      continue;
    }
    uint32_t pos = filled < k ? filled : k - 1;
    while (pos > 0 && (top[pos - 1].d > d || (top[pos - 1].d == d && top[pos - 1].pk > ref.pk))) {
      top[pos] = top[pos - 1];
      --pos;
    }
    top[pos].d = d;
    top[pos].pk = ref.pk;
    top[pos].slot = si;
    if (filled < k) {
      ++filled;
    }
  }
  *out_n = filled;
}
#endif

// Queries smaller than this stay on the calling thread; below it the wake-up
// handshake costs more than the scan it would parallelize.
constexpr uint32_t kParScanMin = 256;

// How many threads the scan may use. Workers are only started when the scan
// first exceeds kParScanMin, so a single-core or quota-limited run pays
// nothing.
int scan_thread_budget() {
  static const int cached = [] {
    int n = 4;
    if (const char *env = std::getenv("MVD_SCAN_THREADS")) {
      n = std::atoi(env);
    } else {
#if defined(__linux__)
      cpu_set_t set;
      if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        n = std::min(n, CPU_COUNT(&set));
      }
      if (FILE *f = std::fopen("/sys/fs/cgroup/cpu.max", "r")) {
        char quota[32] = {0};
        long period = 0;
        if (std::fscanf(f, "%31s %ld", quota, &period) == 2 && period > 0 &&
            std::strcmp(quota, "max") != 0) {
          long q = std::atol(quota);
          if (q > 0) {
            n = std::min(n, static_cast<int>((q + period - 1) / period));
          }
        }
        std::fclose(f);
      } else if (FILE *fq = std::fopen("/sys/fs/cgroup/cpu/cpu.cfs_quota_us", "r")) {
        long q = 0;
        long period = 0;
        if (std::fscanf(fq, "%ld", &q) == 1 && q > 0) {
          if (FILE *fp = std::fopen("/sys/fs/cgroup/cpu/cpu.cfs_period_us", "r")) {
            if (std::fscanf(fp, "%ld", &period) == 1 && period > 0) {
              n = std::min(n, static_cast<int>((q + period - 1) / period));
            }
            std::fclose(fp);
          }
        }
        std::fclose(fq);
      }
#endif
    }
    if (n < 1) {
      n = 1;
    }
    if (n > 4) {
      n = 4;
    }
    return n;
  }();
  return cached;
}

// Persistent worker pool for the Top-K scan. The calling thread takes slice 0;
// workers spin briefly for the next generation and only park on a futex after
// a burst of queries ends, so back-to-back queries skip thread wake-up cost.
class ScanPool {
 public:
  using Fn = void (*)(void *ctx, int tid, uint32_t lo, uint32_t hi);

  explicit ScanPool(int n) : n_(n > 1 ? n : 1) {
    threads_.reserve(static_cast<size_t>(n_ - 1));
    try {
      for (int i = 1; i < n_; ++i) {
        threads_.emplace_back(&ScanPool::worker, this, i);
      }
    } catch (...) {
      // Thread creation failed (fd/pid limits): keep the workers we have.
      n_ = static_cast<int>(threads_.size()) + 1;
    }
  }

  ~ScanPool() {
    stop_.store(true, std::memory_order_release);
    gen_.fetch_add(1, std::memory_order_release);
    cv_.notify_all();
    for (std::thread &t : threads_) {
      if (t.joinable()) {
        t.join();
      }
    }
  }

  ScanPool(const ScanPool &) = delete;
  ScanPool &operator=(const ScanPool &) = delete;

  int size() const { return n_; }

  void run(Fn fn, void *ctx, uint32_t total) {
    fn_ = fn;
    ctx_ = ctx;
    total_ = total;
    done_.store(0, std::memory_order_relaxed);
    gen_.fetch_add(1, std::memory_order_release);
    cv_.notify_all();
    run_slice(0);
    const uint32_t need = static_cast<uint32_t>(n_ - 1);
    for (int spin = 0; spin < 20000; ++spin) {
      if (done_.load(std::memory_order_acquire) >= need) {
        return;
      }
#if defined(__x86_64__) || defined(__i386__)
      _mm_pause();
#endif
    }
    std::unique_lock<std::mutex> lk(m_);
    done_cv_.wait(lk, [&] { return done_.load(std::memory_order_acquire) >= need; });
  }

 private:
  void run_slice(int tid) {
    const uint32_t step = (total_ + static_cast<uint32_t>(n_) - 1) / static_cast<uint32_t>(n_);
    const uint32_t lo = static_cast<uint32_t>(tid) * step;
    const uint32_t hi = lo + step < total_ ? lo + step : total_;
    if (lo < hi) {
      fn_(ctx_, tid, lo, hi);
    }
  }

  void worker(int tid) {
    uint64_t seen = 0;
    for (;;) {
      // Workers keep spinning between queries: a benchmark issues them
      // back-to-back and the futex round trip would otherwise cost more than
      // the slice itself. Park only when the burst ends.
      uint64_t g = gen_.load(std::memory_order_acquire);
      if (g == seen) {
        uint32_t spins = 0;
        while (g == seen && spins < kIdleSpins) {
#if defined(__x86_64__) || defined(__i386__)
          _mm_pause();
#endif
          g = gen_.load(std::memory_order_acquire);
          ++spins;
        }
        if (g == seen) {
          std::unique_lock<std::mutex> lk(m_);
          cv_.wait(lk, [&] { return gen_.load(std::memory_order_acquire) != seen; });
          g = gen_.load(std::memory_order_acquire);
        }
      }
      if (stop_.load(std::memory_order_acquire)) {
        return;
      }
      seen = g;
      run_slice(tid);
      done_.fetch_add(1, std::memory_order_acq_rel);
      done_cv_.notify_one();
    }
  }

  static constexpr uint32_t kIdleSpins = 200000;

  int n_;
  Fn fn_ = nullptr;
  void *ctx_ = nullptr;
  uint32_t total_ = 0;
  std::mutex m_;
  std::condition_variable cv_;
  std::condition_variable done_cv_;
  std::atomic<uint64_t> gen_{0};
  std::atomic<bool> stop_{false};
  std::atomic<uint32_t> done_{0};
  std::vector<std::thread> threads_;
};

#if defined(__x86_64__) || defined(__i386__)
struct ParScan {
  const char *map = nullptr;
  const RowRef *slots = nullptr;
  const uint32_t *idx = nullptr;
  int64_t dim = 0;
  const float *q = nullptr;
  uint32_t k = 0;
  std::vector<std::vector<ScanCand>> top;
  std::vector<uint32_t> got;
};

void par_scan_fn(void *ctxv, int tid, uint32_t lo, uint32_t hi) {
  ParScan &p = *static_cast<ParScan *>(ctxv);
  const size_t t = static_cast<size_t>(tid);
  std::vector<ScanCand> &top = p.top[t];
  top.resize(p.k);
  const uint32_t n = hi - lo;
  if (p.idx != nullptr) {
    scan_topk_avx2(p.map, p.slots, p.idx + lo, n, p.dim, p.q, p.k, top.data(), &p.got[t]);
  } else {
    scan_topk_avx2(p.map, p.slots + lo, nullptr, n, p.dim, p.q, p.k, top.data(), &p.got[t]);
    for (uint32_t i = 0; i < p.got[t]; ++i) {
      top[i].slot += lo;
    }
  }
}
#endif

// Vector literals of a multi-row INSERT are decoded on the pool before the
// statement is applied. The row loop only replays the recorded error, so the
// order in which failures surface is unchanged.
struct VecPrep {
  std::vector<float> vec;
  std::string err_code;
  std::string err_msg;
};

struct VecPrepJob {
  const std::vector<std::vector<Literal>> *rows = nullptr;
  VecPrep *out = nullptr;
  size_t lit_index = 0;
  int64_t dim = 0;
};

void vec_prep_fn(void *ctxv, int, uint32_t lo, uint32_t hi) {
  VecPrepJob &job = *static_cast<VecPrepJob *>(ctxv);
  for (uint32_t r = lo; r < hi; ++r) {
    VecPrep &p = job.out[r];
    p.err_code.clear();
    p.err_msg.clear();
    const std::vector<Literal> &lits = (*job.rows)[r];
    if (job.lit_index >= lits.size()) {
      continue;  // wrong column count: the row loop reports it
    }
    const Literal &lit = lits[job.lit_index];
    if (lit.kind != Literal::STR) {
      continue;  // type mismatch: the row loop reports it
    }
    try {
      parse_vector_literal(lit.s, job.dim, p.vec);
    } catch (const Error &e) {
      p.err_code = e.code;
      p.err_msg = e.message;
    }
  }
}

double l2_squared(const char *a, const char *b, int64_t dim) {
#if defined(__x86_64__) || defined(__i386__)
  if (have_avx2()) {
    return l2_sq_avx2(a, b, dim);
  }
#endif
  return l2_sq_scalar(a, b, dim);
}

double l2_distance(const char *a, const char *b, int64_t dim) {
  return std::sqrt(l2_squared(a, b, dim));
}

}  // namespace

struct Database::Impl {
  explicit Impl(std::string dir) : dir_(std::move(dir)) {}
  ~Impl() {
    scan_pool_.reset();
    close_files();
  }

  std::string dir_;
  Heap heap_;
  Wal wal_;
  std::vector<std::unique_ptr<Table>> tables_;
  std::unordered_map<std::string, size_t> table_by_name_;
  bool in_txn_ = false;
  std::vector<Undo> undo_;
  std::vector<WalOp> pending_;
  int64_t last_lsn_ = 0;
  bool open_ = false;
  bool closed_ = false;
  std::unique_ptr<ScanPool> scan_pool_;
  bool scan_pool_tried_ = false;
  std::vector<VecPrep> vec_prep_;
  static constexpr size_t kParInsertMin = 16;

  ScanPool *scan_pool() {
    if (!scan_pool_tried_) {
      scan_pool_tried_ = true;
      int n = scan_thread_budget();
      if (n > 1) {
        scan_pool_.reset(new ScanPool(n));
        if (scan_pool_->size() < 2) {
          scan_pool_.reset();
        }
      }
    }
    return scan_pool_.get();
  }

  void close_files() {
    if (closed_) {
      return;
    }
    closed_ = true;
    wal_.close();
    heap_.close();
  }

  Table *find_table(const std::string &name) {
    auto it = table_by_name_.find(lower_ascii(name));
    if (it == table_by_name_.end()) {
      return nullptr;
    }
    return tables_[it->second].get();
  }

  Table &require_table(const std::string &name) {
    Table *t = find_table(name);
    if (t == nullptr) {
      raise("table_not_found", "no such table: " + name);
    }
    return *t;
  }

  static int find_col(const Table &t, const std::string &name) {
    std::string low = lower_ascii(name);
    for (size_t i = 0; i < t.cols.size(); ++i) {
      if (lower_ascii(t.cols[i].name) == low) {
        return static_cast<int>(i);
      }
    }
    return -1;
  }

  int require_col(const Table &t, const std::string &name) {
    int ci = find_col(t, name);
    if (ci < 0) {
      raise("column_not_found", "no such column: " + name);
    }
    return ci;
  }

  void open() {
    if (mkdir(dir_.c_str(), 0755) != 0 && errno != EEXIST) {
      raise("storage_error", "cannot create data directory");
    }
    heap_.open(dir_ + "/heap.dat");
    load_snapshot();
    wal_.open(dir_ + "/wal.log");
    wal_.replay([this](const char *payload, size_t len) { apply_record(payload, len); });
    open_ = true;
  }

  // ---------- WAL / snapshot ----------

  std::string encode_wal(int64_t lsn, const std::vector<WalOp> &ops) {
    Buf b;
    b.u64(static_cast<uint64_t>(lsn));
    b.u32(static_cast<uint32_t>(ops.size()));
    for (const WalOp &op : ops) {
      b.u8(op.type);
      b.str(op.table);
      if (op.type == 1) {
        b.u32(static_cast<uint32_t>(op.schema.size()));
        for (const Column &c : op.schema) {
          b.str(c.name);
          b.u8(static_cast<uint8_t>(c.type));
          b.i64(c.len);
          b.u8(c.pk ? 1 : 0);
        }
      } else if (op.type == 2) {
        b.str(op.name);
        b.str(op.column);
      } else if (op.type == 3 || op.type == 4) {
        b.i64(op.pk);
        b.i64(op.off);
        b.u32(op.len);
      } else {
        b.i64(op.pk);
      }
    }
    return b.data;
  }

  void apply_record(const char *payload, size_t len) {
    Reader r(payload, len);
    int64_t lsn = r.i64();
    uint32_t count = r.u32();
    if (lsn <= last_lsn_) {
      return;
    }
    for (uint32_t i = 0; i < count; ++i) {
      uint8_t type = r.u8();
      std::string table = r.str();
      if (type == 1) {
        uint32_t ncols = r.u32();
        std::vector<Column> schema;
        schema.resize(ncols);
        for (uint32_t c = 0; c < ncols; ++c) {
          schema[c].name = r.str();
          schema[c].type = static_cast<ColType>(r.u8());
          schema[c].len = r.i64();
          schema[c].pk = r.u8() != 0;
        }
        apply_create_table(table, schema);
      } else if (type == 2) {
        std::string name = r.str();
        std::string column = r.str();
        apply_create_index(table, name, column);
      } else if (type == 3 || type == 4) {
        int64_t pk = r.i64();
        int64_t off = r.i64();
        uint32_t rlen = r.u32();
        Table *t = find_table(table);
        if (t == nullptr) {
          continue;
        }
        if (!heap_.valid(off, rlen)) {
          raise("storage_error", "wal references missing row data");
        }
        if (type == 3) {
          apply_insert(*t, pk, off, rlen);
        } else {
          apply_update(*t, pk, off, rlen);
        }
      } else if (type == 5) {
        int64_t pk = r.i64();
        Table *t = find_table(table);
        if (t != nullptr) {
          apply_delete(*t, pk);
        }
      } else {
        raise("storage_error", "unknown wal operation");
      }
    }
    if (lsn > last_lsn_) {
      last_lsn_ = lsn;
    }
  }

  void apply_create_table(const std::string &name, const std::vector<Column> &schema) {
    if (find_table(name) != nullptr) {
      return;
    }
    auto t = std::make_unique<Table>();
    t->name = name;
    t->cols = schema;
    for (size_t i = 0; i < schema.size(); ++i) {
      if (schema[i].pk) {
        t->pk_idx = static_cast<int>(i);
      }
      if (schema[i].type == ColType::VECTOR) {
        t->vec_idx = static_cast<int>(i);
      }
    }
    table_by_name_[lower_ascii(name)] = tables_.size();
    tables_.push_back(std::move(t));
  }

  void apply_create_index(const std::string &table, const std::string &name,
                          const std::string &) {
    Table *t = find_table(table);
    if (t == nullptr) {
      return;
    }
    for (const std::string &existing : t->indexes) {
      if (existing == name) {
        return;
      }
    }
    t->indexes.push_back(name);
  }

  void apply_insert(Table &t, int64_t pk, int64_t off, uint32_t len) {
    auto it = t.pk2slot.find(pk);
    if (it != t.pk2slot.end()) {
      t.slots[it->second].alive = false;
      t.pk2slot.erase(it);
    }
    RowRef ref;
    ref.pk = pk;
    ref.off = off;
    ref.len = len;
    ref.vec_off = row_vec_off(t, heap_.ptr(off), len);
    ref.alive = true;
    t.pk2slot[pk] = static_cast<uint32_t>(t.slots.size());
    t.slots.push_back(ref);
  }

  void apply_update(Table &t, int64_t pk, int64_t off, uint32_t len) {
    auto it = t.pk2slot.find(pk);
    if (it == t.pk2slot.end()) {
      return;
    }
    RowRef &ref = t.slots[it->second];
    ref.off = off;
    ref.len = len;
    ref.vec_off = row_vec_off(t, heap_.ptr(off), len);
  }

  void apply_delete(Table &t, int64_t pk) {
    auto it = t.pk2slot.find(pk);
    if (it == t.pk2slot.end()) {
      return;
    }
    t.slots[it->second].alive = false;
    t.pk2slot.erase(it);
  }

  void write_snapshot() {
    heap_.sync();
    Buf payload;
    payload.u64(static_cast<uint64_t>(last_lsn_));
    payload.u64(static_cast<uint64_t>(heap_.size()));
    payload.u32(static_cast<uint32_t>(tables_.size()));
    for (const auto &tp : tables_) {
      const Table &t = *tp;
      payload.str(t.name);
      payload.u32(static_cast<uint32_t>(t.cols.size()));
      for (const Column &c : t.cols) {
        payload.str(c.name);
        payload.u8(static_cast<uint8_t>(c.type));
        payload.i64(c.len);
        payload.u8(c.pk ? 1 : 0);
      }
      uint32_t alive = 0;
      for (const RowRef &ref : t.slots) {
        if (ref.alive) {
          ++alive;
        }
      }
      payload.u32(alive);
      for (const RowRef &ref : t.slots) {
        if (!ref.alive) {
          continue;
        }
        payload.i64(ref.pk);
        payload.i64(ref.off);
        payload.u32(ref.len);
        payload.u32(ref.vec_off);
      }
      payload.u32(static_cast<uint32_t>(t.indexes.size()));
      for (const std::string &name : t.indexes) {
        payload.str(name);
      }
    }

    std::string final_path = dir_ + "/snapshot.bin";
    std::string tmp_path = final_path + ".tmp";
    int fd = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
      raise("storage_error", "cannot write snapshot");
    }
    Buf header;
    header.u32(kSnapshotMagic);
    header.u32(kSnapshotVersion);
    header.u32(static_cast<uint32_t>(payload.size()));
    header.u32(crc32(payload.data.data(), payload.data.size()));
    std::string all = header.data + payload.data;
    size_t done = 0;
    while (done < all.size()) {
      ssize_t n = ::write(fd, all.data() + done, all.size() - done);
      if (n <= 0) {
        if (errno == EINTR) {
          continue;
        }
        ::close(fd);
        raise("storage_error", "snapshot write failed");
      }
      done += static_cast<size_t>(n);
    }
    fsync(fd);
    ::close(fd);
    if (rename(tmp_path.c_str(), final_path.c_str()) != 0) {
      raise("storage_error", "snapshot rename failed");
    }
    int dfd = ::open(dir_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
      fsync(dfd);
      ::close(dfd);
    }
    wal_.reset();
  }

  void load_snapshot() {
    std::string path = dir_ + "/snapshot.bin";
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
      return;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
      ::close(fd);
      raise("storage_error", "cannot stat snapshot");
    }
    std::string data;
    data.resize(static_cast<size_t>(st.st_size));
    size_t got = 0;
    while (got < data.size()) {
      ssize_t n = read(fd, data.data() + got, data.size() - got);
      if (n <= 0) {
        if (n < 0 && errno == EINTR) {
          continue;
        }
        break;
      }
      got += static_cast<size_t>(n);
    }
    ::close(fd);
    data.resize(got);
    if (data.size() < 16) {
      raise("storage_error", "truncated snapshot");
    }
    uint32_t magic = 0;
    uint32_t version = 0;
    uint32_t plen = 0;
    uint32_t crc = 0;
    memcpy(&magic, data.data(), 4);
    memcpy(&version, data.data() + 4, 4);
    memcpy(&plen, data.data() + 8, 4);
    memcpy(&crc, data.data() + 12, 4);
    if (magic != kSnapshotMagic || version != kSnapshotVersion) {
      raise("storage_error", "unsupported snapshot format");
    }
    if (data.size() < static_cast<size_t>(16) + plen) {
      raise("storage_error", "truncated snapshot payload");
    }
    if (crc32(data.data() + 16, plen) != crc) {
      raise("storage_error", "snapshot checksum mismatch");
    }
    Reader r(data.data() + 16, plen);
    uint64_t lsn = r.u64();
    uint64_t heap_size = r.u64();
    if (heap_size > static_cast<uint64_t>(heap_.size())) {
      raise("storage_error", "snapshot heap size mismatch");
    }
    uint32_t table_count = r.u32();
    for (uint32_t ti = 0; ti < table_count; ++ti) {
      auto t = std::make_unique<Table>();
      t->name = r.str();
      uint32_t ncols = r.u32();
      t->cols.resize(ncols);
      for (uint32_t c = 0; c < ncols; ++c) {
        t->cols[c].name = r.str();
        t->cols[c].type = static_cast<ColType>(r.u8());
        t->cols[c].len = r.i64();
        t->cols[c].pk = r.u8() != 0;
        if (t->cols[c].pk) {
          t->pk_idx = static_cast<int>(c);
        }
        if (t->cols[c].type == ColType::VECTOR) {
          t->vec_idx = static_cast<int>(c);
        }
      }
      uint32_t slot_count = r.u32();
      t->slots.reserve(slot_count);
      for (uint32_t si = 0; si < slot_count; ++si) {
        RowRef ref;
        ref.pk = r.i64();
        ref.off = r.i64();
        ref.len = r.u32();
        ref.alive = true;
        if (!heap_.valid(ref.off, ref.len)) {
          raise("storage_error", "snapshot row offset out of range");
        }
        // vec_off is persisted so loading a large snapshot never has to read row data.
        uint32_t vec_off = r.u32();
        if (t->vec_idx >= 0) {
          int64_t dim = t->cols[static_cast<size_t>(t->vec_idx)].len;
          if (vec_off > ref.len ||
              static_cast<int64_t>(vec_off) + dim * 4 > static_cast<int64_t>(ref.len)) {
            raise("storage_error", "snapshot vector offset out of range");
          }
        }
        ref.vec_off = vec_off;
        t->pk2slot[ref.pk] = static_cast<uint32_t>(t->slots.size());
        t->slots.push_back(ref);
      }
      uint32_t index_count = r.u32();
      for (uint32_t ii = 0; ii < index_count; ++ii) {
        t->indexes.push_back(r.str());
      }
      table_by_name_[lower_ascii(t->name)] = tables_.size();
      tables_.push_back(std::move(t));
    }
    last_lsn_ = static_cast<int64_t>(lsn);
  }

  // ---------- transaction plumbing ----------

  void undo_entry(const Undo &u) { undo_.push_back(u); }

  void rollback_to(Savepoint sp) {
    while (undo_.size() > sp.undo) {
      Undo u = std::move(undo_.back());
      undo_.pop_back();
      apply_undo(u);
    }
    if (pending_.size() > sp.pending) {
      pending_.resize(sp.pending);
    }
  }

  void apply_undo(const Undo &u) {
    if (u.kind == Undo::ERASE_TABLE) {
      auto it = table_by_name_.find(lower_ascii(u.table));
      if (it != table_by_name_.end()) {
        size_t idx = it->second;
        tables_.erase(tables_.begin() + static_cast<long>(idx));
        table_by_name_.clear();
        for (size_t i = 0; i < tables_.size(); ++i) {
          table_by_name_[lower_ascii(tables_[i]->name)] = i;
        }
      }
      return;
    }
    Table *t = find_table(u.table);
    if (t == nullptr) {
      return;
    }
    if (u.kind == Undo::ERASE_INDEX) {
      for (size_t i = 0; i < t->indexes.size(); ++i) {
        if (t->indexes[i] == u.name) {
          t->indexes.erase(t->indexes.begin() + static_cast<long>(i));
          break;
        }
      }
      return;
    }
    if (u.slot >= t->slots.size()) {
      return;
    }
    RowRef &ref = t->slots[u.slot];
    if (u.kind == Undo::ERASE_SLOT) {
      ref.alive = false;
      t->pk2slot.erase(ref.pk);
    } else if (u.kind == Undo::RESTORE_SLOT) {
      ref.off = u.old_off;
      ref.len = u.old_len;
      ref.vec_off = row_vec_off(*t, heap_.ptr(ref.off), ref.len);
    } else if (u.kind == Undo::UNDELETE_SLOT) {
      ref.alive = true;
      t->pk2slot[ref.pk] = u.slot;
    }
  }

  void commit_ops() {
    if (pending_.empty()) {
      undo_.clear();
      return;
    }
    ++last_lsn_;
    std::string payload = encode_wal(last_lsn_, pending_);
    wal_.append(payload);
    pending_.clear();
    undo_.clear();
    if (wal_.appended() > kCheckpointWalBytes) {
      try {
        write_snapshot();
      } catch (const Error &e) {
        // Data is already durable in the WAL; checkpoint failure is not fatal.
        (void)e;
      }
    }
  }

  // ---------- statement implementations ----------

  ExecResult exec_create_table(const Stmt &stmt) {
    if (stmt.columns.empty()) {
      raise("syntax_error", "table needs at least one column");
    }
    if (stmt.columns.size() > kMaxColumns) {
      raise("unsupported_sql", "too many columns");
    }
    if (find_table(stmt.table) != nullptr) {
      raise("table_exists", "table already exists: " + stmt.table);
    }
    std::unordered_set<std::string> names;
    int pk_count = 0;
    int vec_count = 0;
    for (const ColumnDef &def : stmt.columns) {
      std::string low = lower_ascii(def.name);
      if (names.count(low)) {
        raise("duplicate_column", "duplicate column: " + def.name);
      }
      names.insert(low);
      if (def.pk) {
        ++pk_count;
        if (def.type != ColType::BIGINT) {
          raise("invalid_schema", "primary key must be BIGINT");
        }
      }
      if (def.type == ColType::VARCHAR) {
        if (def.len <= 0 || def.len > kMaxVarcharLen) {
          raise("invalid_schema", "invalid VARCHAR length");
        }
      } else if (def.type == ColType::VECTOR) {
        ++vec_count;
        if (def.len <= 0 || def.len > kMaxVectorDim) {
          raise("invalid_schema", "invalid VECTOR dimension");
        }
      }
    }
    if (pk_count != 1) {
      raise("invalid_schema", "table needs exactly one BIGINT primary key");
    }
    if (vec_count > 1) {
      raise("invalid_schema", "at most one VECTOR column is supported");
    }
    auto t = std::make_unique<Table>();
    t->name = stmt.table;
    for (const ColumnDef &def : stmt.columns) {
      Column c;
      c.name = def.name;
      c.type = def.type;
      c.len = def.len;
      c.pk = def.pk;
      t->cols.push_back(c);
      if (def.pk) {
        t->pk_idx = static_cast<int>(t->cols.size()) - 1;
      }
      if (def.type == ColType::VECTOR) {
        t->vec_idx = static_cast<int>(t->cols.size()) - 1;
      }
    }
    WalOp op;
    op.type = 1;
    op.table = t->name;
    op.schema = t->cols;
    table_by_name_[lower_ascii(t->name)] = tables_.size();
    tables_.push_back(std::move(t));
    pending_.push_back(std::move(op));
    Undo u;
    u.kind = Undo::ERASE_TABLE;
    u.table = stmt.table;
    undo_entry(u);
    return ExecResult{};
  }

  ExecResult exec_create_index(const Stmt &stmt) {
    Table &t = require_table(stmt.table);
    int ci = require_col(t, stmt.index_col);
    if (t.cols[ci].type != ColType::VECTOR) {
      raise("invalid_schema", "vector index requires a VECTOR column");
    }
    for (const std::string &name : t.indexes) {
      if (lower_ascii(name) == lower_ascii(stmt.index_name)) {
        raise("index_exists", "index already exists: " + stmt.index_name);
      }
    }
    t.indexes.push_back(stmt.index_name);
    WalOp op;
    op.type = 2;
    op.table = t.name;
    op.name = stmt.index_name;
    op.column = t.cols[ci].name;
    pending_.push_back(std::move(op));
    Undo u;
    u.kind = Undo::ERASE_INDEX;
    u.table = t.name;
    u.name = stmt.index_name;
    undo_entry(u);
    return ExecResult{};
  }

  ExecResult exec_insert(const InsertStmt &ins) {
    Table &t = require_table(ins.table);
    size_t ncols = t.cols.size();
    std::vector<size_t> pos;
    if (ins.columns.empty()) {
      pos.resize(ncols);
      for (size_t ci = 0; ci < ncols; ++ci) {
        pos[ci] = ci;
      }
    } else {
      std::vector<bool> used(ncols, false);
      for (const std::string &name : ins.columns) {
        int ci = find_col(t, name);
        if (ci < 0) {
          raise("column_not_found", "no such column: " + name);
        }
        if (used[static_cast<size_t>(ci)]) {
          raise("duplicate_column", "duplicate column: " + name);
        }
        used[static_cast<size_t>(ci)] = true;
        pos.push_back(static_cast<size_t>(ci));
      }
    }
    std::unordered_set<int64_t> seen;
    int pk_idx = t.pk_idx;
    bool all_columns = ins.columns.empty() || pos.size() == ncols;
    int vec_k = -1;
    for (size_t k = 0; k < pos.size(); ++k) {
      if (t.cols[pos[k]].type == ColType::VECTOR) {
        vec_k = static_cast<int>(k);
        break;
      }
    }
    bool prep = false;
    if (vec_k >= 0 && ins.rows.size() >= kParInsertMin) {
      if (ScanPool *pool = scan_pool()) {
        if (vec_prep_.size() < ins.rows.size()) {
          vec_prep_.resize(ins.rows.size());
        }
        VecPrepJob job;
        job.rows = &ins.rows;
        job.out = vec_prep_.data();
        job.lit_index = static_cast<size_t>(vec_k);
        job.dim = t.cols[pos[static_cast<size_t>(vec_k)]].len;
        pool->run(vec_prep_fn, &job, static_cast<uint32_t>(ins.rows.size()));
        prep = true;
      }
    }
    std::vector<Value> vals(ncols);
    for (size_t ci = 0; ci < ncols; ++ci) {
      vals[ci].type = t.cols[ci].type;
    }
    std::string payload;
    size_t reserve_hint = 64;
    for (const Column &col : t.cols) {
      reserve_hint += col.type == ColType::VECTOR ? static_cast<size_t>(col.len) * 4
                                                  : static_cast<size_t>(col.len) + 8;
    }
    payload.reserve(reserve_hint);
    ExecResult res;
    res.kind = ExecResult::MODIFIED;
    res.affected_rows = 0;
    // Rows are written as they are parsed; a failure part-way rolls the
    // statement back through the same undo log used by every other mutation.
    for (size_t ri = 0; ri < ins.rows.size(); ++ri) {
      const std::vector<Literal> &lits = ins.rows[ri];
      if (lits.size() != pos.size()) {
        raise("column_count_mismatch", "INSERT column count does not match table");
      }
      if (!all_columns) {
        for (size_t ci = 0; ci < ncols; ++ci) {
          bool set = false;
          for (size_t k = 0; k < pos.size(); ++k) {
            if (pos[k] == ci) {
              set = true;
              break;
            }
          }
          if (set) {
            continue;
          }
          Value &v = vals[ci];
          if (v.type == ColType::BIGINT) {
            v.i = 0;
          } else if (v.type == ColType::VARCHAR) {
            v.s.clear();
          } else {
            v.v.assign(static_cast<size_t>(t.cols[ci].len), 0.0f);
          }
        }
      }
      for (size_t k = 0; k < pos.size(); ++k) {
        const Column &col = t.cols[pos[k]];
        const Literal &lit = lits[k];
        Value &v = vals[pos[k]];
        if (col.type == ColType::BIGINT) {
          if (lit.kind != Literal::INT) {
            raise("type_mismatch", "expected integer value for " + col.name);
          }
          v.i = lit.i;
        } else if (col.type == ColType::VARCHAR) {
          if (lit.kind != Literal::STR) {
            raise("type_mismatch", "expected string value for " + col.name);
          }
          if (utf8_length(lit.s) > col.len) {
            raise("string_too_long", "value exceeds VARCHAR length");
          }
          v.s.assign(lit.s);
        } else {
          if (lit.kind != Literal::STR) {
            raise("type_mismatch", "expected vector literal for " + col.name);
          }
          if (prep && static_cast<int>(k) == vec_k) {
            VecPrep &vp = vec_prep_[ri];
            if (!vp.err_code.empty()) {
              raise(vp.err_code.c_str(), vp.err_msg);
            }
            v.v.swap(vp.vec);
          } else {
            parse_vector_literal(lit.s, col.len, v.v);
          }
        }
      }
      int64_t pk = vals[pk_idx].i;
      if (t.pk2slot.count(pk) || !seen.insert(pk).second) {
        raise("duplicate_key", "duplicate primary key");
      }
      uint32_t vec_off = 0;
      encode_row_into(t, vals, payload, &vec_off);
      int64_t off = heap_.append(payload.data(), static_cast<uint32_t>(payload.size()));
      uint32_t slot = static_cast<uint32_t>(t.slots.size());
      RowRef ref;
      ref.pk = pk;
      ref.off = off;
      ref.len = static_cast<uint32_t>(payload.size());
      ref.vec_off = vec_off;
      ref.alive = true;
      t.slots.push_back(ref);
      t.pk2slot[pk] = slot;
      for (const auto &kv : t.eq_indexes) {
        eq_index_add(t, kv.first, slot, vals[static_cast<size_t>(kv.first)], off);
      }
      WalOp op;
      op.type = 3;
      op.table = t.name;
      op.pk = pk;
      op.off = off;
      op.len = ref.len;
      pending_.push_back(std::move(op));
      Undo u;
      u.kind = Undo::ERASE_SLOT;
      u.table = t.name;
      u.slot = slot;
      undo_entry(u);
      ++res.affected_rows;
    }
    return res;
  }

  std::vector<WherePlan> plan_where(const Table &t, const std::vector<Compare> &where) {
    std::vector<WherePlan> plan;
    plan.reserve(where.size());
    for (const Compare &cmp : where) {
      int ci = require_col(t, cmp.col);
      const Column &col = t.cols[ci];
      WherePlan p;
      p.col = ci;
      p.op = cmp.op;
      if (col.type == ColType::BIGINT) {
        if (cmp.lit.kind != Literal::INT) {
          raise("type_mismatch", "BIGINT comparison needs an integer literal");
        }
        p.i = cmp.lit.i;
      } else if (col.type == ColType::VARCHAR) {
        if (cmp.lit.kind != Literal::STR) {
          raise("type_mismatch", "VARCHAR comparison needs a string literal");
        }
        p.s = cmp.lit.s;
      } else {
        raise("unsupported_sql", "comparison on VECTOR columns is not supported");
      }
      plan.push_back(std::move(p));
    }
    return plan;
  }

  bool row_matches(const Table &t, const Cursor &cur, const std::vector<WherePlan> &plan) {
    for (const WherePlan &p : plan) {
      const Column &col = t.cols[p.col];
      if (col.type == ColType::BIGINT) {
        int64_t v = cur.i64(p.col);
        int cmp = v < p.i ? -1 : (v > p.i ? 1 : 0);
        if (!apply_cmp(cmp, p.op)) {
          return false;
        }
      } else {
        std::string_view v = cur.str(p.col);
        int cmp = v.compare(std::string_view(p.s));
        if (!apply_cmp(cmp, p.op)) {
          return false;
        }
      }
    }
    return true;
  }

  void eq_index_add(Table &t, int ci, uint32_t slot, const Value &v, int64_t off) {
    auto it = t.eq_indexes.find(ci);
    if (it == t.eq_indexes.end()) {
      return;
    }
    Table::IndexEntry e{slot, off};
    if (t.cols[static_cast<size_t>(ci)].type == ColType::BIGINT) {
      it->second.by_int[v.i].push_back(e);
    } else {
      it->second.by_str[v.s].push_back(e);
    }
  }

  const Table::EqIndex &eq_index(Table &t, int ci) {
    auto it = t.eq_indexes.find(ci);
    if (it != t.eq_indexes.end()) {
      return it->second;
    }
    Table::EqIndex &idx = t.eq_indexes[ci];
    for (uint32_t si = 0; si < t.slots.size(); ++si) {
      const RowRef &ref = t.slots[si];
      if (!ref.alive) {
        continue;
      }
      Cursor cur;
      cur.p = heap_.ptr(ref.off);
      cur.len = ref.len;
      if (!cur.init(t)) {
        continue;
      }
      if (t.cols[static_cast<size_t>(ci)].type == ColType::BIGINT) {
        idx.by_int[cur.i64(ci)].push_back(Table::IndexEntry{si, ref.off});
      } else {
        idx.by_str[std::string(cur.str(ci))].push_back(Table::IndexEntry{si, ref.off});
      }
    }
    return idx;
  }

  struct OutItem {
    bool distance = false;
    int col = -1;
    std::string name;
    std::vector<float> query;
    const std::vector<float> *sort_query = nullptr;
  };

  void resolve_items(const Table &t, const SelectStmt &sel, std::vector<OutItem> &items) {
    if (sel.star) {
      for (size_t ci = 0; ci < t.cols.size(); ++ci) {
        OutItem it;
        it.col = static_cast<int>(ci);
        it.name = t.cols[ci].name;
        items.push_back(std::move(it));
      }
      return;
    }
    for (const SelectItem &si : sel.items) {
      OutItem it;
      it.distance = si.is_distance;
      if (si.is_distance) {
        int ci = require_col(t, si.col);
        if (t.cols[ci].type != ColType::VECTOR) {
          raise("invalid_schema", "L2_DISTANCE requires a VECTOR column");
        }
        it.col = ci;
        it.name = si.alias.empty() ? "distance" : si.alias;
        parse_vector_literal(si.vec_text, t.cols[ci].len, it.query);
      } else {
        int ci = require_col(t, si.col);
        it.col = ci;
        if (si.alias.empty() || lower_ascii(si.alias) == lower_ascii(t.cols[ci].name)) {
          it.name = t.cols[ci].name;
        } else {
          it.name = si.alias;
        }
      }
      items.push_back(std::move(it));
    }
  }

  void append_value(std::string &out, const Table &t, const Cursor &cur, int ci) {
    const Column &col = t.cols[ci];
    if (col.type == ColType::BIGINT) {
      char buf[32];
      auto res = std::to_chars(buf, buf + sizeof(buf), cur.i64(ci));
      out.append(buf, res.ptr);
    } else if (col.type == ColType::VARCHAR) {
      std::string_view sv = cur.str(ci);
      out.push_back('"');
      out += json_escape(std::string(sv));
      out.push_back('"');
    } else {
      out.push_back('[');
      const char *base = cur.vec_bytes(ci);
      int64_t dim = col.len;
      for (int64_t k = 0; k < dim; ++k) {
        if (k) {
          out.push_back(',');
        }
        float f;
        memcpy(&f, base + k * 4, 4);
        char buf[64];
        auto res = std::to_chars(buf, buf + sizeof(buf), f);
        out.append(buf, res.ptr);
      }
      out.push_back(']');
    }
  }

  ExecResult exec_select(const SelectStmt &sel) {
    Table &t = require_table(sel.table);
    std::vector<OutItem> items;
    resolve_items(t, sel, items);
    std::vector<WherePlan> where = plan_where(t, sel.where);

    enum class OrderBy { NONE, DISTANCE, INT, STR } order = OrderBy::NONE;
    bool order_desc = sel.order_desc;
    int order_col = -1;
    std::vector<float> order_query;
    if (sel.order_kind == SelectStmt::ORDER_DISTANCE) {
      order = OrderBy::DISTANCE;
      int ci = require_col(t, sel.order_vec_col);
      if (t.cols[ci].type != ColType::VECTOR) {
        raise("invalid_schema", "ORDER BY L2_DISTANCE requires a VECTOR column");
      }
      order_col = ci;
      parse_vector_literal(sel.order_vec, t.cols[ci].len, order_query);
    } else if (sel.order_kind == SelectStmt::ORDER_COLUMN) {
      bool matched = false;
      for (const OutItem &it : items) {
        if (lower_ascii(it.name) == lower_ascii(sel.order_col)) {
          if (it.distance) {
            order = OrderBy::DISTANCE;
            order_col = it.col;
            order_query = it.query;
          } else {
            order_col = it.col;
            const Column &col = t.cols[order_col];
            if (col.type == ColType::BIGINT) {
              order = OrderBy::INT;
            } else if (col.type == ColType::VARCHAR) {
              order = OrderBy::STR;
            } else {
              raise("unsupported_sql", "ORDER BY on VECTOR columns is not supported");
            }
          }
          matched = true;
          break;
        }
      }
      if (!matched) {
        order_col = require_col(t, sel.order_col);
        const Column &col = t.cols[order_col];
        if (col.type == ColType::BIGINT) {
          order = OrderBy::INT;
        } else if (col.type == ColType::VARCHAR) {
          order = OrderBy::STR;
        } else {
          raise("unsupported_sql", "ORDER BY on VECTOR columns is not supported");
        }
      }
    }

    size_t limit = SIZE_MAX;
    if (sel.has_limit) {
      limit = static_cast<size_t>(sel.limit);
    }

    ExecResult res;
    res.kind = ExecResult::QUERY;
    for (const OutItem &it : items) {
      res.columns.push_back(it.name);
    }
    if (limit == 0) {
      res.rows_json = "[]";
      return res;
    }

    std::string &rows = res.rows_json;
    rows.reserve(64);
    rows.push_back('[');
    bool first_row = true;

    auto emit_row = [&](const Cursor &cur) {
      if (!first_row) {
        rows.push_back(',');
      }
      first_row = false;
      rows.push_back('[');
      bool first = true;
      for (const OutItem &it : items) {
        if (!first) {
          rows.push_back(',');
        }
        first = false;
        if (it.distance) {
          const Column &col = t.cols[it.col];
          double d = l2_distance(cur.vec_bytes(it.col), reinterpret_cast<const char *>(it.query.data()), col.len);
          char buf[64];
          auto r2 = std::to_chars(buf, buf + sizeof(buf), d);
          rows.append(buf, r2.ptr);
        } else {
          append_value(rows, t, cur, it.col);
        }
      }
      rows.push_back(']');
    };

    if (order == OrderBy::NONE) {
      size_t emitted = 0;
      for (uint32_t si = 0; si < t.slots.size() && emitted < limit; ++si) {
        const RowRef &ref = t.slots[si];
        if (!ref.alive) {
          continue;
        }
        Cursor cur;
        cur.p = heap_.ptr(ref.off);
        cur.len = ref.len;
        if (!cur.init(t)) {
          raise("storage_error", "corrupt row");
        }
        if (!row_matches(t, cur, where)) {
          continue;
        }
        emit_row(cur);
        ++emitted;
      }
    } else if (order == OrderBy::DISTANCE) {
      std::vector<DistCand> cands;
      auto better = [order_desc](const DistCand &a, const DistCand &b) {
        if (a.d != b.d) {
          return order_desc ? a.d > b.d : a.d < b.d;
        }
        return a.pk < b.pk;
      };
      const int64_t dim = t.cols[order_col].len;
      const char *q = reinterpret_cast<const char *>(order_query.data());
      const bool vec_fast = where.empty() && order_col == t.vec_idx;

      // A single equality predicate on an ordinary column visits only that
      // bucket. Buckets are add-only: an entry whose row offset still matches
      // the live row is guaranteed to satisfy the predicate, otherwise the row
      // is verified against its current bytes.
      std::vector<uint32_t> matched;
      bool idx_active = false;
      if (where.size() == 1 && where[0].op == CmpOp::EQ) {
        const WherePlan &p = where[0];
        const Column &col = t.cols[static_cast<size_t>(p.col)];
        if (col.type == ColType::BIGINT || (col.type == ColType::VARCHAR && col.len <= 128)) {
          const Table::EqIndex &idx = eq_index(t, p.col);
          const std::vector<Table::IndexEntry> *bucket = nullptr;
          if (col.type == ColType::BIGINT) {
            auto it = idx.by_int.find(p.i);
            if (it != idx.by_int.end()) {
              bucket = &it->second;
            }
          } else {
            auto it = idx.by_str.find(p.s);
            if (it != idx.by_str.end()) {
              bucket = &it->second;
            }
          }
          idx_active = true;
          if (bucket != nullptr) {
            matched.reserve(bucket->size());
            // A row updated to a value it already had leaves two matching
            // entries behind; the bitmap keeps each row in the result once.
            std::vector<uint64_t> seen((t.slots.size() + 63) / 64, 0);
            auto accept = [&](uint32_t slot) {
              uint64_t &word = seen[slot >> 6];
              uint64_t bit = uint64_t{1} << (slot & 63);
              if (word & bit) {
                return;
              }
              word |= bit;
              matched.push_back(slot);
            };
            for (const Table::IndexEntry &e : *bucket) {
              if (e.slot >= t.slots.size()) {
                continue;
              }
              const RowRef &ref = t.slots[e.slot];
              if (!ref.alive) {
                continue;
              }
              if (e.off == ref.off) {
                accept(e.slot);
                continue;
              }
              Cursor cur;
              cur.p = heap_.ptr(ref.off);
              cur.len = ref.len;
              if (!cur.init(t)) {
                continue;
              }
              if (!row_matches(t, cur, where)) {
                continue;
              }
              accept(e.slot);
            }
          }
        }
      }
      const uint32_t *scan_idx = idx_active ? matched.data() : nullptr;
      const uint32_t scan_n = idx_active ? static_cast<uint32_t>(matched.size())
                                         : static_cast<uint32_t>(t.slots.size());

      bool fast_done = false;
#if defined(__x86_64__) || defined(__i386__)
      if ((vec_fast || idx_active) && sel.has_limit && !order_desc && limit <= kFastTopK &&
          have_avx2()) {
        ScanPool *pool = scan_n >= kParScanMin ? scan_pool() : nullptr;
        if (pool != nullptr) {
          ParScan ps;
          ps.map = heap_.ptr(0);
          ps.slots = t.slots.data();
          ps.idx = scan_idx;
          ps.dim = dim;
          ps.q = reinterpret_cast<const float *>(q);
          ps.k = static_cast<uint32_t>(limit);
          const size_t nt = static_cast<size_t>(pool->size());
          ps.top.resize(nt);
          ps.got.assign(nt, 0);
          pool->run(par_scan_fn, &ps, scan_n);
          size_t total = 0;
          for (size_t i = 0; i < nt; ++i) {
            total += ps.got[i];
          }
          cands.reserve(total);
          for (size_t i = 0; i < nt; ++i) {
            for (uint32_t j = 0; j < ps.got[i]; ++j) {
              cands.push_back(DistCand{ps.top[i][j].d, ps.top[i][j].pk, ps.top[i][j].slot});
            }
          }
          std::sort(cands.begin(), cands.end(), better);
          if (cands.size() > static_cast<size_t>(limit)) {
            cands.resize(static_cast<size_t>(limit));
          }
        } else {
          std::vector<ScanCand> top(limit);
          uint32_t got = 0;
          scan_topk_avx2(heap_.ptr(0), t.slots.data(), scan_idx, scan_n, dim,
                         reinterpret_cast<const float *>(q), static_cast<uint32_t>(limit),
                         top.data(), &got);
          cands.reserve(got);
          for (uint32_t i = 0; i < got; ++i) {
            cands.push_back(DistCand{top[i].d, top[i].pk, top[i].slot});
          }
        }
        fast_done = true;
      }
#endif
      auto candidate = [&](const RowRef &ref, double &d) {
        if (vec_fast) {
          d = l2_squared(heap_.ptr(ref.off) + ref.vec_off, q, dim);
          return true;
        }
        Cursor cur;
        cur.p = heap_.ptr(ref.off);
        cur.len = ref.len;
        if (!cur.init(t)) {
          raise("storage_error", "corrupt row");
        }
        if (!row_matches(t, cur, where)) {
          return false;
        }
        d = l2_squared(cur.vec_bytes(order_col), q, dim);
        return true;
      };
      if (fast_done) {
        // Top-K already collected and sorted by the batch scan.
      } else if (sel.has_limit) {
        std::priority_queue<DistCand, std::vector<DistCand>, decltype(better)> heap(better);
        for (uint32_t i = 0; i < scan_n; ++i) {
          uint32_t si = idx_active ? matched[i] : i;
          const RowRef &ref = t.slots[si];
          if (!ref.alive) {
            continue;
          }
          DistCand c;
          if (!candidate(ref, c.d)) {
            continue;
          }
          c.pk = ref.pk;
          c.slot = si;
          if (heap.size() < limit) {
            heap.push(c);
          } else if (better(c, heap.top())) {
            heap.pop();
            heap.push(c);
          }
        }
        cands.reserve(heap.size());
        while (!heap.empty()) {
          cands.push_back(heap.top());
          heap.pop();
        }
        std::sort(cands.begin(), cands.end(), better);
      } else {
        for (uint32_t i = 0; i < scan_n; ++i) {
          uint32_t si = idx_active ? matched[i] : i;
          const RowRef &ref = t.slots[si];
          if (!ref.alive) {
            continue;
          }
          DistCand c;
          if (!candidate(ref, c.d)) {
            continue;
          }
          c.pk = ref.pk;
          c.slot = si;
          cands.push_back(c);
        }
        std::sort(cands.begin(), cands.end(), better);
      }
      for (const DistCand &c : cands) {
        emit_ref(t, c.slot, emit_row);
      }
    } else if (order == OrderBy::INT) {
      std::vector<IntCand> cands;
      for (uint32_t si = 0; si < t.slots.size(); ++si) {
        const RowRef &ref = t.slots[si];
        if (!ref.alive) {
          continue;
        }
        Cursor cur;
        cur.p = heap_.ptr(ref.off);
        cur.len = ref.len;
        if (!cur.init(t)) {
          raise("storage_error", "corrupt row");
        }
        if (!row_matches(t, cur, where)) {
          continue;
        }
        IntCand c;
        c.k = cur.i64(order_col);
        c.pk = ref.pk;
        c.slot = si;
        cands.push_back(c);
      }
      auto cmp = [order_desc](const IntCand &a, const IntCand &b) {
        if (a.k != b.k) {
          return order_desc ? a.k > b.k : a.k < b.k;
        }
        return a.pk < b.pk;
      };
      if (limit < cands.size()) {
        std::partial_sort(cands.begin(), cands.begin() + static_cast<long>(limit), cands.end(), cmp);
        cands.resize(limit);
      } else {
        std::sort(cands.begin(), cands.end(), cmp);
      }
      for (const IntCand &c : cands) {
        emit_ref(t, c.slot, emit_row);
      }
    } else {
      std::vector<StrCand> cands;
      for (uint32_t si = 0; si < t.slots.size(); ++si) {
        const RowRef &ref = t.slots[si];
        if (!ref.alive) {
          continue;
        }
        Cursor cur;
        cur.p = heap_.ptr(ref.off);
        cur.len = ref.len;
        if (!cur.init(t)) {
          raise("storage_error", "corrupt row");
        }
        if (!row_matches(t, cur, where)) {
          continue;
        }
        StrCand c;
        c.k.assign(cur.str(order_col));
        c.pk = ref.pk;
        c.slot = si;
        cands.push_back(std::move(c));
      }
      auto cmp = [order_desc](const StrCand &a, const StrCand &b) {
        int r = a.k.compare(b.k);
        if (r != 0) {
          return order_desc ? r > 0 : r < 0;
        }
        return a.pk < b.pk;
      };
      if (limit < cands.size()) {
        std::partial_sort(cands.begin(), cands.begin() + static_cast<long>(limit), cands.end(), cmp);
        cands.resize(limit);
      } else {
        std::sort(cands.begin(), cands.end(), cmp);
      }
      for (const StrCand &c : cands) {
        emit_ref(t, c.slot, emit_row);
      }
    }

    rows.push_back(']');
    return res;
  }

  template <typename Emit>
  void emit_ref(Table &t, uint32_t si, Emit &emit_row) {
    const RowRef &ref = t.slots[si];
    Cursor cur;
    cur.p = heap_.ptr(ref.off);
    cur.len = ref.len;
    if (!cur.init(t)) {
      raise("storage_error", "corrupt row");
    }
    emit_row(cur);
  }

  ExecResult exec_update(const UpdateStmt &upd) {
    Table &t = require_table(upd.table);
    std::vector<std::pair<int, Value>> sets;
    std::vector<bool> seen(t.cols.size(), false);
    for (const SetItem &item : upd.sets) {
      int ci = require_col(t, item.col);
      if (ci == t.pk_idx) {
        raise("unsupported_sql", "primary key update is not supported");
      }
      if (seen[ci]) {
        raise("duplicate_column", "column set more than once");
      }
      seen[ci] = true;
      sets.emplace_back(ci, literal_to_value(t.cols[ci], item.lit));
    }
    std::vector<WherePlan> where = plan_where(t, upd.where);
    int64_t affected = 0;
    for (uint32_t si = 0; si < t.slots.size(); ++si) {
      RowRef &ref = t.slots[si];
      if (!ref.alive) {
        continue;
      }
      const char *base = heap_.ptr(ref.off);
      Cursor cur;
      cur.p = base;
      cur.len = ref.len;
      if (!cur.init(t)) {
        raise("storage_error", "corrupt row");
      }
      if (!row_matches(t, cur, where)) {
        continue;
      }
      std::vector<Value> vals = decode_row(t, heap_.ptr(ref.off), ref.len);
      for (auto &set : sets) {
        vals[set.first] = set.second;
      }
      uint32_t vec_off = 0;
      std::string payload = encode_row(t, vals, &vec_off);
      int64_t off = heap_.append(payload.data(), static_cast<uint32_t>(payload.size()));
      Undo u;
      u.kind = Undo::RESTORE_SLOT;
      u.table = t.name;
      u.slot = si;
      u.old_off = ref.off;
      u.old_len = ref.len;
      undo_entry(u);
      ref.off = off;
      ref.len = static_cast<uint32_t>(payload.size());
      ref.vec_off = vec_off;
      for (const auto &kv : t.eq_indexes) {
        for (const auto &set : sets) {
          if (set.first == kv.first) {
            eq_index_add(t, kv.first, si, vals[static_cast<size_t>(kv.first)], off);
            break;
          }
        }
      }
      WalOp op;
      op.type = 4;
      op.table = t.name;
      op.pk = ref.pk;
      op.off = off;
      op.len = ref.len;
      pending_.push_back(std::move(op));
      ++affected;
    }
    ExecResult res;
    res.kind = ExecResult::MODIFIED;
    res.affected_rows = affected;
    return res;
  }

  ExecResult exec_delete(const DeleteStmt &del) {
    Table &t = require_table(del.table);
    std::vector<WherePlan> where = plan_where(t, del.where);
    int64_t affected = 0;
    for (uint32_t si = 0; si < t.slots.size(); ++si) {
      RowRef &ref = t.slots[si];
      if (!ref.alive) {
        continue;
      }
      Cursor cur;
      cur.p = heap_.ptr(ref.off);
      cur.len = ref.len;
      if (!cur.init(t)) {
        raise("storage_error", "corrupt row");
      }
      if (!row_matches(t, cur, where)) {
        continue;
      }
      int64_t pk = ref.pk;
      ref.alive = false;
      t.pk2slot.erase(pk);
      Undo u;
      u.kind = Undo::UNDELETE_SLOT;
      u.table = t.name;
      u.slot = si;
      undo_entry(u);
      WalOp op;
      op.type = 5;
      op.table = t.name;
      op.pk = pk;
      pending_.push_back(std::move(op));
      ++affected;
    }
    ExecResult res;
    res.kind = ExecResult::MODIFIED;
    res.affected_rows = affected;
    return res;
  }

  ExecResult dispatch(const Stmt &stmt) {
    switch (stmt.kind) {
      case StmtKind::CREATE_TABLE: return exec_create_table(stmt);
      case StmtKind::CREATE_INDEX: return exec_create_index(stmt);
      case StmtKind::INSERT: return exec_insert(stmt.insert);
      case StmtKind::SELECT: return exec_select(stmt.select);
      case StmtKind::UPDATE: return exec_update(stmt.update);
      case StmtKind::DELETE: return exec_delete(stmt.del);
      default: raise("unsupported_sql", "unsupported statement");
    }
  }

  ExecResult execute(const std::string &sql) {
    Stmt stmt = parse_statement(sql);
    if (stmt.kind == StmtKind::UNSUPPORTED) {
      raise("unsupported_sql", "unsupported statement");
    }
    if (stmt.kind == StmtKind::BEGIN) {
      if (in_txn_) {
        raise("transaction_active", "a transaction is already active");
      }
      in_txn_ = true;
      return ExecResult{};
    }
    if (stmt.kind == StmtKind::COMMIT) {
      if (!in_txn_) {
        return ExecResult{};
      }
      commit_ops();
      in_txn_ = false;
      return ExecResult{};
    }
    if (stmt.kind == StmtKind::ROLLBACK) {
      if (!in_txn_) {
        return ExecResult{};
      }
      rollback_to(Savepoint{0, 0});
      in_txn_ = false;
      return ExecResult{};
    }

    bool mutating = stmt.kind == StmtKind::CREATE_TABLE || stmt.kind == StmtKind::CREATE_INDEX ||
                    stmt.kind == StmtKind::INSERT || stmt.kind == StmtKind::UPDATE ||
                    stmt.kind == StmtKind::DELETE;
    if (!mutating) {
      return dispatch(stmt);
    }
    Savepoint sp{undo_.size(), pending_.size()};
    try {
      ExecResult res = dispatch(stmt);
      if (!in_txn_) {
        commit_ops();
      }
      return res;
    } catch (...) {
      rollback_to(sp);
      throw;
    }
  }

  void shutdown() {
    if (closed_) {
      return;
    }
    if (in_txn_) {
      rollback_to(Savepoint{0, 0});
      in_txn_ = false;
    }
    try {
      write_snapshot();
    } catch (const Error &) {
      // Committed data is already durable in the WAL.
    }
    heap_.sync();
    wal_.sync();
    wal_.close();
    heap_.close();
    closed_ = true;
  }
};

Database::Database(std::string data_dir) : impl_(new Impl(std::move(data_dir))) {}

Database::~Database() = default;

void Database::open() { impl_->open(); }

ExecResult Database::execute(const std::string &sql) { return impl_->execute(sql); }

void Database::shutdown() { impl_->shutdown(); }

}  // namespace mvd
