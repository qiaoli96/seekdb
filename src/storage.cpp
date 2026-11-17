// MiniVectorDB 持久化层：catalog.dat（表定义）+ wal.log（追加事务日志）
//
// WAL 记录（全小端）：
//   [u32 payload_len][u32 crc32][u64 txn][u8 type][payload ...]
//   crc32 = CRC-32/IEEE over (txn 8B || type 1B || payload)，覆盖前缀里除 len 之外的
//   每一个字节，因此位翻转、半条记录、错位都会被同一次校验发现。
//   type: 1=Insert 2=Update 3=Delete 0xEE=COMMIT。COMMIT 的 payload_len = 0。
// 提交 = 整批记录 + COMMIT 记录一次 write + 恰好一次 fdatasync。
// 恢复 = 从头扫描：长度越界 / CRC 不匹配 / 记录不完整 → 立刻停止并丢弃尾部；
//        没有 COMMIT 收尾的事务整段丢弃（replay_groups 会先 abort_group 再截断文件）。
// 流式写：append 只入内存缓冲，flush 落地（不 fsync），commit 追加 COMMIT + fdatasync。
//        事务内每条语句 flush 一次，因此事务内也能读到自己的写；语句失败/事务回滚用
//        rollback_to 把文件截回语句/事务起点，未提交字节不会留在已提交区域里。
// 行存储：执行器直接把 wal.log 当行存储（每行一条 append-only 记录，含 CRC 与事务边界），
//        所以行内容只落这一份；内存里只保留槽位/存活位/pk 索引与有上限的行缓存。
//
// 文件头：magic(8) + version(u16) + 字节序标记(u16, 写 0x0102) + 保留(u32)。
// 目录布局与 README 一致：文件名直接位于 --data-dir 下。
//
// 仅依赖标准库 + POSIX（open/write/fdatasync/ftruncate/lseek）；
// _WIN32 分支只是同一组原语的薄封装，用于在 Windows 上直接跑本模块单测。
#include "storage.h"

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <utility>

#if defined(_WIN32)
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <sys/types.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {

static_assert(sizeof(float) == 4, "WAL/catalog layout assumes 32-bit IEEE-754 float");
static_assert(sizeof(double) == 8, "WAL layout assumes 64-bit IEEE-754 double");

// ------------------------------------------------------------------ 平台原语
#if defined(_WIN32)
int p_open(const std::string& path) {
  return ::_open(path.c_str(), _O_RDWR | _O_CREAT | _O_BINARY, _S_IREAD | _S_IWRITE);
}
int p_open_trunc(const std::string& path) {
  return ::_open(path.c_str(), _O_RDWR | _O_CREAT | _O_TRUNC | _O_BINARY, _S_IREAD | _S_IWRITE);
}
int p_open_read(const std::string& path) { return ::_open(path.c_str(), _O_RDONLY | _O_BINARY); }
int p_read(int fd, void* buf, size_t n) { return ::_read(fd, buf, static_cast<unsigned>(n)); }
int p_pread(int fd, void* buf, size_t n, int64_t off) {
  if (::_lseeki64(fd, off, SEEK_SET) < 0) return -1;
  return ::_read(fd, buf, static_cast<unsigned>(n));
}
int p_pwrite(int fd, const void* buf, size_t n, int64_t off) {
  if (::_lseeki64(fd, off, SEEK_SET) < 0) return -1;
  return ::_write(fd, buf, static_cast<unsigned>(n));
}
int p_write(int fd, const void* buf, size_t n) { return ::_write(fd, buf, static_cast<unsigned>(n)); }
bool p_fdatasync(int fd) { return ::_commit(fd) == 0; }
bool p_fsync(int fd) { return ::_commit(fd) == 0; }
bool p_ftruncate(int fd, uint64_t size) { return ::_chsize_s(fd, static_cast<long long>(size)) == 0; }
int64_t p_lseek(int fd, int64_t off, int whence) {
  return static_cast<int64_t>(::_lseeki64(fd, off, whence));
}
void p_close(int fd) { ::_close(fd); }
void p_unlink(const std::string& path) { ::_unlink(path.c_str()); }
bool p_rename(const std::string& from, const std::string& to) {
  p_unlink(to);  // Windows 的 rename 不覆盖已存在目标
  return std::rename(from.c_str(), to.c_str()) == 0;
}
bool p_mkdir_one(const std::string& path) { return ::_mkdir(path.c_str()) == 0 || errno == EEXIST; }
bool p_fsync_dir(const std::string&) { return true; }  // Windows 不支持对目录 fsync
#else
int p_open(const std::string& path) {
  return ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
}
int p_open_trunc(const std::string& path) {
  return ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
}
int p_open_read(const std::string& path) { return ::open(path.c_str(), O_RDONLY | O_CLOEXEC); }
int p_read(int fd, void* buf, size_t n) {
  return static_cast<int>(::read(fd, buf, n));
}
// 定位读写：同一 fd 上扫描与点查可交替进行，不依赖共享文件偏移。
int p_pread(int fd, void* buf, size_t n, int64_t off) {
  return static_cast<int>(::pread(fd, buf, n, static_cast<off_t>(off)));
}
int p_pwrite(int fd, const void* buf, size_t n, int64_t off) {
  return static_cast<int>(::pwrite(fd, buf, n, static_cast<off_t>(off)));
}
int p_write(int fd, const void* buf, size_t n) {
  return static_cast<int>(::write(fd, buf, n));
}
bool p_fdatasync(int fd) { return ::fdatasync(fd) == 0; }
bool p_fsync(int fd) { return ::fsync(fd) == 0; }
bool p_ftruncate(int fd, uint64_t size) { return ::ftruncate(fd, static_cast<off_t>(size)) == 0; }
int64_t p_lseek(int fd, int64_t off, int whence) {
  return static_cast<int64_t>(::lseek(fd, static_cast<off_t>(off), whence));
}
void p_close(int fd) { ::close(fd); }
void p_unlink(const std::string& path) { ::unlink(path.c_str()); }
bool p_rename(const std::string& from, const std::string& to) {
  return std::rename(from.c_str(), to.c_str()) == 0;
}
bool p_mkdir_one(const std::string& path) { return ::mkdir(path.c_str(), 0755) == 0 || errno == EEXIST; }
bool p_fsync_dir(const std::string& dir) {
  int fd = ::open(dir.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  bool ok = ::fsync(fd) == 0;
  ::close(fd);
  return ok;
}
#endif

bool write_all(int fd, const void* data, size_t n) {
  const char* p = static_cast<const char*>(data);
  size_t off = 0;
  while (off < n) {
    int w = p_write(fd, p + off, n - off);
    if (w < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (w == 0) return false;
    off += static_cast<size_t>(w);
  }
  return true;
}

bool write_all_at(int fd, const void* data, size_t n, uint64_t off) {
  const char* p = static_cast<const char*>(data);
  size_t done = 0;
  while (done < n) {
    int w = p_pwrite(fd, p + done, n - done, static_cast<int64_t>(off + done));
    if (w < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (w == 0) return false;
    done += static_cast<size_t>(w);
  }
  return true;
}

bool read_exact(int fd, void* data, size_t n) {
  char* p = static_cast<char*>(data);
  size_t off = 0;
  while (off < n) {
    int r = p_read(fd, p + off, n - off);
    if (r < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (r == 0) return false;
    off += static_cast<size_t>(r);
  }
  return true;
}

bool mkdir_all(const std::string& path) {
  if (path.empty()) return false;
  for (size_t i = 1; i < path.size(); ++i) {
    if (path[i] != '/' && path[i] != '\\') continue;
    std::string prefix = path.substr(0, i);
    if (prefix.size() == 2 && prefix[1] == ':') continue;  // 盘符本身
    if (!p_mkdir_one(prefix)) return false;
  }
  return p_mkdir_one(path);
}

// ------------------------------------------------------------------ CRC-32
constexpr uint32_t crc32_byte(uint32_t c) {
  for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
  return c;
}

struct Crc32Table {
  uint32_t t[256];
  constexpr Crc32Table() : t() {
    for (uint32_t i = 0; i < 256; ++i) t[i] = crc32_byte(i);
  }
};
constexpr Crc32Table kCrcTable{};

uint32_t crc32(uint32_t crc, const void* data, size_t n) {
  if (n == 0) return crc;
  const unsigned char* p = static_cast<const unsigned char*>(data);
  crc = ~crc;
  for (size_t i = 0; i < n; ++i) crc = kCrcTable.t[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
  return ~crc;
}

// ------------------------------------------------------------------ 小端编解码
void put_u16(std::vector<char>& b, uint16_t v) {
  b.push_back(static_cast<char>(v & 0xFFu));
  b.push_back(static_cast<char>((v >> 8) & 0xFFu));
}
void put_u32(std::vector<char>& b, uint32_t v) {
  for (int i = 0; i < 4; ++i) b.push_back(static_cast<char>((v >> (8 * i)) & 0xFFu));
}
void put_u64(std::vector<char>& b, uint64_t v) {
  for (int i = 0; i < 8; ++i) b.push_back(static_cast<char>((v >> (8 * i)) & 0xFFu));
}
void put_f32(std::vector<char>& b, float v) {
  uint32_t u = 0;
  std::memcpy(&u, &v, sizeof(u));
  put_u32(b, u);
}
void put_f64(std::vector<char>& b, double v) {
  uint64_t u = 0;
  std::memcpy(&u, &v, sizeof(u));
  put_u64(b, u);
}
void put_bytes(std::vector<char>& b, const std::string& s) {
  b.insert(b.end(), s.begin(), s.end());
}

uint32_t le_u32(const unsigned char* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint64_t le_u64(const unsigned char* p) {
  uint64_t lo = le_u32(p);
  uint64_t hi = le_u32(p + 4);
  return lo | (hi << 32);
}

struct Cur {
  const unsigned char* p = nullptr;
  size_t n = 0;
  size_t off = 0;

  bool has(size_t k) const { return n - off >= k; }
  bool u8(uint8_t& v) {
    if (!has(1)) return false;
    v = p[off];
    ++off;
    return true;
  }
  bool u32(uint32_t& v) {
    if (!has(4)) return false;
    v = le_u32(p + off);
    off += 4;
    return true;
  }
  bool u64(uint64_t& v) {
    if (!has(8)) return false;
    v = le_u64(p + off);
    off += 8;
    return true;
  }
  bool i32(int32_t& v) {
    uint32_t u = 0;
    if (!u32(u)) return false;
    v = static_cast<int32_t>(u);
    return true;
  }
  bool i64(int64_t& v) {
    uint64_t u = 0;
    if (!u64(u)) return false;
    v = static_cast<int64_t>(u);
    return true;
  }
  bool f32(float& v) {
    uint32_t u = 0;
    if (!u32(u)) return false;
    std::memcpy(&v, &u, sizeof(v));
    return true;
  }
  bool f64(double& v) {
    uint64_t u = 0;
    if (!u64(u)) return false;
    std::memcpy(&v, &u, sizeof(v));
    return true;
  }
  bool str(std::string& s) {
    uint32_t len = 0;
    if (!u32(len)) return false;
    if (!has(len)) return false;
    s.assign(reinterpret_cast<const char*>(p + off), len);
    off += len;
    return true;
  }
};

// ------------------------------------------------------------------ 行编码
enum : uint8_t { kVNull = 0, kVInt = 1, kVFloat = 2, kVStr = 3, kVVec = 4 };

void encode_value(std::vector<char>& b, const Value& v) {
  switch (v.k) {
    case Value::K::Null:
      b.push_back(static_cast<char>(kVNull));
      break;
    case Value::K::Int:
      b.push_back(static_cast<char>(kVInt));
      put_u64(b, static_cast<uint64_t>(v.i));
      break;
    case Value::K::Float:
      b.push_back(static_cast<char>(kVFloat));
      put_f64(b, v.d);
      break;
    case Value::K::Str:
      b.push_back(static_cast<char>(kVStr));
      put_u32(b, static_cast<uint32_t>(v.s.size()));
      put_bytes(b, v.s);
      break;
    case Value::K::Vec:
      b.push_back(static_cast<char>(kVVec));
      put_u32(b, static_cast<uint32_t>(v.v.size()));
      for (float f : v.v) put_f32(b, f);
      break;
  }
}

bool decode_value(Cur& c, Value& v) {
  uint8_t tag = 0;
  if (!c.u8(tag)) return false;
  switch (tag) {
    case kVNull:
      v = Value{};
      return true;
    case kVInt: {
      int64_t x = 0;
      if (!c.i64(x)) return false;
      v = Value::Int(x);
      return true;
    }
    case kVFloat: {
      double x = 0;
      if (!c.f64(x)) return false;
      v = Value::Float(x);
      return true;
    }
    case kVStr: {
      std::string s;
      if (!c.str(s)) return false;
      v = Value::Str(std::move(s));
      return true;
    }
    case kVVec: {
      uint32_t cnt = 0;
      if (!c.u32(cnt)) return false;
      if (!c.has(static_cast<size_t>(cnt) * 4)) return false;
      std::vector<float> xs(cnt);
      for (uint32_t i = 0; i < cnt; ++i) {
        float f = 0;
        if (!c.f32(f)) return false;
        xs[i] = f;
      }
      v = Value::Vec(std::move(xs));
      return true;
    }
    default:
      return false;
  }
}

// payload = [u32 table_len][table][u32 ncols][value ...]
void build_payload(const Mutation& m, std::vector<char>& out) {
  out.clear();
  put_u32(out, static_cast<uint32_t>(m.table.size()));
  put_bytes(out, m.table);
  put_u32(out, static_cast<uint32_t>(m.row.size()));
  for (const Value& v : m.row) encode_value(out, v);
}

bool parse_payload(const unsigned char* p, size_t n, Mutation& m) {
  Cur c{p, n, 0};
  uint32_t tlen = 0;
  if (!c.u32(tlen)) return false;
  if (!c.has(tlen)) return false;
  m.table.assign(reinterpret_cast<const char*>(p + c.off), tlen);
  c.off += tlen;
  uint32_t ncols = 0;
  if (!c.u32(ncols)) return false;
  if (static_cast<size_t>(ncols) > n) return false;  // 最小 1 字节/列，先做量级检查
  m.row.clear();
  m.row.reserve(ncols);
  for (uint32_t i = 0; i < ncols; ++i) {
    Value v;
    if (!decode_value(c, v)) return false;
    m.row.push_back(std::move(v));
  }
  return c.off == n;
}

// ------------------------------------------------------------------ 记录/文件头
constexpr size_t kWalHeader = 16;
constexpr size_t kCatalogHeader = 24;
constexpr size_t kRecPrefix = 17;  // u32 len + u32 crc + u64 txn + u8 type
constexpr size_t kScanChunk = 1u << 20;  // 顺序扫描读缓冲（大块少 syscall）
constexpr uint32_t kMaxPayload = 64u * 1024u * 1024u;
constexpr uint16_t kFormatVersion = 1;
constexpr uint16_t kEndianMark = 0x0102;
constexpr uint8_t kRecInsert = 1;
constexpr uint8_t kRecUpdate = 2;
constexpr uint8_t kRecDelete = 3;
constexpr uint8_t kRecCommit = 0xEE;

const char kWalMagic[8] = {'M', 'V', 'D', 'B', 'W', 'A', 'L', '1'};
const char kCatalogMagic[8] = {'M', 'V', 'D', 'B', 'C', 'A', 'T', '1'};

std::vector<char> make_wal_header() {
  std::vector<char> h;
  h.insert(h.end(), kWalMagic, kWalMagic + 8);
  put_u16(h, kFormatVersion);
  put_u16(h, kEndianMark);
  put_u32(h, 0);
  return h;
}

// 返回 0 = 合法；1 = 头损坏；-1 = 读 IO 失败
int check_wal_header(int fd, std::string& err) {
  if (p_lseek(fd, 0, SEEK_SET) != 0) {
    err = "storage: wal seek failed";
    return -1;
  }
  char buf[kWalHeader];
  if (!read_exact(fd, buf, sizeof(buf))) {
    err = "storage: wal header read failed";
    return -1;
  }
  const unsigned char* p = reinterpret_cast<const unsigned char*>(buf);
  if (std::memcmp(buf, kWalMagic, 8) != 0) {
    err = "storage: wal magic mismatch (not a MiniVectorDB wal)";
    return 1;
  }
  uint16_t ver = static_cast<uint16_t>(p[8] | (p[9] << 8));
  uint16_t endian = static_cast<uint16_t>(p[10] | (p[11] << 8));
  if (endian != kEndianMark) {
    err = "storage: wal byte-order marker mismatch";
    return 1;
  }
  if (ver != kFormatVersion) {
    err = "storage: unsupported wal version " + std::to_string(ver);
    return 1;
  }
  return 0;
}

uint8_t type_of(Mutation::Op op) {
  switch (op) {
    case Mutation::Op::Insert: return kRecInsert;
    case Mutation::Op::Update: return kRecUpdate;
    case Mutation::Op::Delete: return kRecDelete;
  }
  return kRecInsert;
}

Mutation::Op op_of(uint8_t type) {
  switch (type) {
    case kRecUpdate: return Mutation::Op::Update;
    case kRecDelete: return Mutation::Op::Delete;
    default: return Mutation::Op::Insert;
  }
}

void append_record(std::vector<char>& buf, uint64_t txn, uint8_t type,
                   const std::vector<char>& payload) {
  unsigned char head[9];
  for (int i = 0; i < 8; ++i) head[i] = static_cast<unsigned char>((txn >> (8 * i)) & 0xFFu);
  head[8] = type;
  uint32_t crc = crc32(0, head, sizeof(head));
  crc = crc32(crc, payload.data(), payload.size());
  put_u32(buf, static_cast<uint32_t>(payload.size()));
  put_u32(buf, crc);
  put_u64(buf, txn);
  buf.push_back(static_cast<char>(type));
  buf.insert(buf.end(), payload.begin(), payload.end());
}

// WAL 顺序读：够了就给指针，不够就说明尾部截断
class WalReader {
 public:
  WalReader(int fd, uint64_t start, size_t chunk)
      : fd_(fd), buf_start_(start), buf_(chunk < kChunk ? kChunk : chunk) {}

  bool ensure(size_t n) {
    if (n > buf_.size()) buf_.resize(n);
    if (buf_len_ - buf_off_ >= n) return true;
    if (buf_off_ > 0) {
      size_t rem = buf_len_ - buf_off_;
      buf_start_ += buf_off_;
      std::memmove(buf_.data(), buf_.data() + buf_off_, rem);
      buf_len_ = rem;
      buf_off_ = 0;
    }
    while (buf_len_ < n) {
      size_t want = buf_.size() - buf_len_;
      if (want > kMaxRead) want = kMaxRead;
      int got = p_pread(fd_, buf_.data() + buf_len_, want,
                        static_cast<int64_t>(buf_start_ + buf_len_));
      if (got < 0) {
        if (errno == EINTR) continue;
        io_error_ = true;
        err_no_ = errno;
        return false;
      }
      if (got == 0) return false;  // 干净的 EOF：尾部不完整
      buf_len_ += static_cast<size_t>(got);
    }
    return true;
  }

  const unsigned char* data() const { return buf_.data() + buf_off_; }
  void skip(size_t n) { buf_off_ += n; }
  uint64_t pos() const { return buf_start_ + buf_off_; }
  bool io_error() const { return io_error_; }
  int err_no() const { return err_no_; }

 private:
  static constexpr size_t kChunk = 64u * 1024u;
  static constexpr size_t kMaxRead = 1u << 20;
  int fd_;
  uint64_t buf_start_ = 0;   // buf_[0] 对应的文件偏移
  std::vector<unsigned char> buf_;
  size_t buf_off_ = 0;
  size_t buf_len_ = 0;
  bool io_error_ = false;
  int err_no_ = 0;
};

bool read_whole_file(const std::string& path, std::vector<char>& out, bool& existed,
                     std::string& err) {
  existed = false;
  out.clear();
  int fd = p_open_read(path);
  if (fd < 0) {
    if (errno == ENOENT) return true;  // 首次启动：还没有 catalog
    err = "storage: cannot open '" + path + "': " + std::strerror(errno);
    return false;
  }
  existed = true;
  int64_t size = p_lseek(fd, 0, SEEK_END);
  if (size < 0 || p_lseek(fd, 0, SEEK_SET) != 0) {
    err = "storage: seek failed on '" + path + "'";
    p_close(fd);
    return false;
  }
  out.resize(static_cast<size_t>(size));
  if (size > 0 && !read_exact(fd, out.data(), out.size())) {
    err = "storage: read failed on '" + path + "'";
    p_close(fd);
    return false;
  }
  p_close(fd);
  return true;
}

// ------------------------------------------------------------------ schema 编码
uint8_t col_type_code(ColType t) {
  switch (t) {
    case ColType::BigInt: return 0;
    case ColType::VarChar: return 1;
    case ColType::Vector: return 2;
  }
  return 0;
}

bool col_type_from(uint8_t code, ColType& t) {
  switch (code) {
    case 0: t = ColType::BigInt; return true;
    case 1: t = ColType::VarChar; return true;
    case 2: t = ColType::Vector; return true;
    default: return false;
  }
}

void encode_schema(std::vector<char>& b, const Schema& s) {
  put_u32(b, static_cast<uint32_t>(s.table.size()));
  put_bytes(b, s.table);
  put_u32(b, static_cast<uint32_t>(s.cols.size()));
  for (const Column& c : s.cols) {
    put_u32(b, static_cast<uint32_t>(c.name.size()));
    put_bytes(b, c.name);
    b.push_back(static_cast<char>(col_type_code(c.type)));
    put_u32(b, static_cast<uint32_t>(static_cast<int32_t>(c.varchar_len)) & 0xFFFFFFFFu);
    put_u32(b, static_cast<uint32_t>(static_cast<int32_t>(c.dim)) & 0xFFFFFFFFu);
    b.push_back(static_cast<char>(c.primary ? 1 : 0));
  }
  put_u32(b, static_cast<uint32_t>(static_cast<int32_t>(s.pk_col)) & 0xFFFFFFFFu);
  put_u32(b, static_cast<uint32_t>(static_cast<int32_t>(s.vec_col)) & 0xFFFFFFFFu);
}

bool decode_schema(Cur& cur, Schema& s) {
  uint32_t tlen = 0;
  if (!cur.u32(tlen)) return false;
  if (!cur.has(tlen)) return false;
  s.table.assign(reinterpret_cast<const char*>(cur.p + cur.off), tlen);
  cur.off += tlen;
  uint32_t ncols = 0;
  if (!cur.u32(ncols)) return false;
  if (static_cast<size_t>(ncols) > cur.n) return false;
  s.cols.clear();
  s.cols.reserve(ncols);
  for (uint32_t i = 0; i < ncols; ++i) {
    Column c;
    if (!cur.str(c.name)) return false;
    uint8_t code = 0;
    if (!cur.u8(code)) return false;
    if (!col_type_from(code, c.type)) return false;
    int32_t vlen = 0;
    if (!cur.i32(vlen)) return false;
    c.varchar_len = vlen;
    int32_t dim = 0;
    if (!cur.i32(dim)) return false;
    c.dim = dim;
    uint8_t prim = 0;
    if (!cur.u8(prim)) return false;
    c.primary = prim != 0;
    s.cols.push_back(std::move(c));
  }
  int32_t pk = -1;
  int32_t vc = -1;
  if (!cur.i32(pk)) return false;
  if (!cur.i32(vc)) return false;
  s.pk_col = pk;
  s.vec_col = vc;
  return true;
}

}  // namespace

// ==================================================================== Storage
bool Storage::open(const std::string& dir, std::string& err) {
  err.clear();
  close();
  if (dir.empty()) {
    err = "storage: empty data dir";
    return false;
  }
  if (!mkdir_all(dir)) {
    err = "storage: cannot create data dir '" + dir + "'";
    return false;
  }
  dir_ = dir;
  wal_path_ = dir_ + "/wal.log";
  catalog_path_ = dir_ + "/catalog.dat";

  int fd = p_open(wal_path_);
  if (fd < 0) {
    err = "storage: cannot open '" + wal_path_ + "': " + std::strerror(errno);
    return false;
  }
  int64_t size = p_lseek(fd, 0, SEEK_END);
  if (size < 0) {
    err = "storage: seek failed on '" + wal_path_ + "'";
    p_close(fd);
    return false;
  }
  bool fresh = false;
  if (static_cast<uint64_t>(size) < kWalHeader) {
    // 空文件，或建到一半的文件头：不含任何已提交数据，直接从头写
    if (!p_ftruncate(fd, 0)) {
      err = "storage: cannot reset '" + wal_path_ + "': " + std::strerror(errno);
      p_close(fd);
      return false;
    }
    std::vector<char> h = make_wal_header();
    if (!write_all(fd, h.data(), h.size()) || !p_fdatasync(fd)) {
      err = "storage: cannot init '" + wal_path_ + "': " + std::strerror(errno);
      p_close(fd);
      return false;
    }
    fresh = true;
  } else {
    if (check_wal_header(fd, err) != 0) {
      p_close(fd);
      return false;
    }
  }
  int64_t end = p_lseek(fd, 0, SEEK_END);
  if (end < 0) {
    err = "storage: seek failed on '" + wal_path_ + "'";
    p_close(fd);
    return false;
  }
  wal_fd_ = fd;
  if (fresh) p_fsync_dir(dir_);
  txn_seq_ = 0;
  bytes_written_ = 0;
  file_size_ = static_cast<uint64_t>(end);
  pending_base_ = file_size_;
  pending_.clear();
  return true;
}

void Storage::close() {
  if (wal_fd_ >= 0) {
    p_close(wal_fd_);
    wal_fd_ = -1;
  }
}

bool Storage::commit_batch(const std::vector<Mutation>& muts, std::string& err) {
  err.clear();
  if (wal_fd_ < 0) {
    err = "storage: not open";
    return false;
  }
  if (muts.empty()) return true;  // 空批次不落盘，也就不需要 COMMIT
  const uint64_t txn = begin_txn();
  if (!append(muts, txn, nullptr, err)) return false;
  return commit(txn, err);
}

// ---- 流式写入 ----
bool Storage::append(const std::vector<Mutation>& muts, uint64_t txn,
                     std::vector<RecordLoc>* locs, std::string& err) {
  err.clear();
  if (wal_fd_ < 0) {
    err = "storage: not open";
    return false;
  }
  if (locs) locs->clear();
  uint64_t pos = file_end();
  for (const Mutation& m : muts) {
    build_payload(m, payload_scratch_);
    if (payload_scratch_.size() > kMaxPayload) {
      err = "storage: mutation too large (" + std::to_string(payload_scratch_.size()) + " bytes)";
      return false;
    }
    const size_t before = pending_.size();
    append_record(pending_, txn, type_of(m.op), payload_scratch_);
    const uint32_t len = static_cast<uint32_t>(pending_.size() - before);
    if (locs) locs->push_back(RecordLoc{pos, len});
    pos += len;
  }
  return true;
}

bool Storage::write_pending(std::string& err) {
  if (pending_.empty()) {
    pending_base_ = file_size_;
    return true;
  }
  const uint64_t base = file_size_;
  const size_t n = pending_.size();
  if (!write_all_at(wal_fd_, pending_.data(), n, base)) {
    err = "storage: wal write failed: " + std::string(std::strerror(errno));
    p_ftruncate(wal_fd_, base);   // 尽力回滚半条事务
    pending_.clear();
    file_size_ = base;
    pending_base_ = base;
    return false;
  }
  file_size_ += n;
  bytes_written_ += n;
  pending_.clear();
  if (pending_.capacity() > (4u << 20)) std::vector<char>().swap(pending_);
  pending_base_ = file_size_;
  return true;
}

bool Storage::flush(std::string& err) {
  err.clear();
  if (wal_fd_ < 0) {
    err = "storage: not open";
    return false;
  }
  return write_pending(err);
}

bool Storage::commit(uint64_t txn, std::string& err) {
  err.clear();
  if (wal_fd_ < 0) {
    err = "storage: not open";
    return false;
  }
  const uint64_t base = pending_base_;
  const std::vector<char> no_payload;
  append_record(pending_, txn, kRecCommit, no_payload);
  if (!write_pending(err)) return false;
  if (!p_fdatasync(wal_fd_)) {
    err = "storage: fdatasync failed: " + std::string(std::strerror(errno));
    p_ftruncate(wal_fd_, base);
    file_size_ = base;
    pending_base_ = base;
    return false;
  }
  if (txn > txn_seq_) txn_seq_ = txn;
  return true;
}

bool Storage::rollback_to(uint64_t size, std::string& err) {
  err.clear();
  if (wal_fd_ < 0) {
    err = "storage: not open";
    return false;
  }
  if (size >= file_size_) {
    const size_t keep = static_cast<size_t>(size - file_size_);
    if (keep < pending_.size()) pending_.resize(keep);
    return true;
  }
  pending_.clear();
  if (!p_ftruncate(wal_fd_, size)) {
    err = "storage: cannot truncate wal: " + std::string(std::strerror(errno));
    return false;
  }
  file_size_ = size;
  pending_base_ = size;
  return true;
}

bool Storage::truncate(uint64_t size, std::string& err) {
  err.clear();
  if (wal_fd_ < 0) {
    err = "storage: not open";
    return false;
  }
  if (size == file_size_ && pending_.empty()) return true;
  pending_.clear();
  if (!p_ftruncate(wal_fd_, size)) {
    err = "storage: cannot truncate wal: " + std::string(std::strerror(errno));
    return false;
  }
  file_size_ = size;
  pending_base_ = size;
  return true;
}

namespace {

// 遗留 replay 用：把已提交事务整批读回内存。引擎恢复不走这里（走 replay_groups，只重建索引）。
class CollectSink : public Storage::GroupSink {
 public:
  explicit CollectSink(std::vector<Mutation>& out) : out_(out) {}
  void begin_group(uint64_t) override { tmp_.clear(); }
  void record(const Storage::View& v) override {
    Mutation m;
    m.op = op_of(v.type);
    if (!parse_payload(v.payload, v.payload_len, m)) {
      bad_ = true;
      return;
    }
    tmp_.push_back(std::move(m));
  }
  void commit_group() override {
    if (!bad_) {
      for (Mutation& m : tmp_) out_.push_back(std::move(m));
    }
    bad_ = false;
    tmp_.clear();
  }
  void abort_group() override { tmp_.clear(); }

 private:
  std::vector<Mutation>& out_;
  std::vector<Mutation> tmp_;
  bool bad_ = false;
};

}  // namespace

bool Storage::replay(std::vector<Mutation>& out, std::string& err) {
  CollectSink sink(out);
  return replay_groups(sink, nullptr, err);
}

// ---- 顺序扫描（不校验 CRC，供执行器流式扫描与写入定位使用）----
bool Storage::scan(const std::function<bool(const View&)>& sink, std::string& err) {
  err.clear();
  if (wal_fd_ < 0) {
    err = "storage: not open";
    return false;
  }
  if (file_size_ <= kWalHeader) return true;
  WalReader rd(wal_fd_, kWalHeader, kScanChunk);
  while (true) {
    if (!rd.ensure(kRecPrefix)) break;
    const uint32_t len = le_u32(rd.data());
    if (len > kMaxPayload) break;
    if (!rd.ensure(kRecPrefix + len)) break;   // 尾部半条记录：到此为止
    const unsigned char* rec = rd.data();
    const uint8_t type = rec[16];
    const uint64_t rec_off = rd.pos();
    if (type == kRecCommit) {
      rd.skip(kRecPrefix);
      continue;
    }
    if (type != kRecInsert && type != kRecUpdate && type != kRecDelete) break;
    View v;
    v.type = type;
    v.txn = le_u64(rec + 8);
    v.offset = rec_off;
    v.len = static_cast<uint32_t>(kRecPrefix + len);
    v.payload_len = len;
    v.payload = rec + kRecPrefix;
    const bool cont = sink(v);
    rd.skip(kRecPrefix + len);
    if (!cont) break;
  }
  if (rd.io_error()) {
    err = "storage: wal read failed: " + std::string(std::strerror(rd.err_no()));
    return false;
  }
  return true;
}

// ---- 校验 CRC 的事务分组回放（恢复用）----
bool Storage::replay_groups(GroupSink& sink, uint64_t* committed_end, std::string& err) {
  err.clear();
  if (committed_end) *committed_end = kWalHeader;
  if (wal_fd_ < 0) {
    err = "storage: not open";
    return false;
  }
  if (file_size_ <= kWalHeader) return true;

  WalReader rd(wal_fd_, kWalHeader, kScanChunk);
  uint64_t last_commit = kWalHeader;
  uint64_t group_txn = 0;
  uint64_t max_txn = txn_seq_;
  bool have_group = false;
  // 每个 break 都表示「从这里开始的数据不可信」，尾部整段丢弃
  while (true) {
    if (!rd.ensure(kRecPrefix)) break;
    const uint32_t len = le_u32(rd.data());
    if (len > kMaxPayload) break;
    if (!rd.ensure(kRecPrefix + len)) break;
    const unsigned char* rec = rd.data();
    const uint32_t want_crc = le_u32(rec + 4);
    const uint64_t txn = le_u64(rec + 8);
    const uint8_t type = rec[16];
    uint32_t got_crc = crc32(0, rec + 8, 9);
    got_crc = crc32(got_crc, rec + kRecPrefix, len);
    if (got_crc != want_crc) break;                     // CRC 不匹配
    const uint64_t rec_off = rd.pos();
    if (type == kRecCommit) {
      if (have_group && txn == group_txn) {
        sink.commit_group();
        last_commit = rec_off + kRecPrefix;
      } else if (have_group) {
        sink.abort_group();                             // COMMIT 不属于这一组：整组作废
      } else {
        last_commit = rec_off + kRecPrefix;              // 空事务：也是干净的边界
      }
      have_group = false;
      if (txn > max_txn) max_txn = txn;
      rd.skip(kRecPrefix);
      continue;
    }
    if (type != kRecInsert && type != kRecUpdate && type != kRecDelete) break;
    if (have_group && txn != group_txn) {
      sink.abort_group();
      have_group = false;
    }
    if (!have_group) {
      sink.begin_group(txn);
      group_txn = txn;
      have_group = true;
    }
    View v;
    v.type = type;
    v.txn = txn;
    v.offset = rec_off;
    v.len = static_cast<uint32_t>(kRecPrefix + len);
    v.payload_len = len;
    v.payload = rec + kRecPrefix;
    sink.record(v);
    rd.skip(kRecPrefix + len);
  }
  if (have_group) sink.abort_group();   // 没有 COMMIT 收尾的尾部整组丢弃
  if (rd.io_error()) {
    err = "storage: wal read failed: " + std::string(std::strerror(rd.err_no()));
    return false;
  }
  txn_seq_ = max_txn;
  if (committed_end) *committed_end = last_commit;
  return true;
}

// ---- 随机读 ----
bool Storage::read_payload(uint64_t offset, uint32_t len, std::vector<unsigned char>& out,
                           std::string& err) {
  err.clear();
  out.clear();
  if (wal_fd_ < 0) {
    err = "storage: not open";
    return false;
  }
  if (len < kRecPrefix || offset + len > file_size_) {
    err = "storage: record out of range";
    return false;
  }
  const size_t plen = len - kRecPrefix;
  out.resize(plen);
  size_t done = 0;
  while (done < plen) {
    const int r = p_pread(wal_fd_, out.data() + done, plen - done,
                          static_cast<int64_t>(offset + kRecPrefix + done));
    if (r < 0) {
      if (errno == EINTR) continue;
      err = "storage: read failed: " + std::string(std::strerror(errno));
      return false;
    }
    if (r == 0) {
      err = "storage: short read";
      return false;
    }
    done += static_cast<size_t>(r);
  }
  return true;
}

// ---- 行负载零拷贝解码 ----
bool Storage::decode_payload(const unsigned char* p, size_t n, PayloadView& out) {
  out.cols.clear();
  Cur c{p, n, 0};
  uint32_t tlen = 0;
  if (!c.u32(tlen)) return false;
  if (!c.has(tlen)) return false;
  out.table = reinterpret_cast<const char*>(p + c.off);
  out.table_len = tlen;
  c.off += tlen;
  uint32_t ncols = 0;
  if (!c.u32(ncols)) return false;
  if (static_cast<size_t>(ncols) > n) return false;
  out.cols.reserve(ncols);
  for (uint32_t i = 0; i < ncols; ++i) {
    uint8_t tag = 0;
    if (!c.u8(tag)) return false;
    ValueView v;
    v.tag = tag;
    switch (tag) {
      case kVNull:
        break;
      case kVInt:
      case kVFloat:
        if (!c.has(8)) return false;
        v.data = p + c.off;
        c.off += 8;
        break;
      case kVStr: {
        uint32_t slen = 0;
        if (!c.u32(slen)) return false;
        if (!c.has(slen)) return false;
        v.data = p + c.off;
        v.n = slen;
        c.off += slen;
        break;
      }
      case kVVec: {
        uint32_t cnt = 0;
        if (!c.u32(cnt)) return false;
        const size_t bytes = static_cast<size_t>(cnt) * 4;
        if (!c.has(bytes)) return false;
        v.data = p + c.off;
        v.n = cnt;
        c.off += bytes;
        break;
      }
      default:
        return false;
    }
    out.cols.push_back(v);
  }
  return c.off == n;
}

bool Storage::save_catalog(const std::vector<Schema>& schemas, std::string& err) {
  err.clear();
  if (dir_.empty()) {
    err = "storage: not open";
    return false;
  }
  std::vector<char> body;
  put_u32(body, static_cast<uint32_t>(schemas.size()));
  for (const Schema& s : schemas) encode_schema(body, s);

  std::vector<char> file;
  file.insert(file.end(), kCatalogMagic, kCatalogMagic + 8);
  put_u16(file, kFormatVersion);
  put_u16(file, kEndianMark);
  put_u32(file, crc32(0, body.data(), body.size()));
  put_u64(file, static_cast<uint64_t>(body.size()));
  file.insert(file.end(), body.begin(), body.end());

  const std::string tmp = catalog_path_ + ".tmp";
  int fd = p_open_trunc(tmp);
  if (fd < 0) {
    err = "storage: cannot create '" + tmp + "': " + std::strerror(errno);
    return false;
  }
  if (!write_all(fd, file.data(), file.size()) || !p_fsync(fd)) {
    int saved = errno;
    p_close(fd);
    p_unlink(tmp);
    err = "storage: cannot write '" + tmp + "': " + std::strerror(saved);
    return false;
  }
  p_close(fd);
  if (!p_rename(tmp, catalog_path_)) {
    p_unlink(tmp);
    err = "storage: cannot rename catalog into place: " + std::string(std::strerror(errno));
    return false;
  }
  p_fsync_dir(dir_);
  bytes_written_ += static_cast<uint64_t>(file.size());
  return true;
}

bool Storage::load_catalog(std::vector<Schema>& schemas, std::string& err) {
  err.clear();
  schemas.clear();
  if (dir_.empty()) {
    err = "storage: not open";
    return false;
  }
  std::vector<char> raw;
  bool existed = false;
  if (!read_whole_file(catalog_path_, raw, existed, err)) return false;
  if (!existed) return true;  // 首次启动还没有表定义
  if (raw.size() < kCatalogHeader) {
    err = "storage: catalog truncated";
    return false;
  }
  const unsigned char* p = reinterpret_cast<const unsigned char*>(raw.data());
  if (std::memcmp(raw.data(), kCatalogMagic, 8) != 0) {
    err = "storage: catalog magic mismatch";
    return false;
  }
  uint16_t ver = static_cast<uint16_t>(p[8] | (p[9] << 8));
  uint16_t endian = static_cast<uint16_t>(p[10] | (p[11] << 8));
  if (endian != kEndianMark) {
    err = "storage: catalog byte-order marker mismatch";
    return false;
  }
  if (ver != kFormatVersion) {
    err = "storage: unsupported catalog version " + std::to_string(ver);
    return false;
  }
  uint32_t want_crc = le_u32(p + 12);
  uint64_t body_len = le_u64(p + 16);
  if (body_len != static_cast<uint64_t>(raw.size()) - kCatalogHeader) {
    err = "storage: catalog length mismatch";
    return false;
  }
  const unsigned char* body = p + kCatalogHeader;
  if (crc32(0, body, static_cast<size_t>(body_len)) != want_crc) {
    err = "storage: catalog crc mismatch";
    return false;
  }
  Cur cur{body, static_cast<size_t>(body_len), 0};
  uint32_t nschemas = 0;
  if (!cur.u32(nschemas)) {
    err = "storage: catalog body malformed";
    return false;
  }
  schemas.reserve(nschemas);
  for (uint32_t i = 0; i < nschemas; ++i) {
    Schema s;
    if (!decode_schema(cur, s)) {
      schemas.clear();
      err = "storage: catalog schema malformed";
      return false;
    }
    schemas.push_back(std::move(s));
  }
  if (cur.off != cur.n) {
    schemas.clear();
    err = "storage: catalog trailing bytes";
    return false;
  }
  return true;
}
