#pragma once
#include <cstdint>
#include <cstring>
#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "storage.h"
#include "value.h"

// 行负载视图里的整数取值（文件格式小端；主机亦为小端，与 storage.cpp 的解码一致）。
inline int64_t view_int(const ValueView& v) {
  int64_t x = 0;
  std::memcpy(&x, v.data, sizeof(x));
  return x;
}

// 把行负载的列视图物化成完整 Row（需要拥有数据时才用；扫描路径不物化）。
bool decode_cols(const std::vector<ValueView>& cols, Row& out);
inline bool decode_row_values(const PayloadView& pv, Row& out) { return decode_cols(pv.cols, out); }

// pk → 行号 的开放寻址哈希表：每行约 19 字节，约为 std::unordered_map 的 1/3。
class PkIndex {
 public:
  bool find(int64_t pk, uint32_t& rowid) const;
  void bind(int64_t pk, uint32_t rowid);
  bool unbind(int64_t pk);
  void clear();
  size_t size() const { return count_; }
  size_t bytes() const {
    return keys_.capacity() * sizeof(int64_t) + vals_.capacity() * sizeof(uint32_t) +
           used_.capacity() + sizeof(*this);
  }

 private:
  static size_t home_of(int64_t pk) {
    return static_cast<size_t>(static_cast<uint64_t>(pk) * 0x9E3779B97F4A7C15ull);
  }
  void rehash(size_t cap);
  std::vector<int64_t> keys_;
  std::vector<uint32_t> vals_;
  std::vector<uint8_t> used_;   // 0 空 / 1 占用 / 2 墓碑
  size_t mask_ = 0;
  size_t count_ = 0;            // 活元素
  size_t filled_ = 0;           // 活元素 + 墓碑
};

// 单表行存储：内存里只有槽位（文件偏移/长度）、存活位、pk 索引和一个有上限的行缓存；
// 行内容始终在 wal.log 里，读取时才 pread + 反序列化。因此表大小可以远超内存限额。
class RowStore {
 public:
  void init(Storage* st) { st_ = st; }
  void set_cache_budget(size_t bytes);

  uint32_t append_slot(uint64_t off, uint32_t len, bool alive);
  void set_slot(uint32_t rowid, uint64_t off, uint32_t len, bool alive);
  void truncate_slots(uint32_t n);
  uint32_t slot_count() const { return static_cast<uint32_t>(off_.size()); }
  uint64_t offset(uint32_t rowid) const { return off_[rowid]; }
  uint32_t length(uint32_t rowid) const { return len_[rowid]; }
  bool alive(uint32_t rowid) const;
  void set_alive(uint32_t rowid, bool v);

  bool find(int64_t pk, uint32_t& rowid) const { return pk_index_.find(pk, rowid); }
  void bind(int64_t pk, uint32_t rowid) { pk_index_.bind(pk, rowid); }
  bool unbind(int64_t pk) { return pk_index_.unbind(pk); }
  size_t live_count() const { return pk_index_.size(); }

  // 取行内容：命中缓存直接返回，否则 pread + 反序列化后放进缓存。
  std::shared_ptr<const Row> fetch(uint32_t rowid, std::string& err);
  // 只取该行当前版本的负载字节与列视图；视图指向内部复用缓冲，下次调用即失效。
  bool read_views(uint32_t rowid, PayloadView& out, std::string& err);
  // 取行视图：arena 就绪时直接返回 arena 里的零拷贝视图（指针指向 arena，跨调用有效），
  // 否则退回 read_views（预读缓冲，下次调用即失效）。
  bool row_views(uint32_t rowid, PayloadView& out, std::string& err);

  // 文件被截断（回滚）后偏移会被复用：必须清空按偏移索引的缓存。
  void forget_cache();

  size_t meta_bytes() const;
  size_t cache_bytes() const { return cache_used_; }
  size_t cache_budget() const { return cache_budget_; }

  // ---- 向量行镜像 arena ----
  // 预算内时把整行放进一块 32B 对齐的连续 slab 区：向量浮点在每条记录起点（32B 对齐），
  // 标量列紧随其后。查询就在 arena 上算距离，不再逐查询顺序解析行日志。
  // 放不下（估算超预算、或构建/追加中真的超了）就整体释放并永久退回 WAL 流式路径 ——
  // 大表不会因此多占一分内存。索引（偏移/长度/记录序）同样计入预算。
  void configure_arena(int ncols, int vec_col, int dim, size_t budget);
  bool arena_ready() const { return arena_state_ == ArenaState::Ready; }
  bool arena_disabled() const { return arena_state_ == ArenaState::Off; }
  size_t arena_budget() const { return arena_budget_; }
  size_t arena_bytes() const { return arena_bytes_ + arena_index_bytes(); }
  // 预算估算：只有整表行负载 + 索引确实放得下才允许构建（大表连尝试都不会尝试）。
  bool arena_may_build() const;
  void arena_begin_build();
  void arena_finish_build();
  void arena_disable();
  void arena_invalidate();
  // INSERT/UPDATE 落到内存态之后登记；arena 未就绪时是空操作（下次构建会覆盖）。
  bool arena_put(uint32_t rowid, const Row& row);
  bool arena_put_views(uint32_t rowid, const std::vector<ValueView>& cols);

  // 按记录顺序遍历 arena：与 WAL 流式扫描（scan_table）的访问顺序完全一致。
  // 返回到达末尾为 true；fn 返回 false 提前停止，仍算已处理；返回 false 表示 arena 不可用。
  template <typename Fn>
  bool arena_scan(Fn&& fn) {
    if (arena_state_ != ArenaState::Ready) return false;
    const size_t n = arena_log_.size();   // 遍历期间追加的新版本不属于本次扫描
    for (size_t i = 0; i < n; ++i) {
      const ArenaLogEntry e = arena_log_[i];
      if (e.rowid >= arena_off_.size() || arena_off_[e.rowid] != e.off) continue;  // 旧版本
      if (!alive(e.rowid)) continue;
      if (!arena_decode(e.off, e.len, view_scratch_)) return false;
      if (!fn(e.rowid, view_scratch_)) return true;
    }
    return true;
  }

 private:
  struct CacheNode {
    uint64_t off = 0;
    size_t bytes = 0;
    std::shared_ptr<const Row> row;
  };
  void cache_put(uint64_t off, const std::shared_ptr<const Row>& row, size_t bytes);
  void cache_evict_until(size_t want);

  // ---- arena 内部结构 ----
  enum class ArenaState : uint8_t { Off = 0, Dirty = 1, Ready = 2 };
  struct ArenaSlab {   // 32B 对齐的 1 MiB slab，跨行连续，记录不跨 slab
    unsigned char* p = nullptr;
    ArenaSlab() = default;
    ArenaSlab(const ArenaSlab&) = delete;
    ArenaSlab& operator=(const ArenaSlab&) = delete;
    ~ArenaSlab();
  };
  struct ArenaLogEntry {   // 追加顺序 == WAL 记录顺序
    uint64_t off = 0;
    uint32_t rowid = 0;
    uint32_t len = 0;
  };
  struct ArenaColRef {     // 编码输入：一列的零拷贝描述（与负载 tag 一致）
    uint8_t tag = 0;
    const void* data = nullptr;
    uint32_t n = 0;
  };

  static constexpr size_t kArenaChunkShift = 20;               // 1 MiB / slab
  static constexpr size_t kArenaChunk = size_t{1} << kArenaChunkShift;
  static constexpr uint64_t kNoRec = ~static_cast<uint64_t>(0);
  unsigned char* arena_base(uint64_t off) const {
    return slabs_[static_cast<size_t>(off >> kArenaChunkShift)]->p +
           static_cast<size_t>(off & (kArenaChunk - 1));
  }
  bool arena_reserve(size_t len, uint64_t& off);
  bool arena_store_refs(uint32_t rowid, const std::vector<ArenaColRef>& refs);
  bool arena_decode(uint64_t off, uint32_t len, std::vector<ValueView>& out) const;
  size_t arena_index_bytes() const;
  void arena_free();

  Storage* st_ = nullptr;
  std::vector<uint64_t> off_;            // 行号 → 记录偏移
  std::vector<uint32_t> len_;            // 行号 → 记录长度
  std::vector<uint64_t> alive_;          // 存活位图，1 位/行
  PkIndex pk_index_;
  std::list<CacheNode> lru_;             // front = 最近使用
  std::unordered_map<uint64_t, std::list<CacheNode>::iterator> lru_map_;
  size_t cache_budget_ = 64u << 20;      // 默认 64 MiB
  size_t cache_used_ = 0;
  std::vector<unsigned char> readbuf_;

  ArenaState arena_state_ = ArenaState::Off;
  bool arena_building_ = false;
  int arena_ncols_ = 0;
  int arena_vec_col_ = -1;
  int arena_dim_ = 0;
  size_t arena_budget_ = 0;
  size_t arena_head_ = 0;        // dim*4：向量浮点区
  size_t arena_tail_off_ = 0;    // 尾部起点（32B 对齐）
  size_t arena_bytes_ = 0;       // 已提交的 slab 字节
  size_t arena_fill_ = 0;        // 下一字节位置（全局，跨 slab）
  std::vector<std::unique_ptr<ArenaSlab>> slabs_;
  std::vector<uint64_t> arena_off_;             // 行号 → arena 记录偏移
  std::vector<uint32_t> arena_len_;             // 行号 → arena 记录长度
  std::vector<ArenaLogEntry> arena_log_;        // 追加顺序（= WAL 顺序）
  std::vector<ValueView> view_scratch_;         // 解码复用
  std::vector<ArenaColRef> refs_scratch_;       // 编码复用
};
