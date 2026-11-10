// 行存储：把「行内容在文件里、内存里只有元数据」这件事封装成按行号访问的接口。
//   槽位：行号 → (记录偏移, 记录长度)，12 字节/行
//   存活位：1 位/行
//   pk 索引：开放寻址，约 19 字节/行
//   行缓存：按字节上限的 LRU，按记录偏移索引（同一行的新版本是新偏移）
// 一次全表扫描不需要任何一份 Row 常驻：调用方直接用负载视图做谓词与距离计算。
#include "rowstore.h"

#include <utility>

bool decode_cols(const std::vector<ValueView>& cols, Row& out) {
  out.clear();
  out.reserve(cols.size());
  for (const ValueView& v : cols) {
    switch (v.tag) {
      case 0:   // null
        out.push_back(Value{});
        break;
      case 1:   // int
        out.push_back(Value::Int(view_int(v)));
        break;
      case 2: {  // float（double）
        double d = 0;
        std::memcpy(&d, v.data, sizeof(d));
        out.push_back(Value::Float(d));
        break;
      }
      case 3:    // str
        out.push_back(Value::Str(std::string(reinterpret_cast<const char*>(v.data), v.n)));
        break;
      case 4: {  // vec
        std::vector<float> xs(v.n);
        if (v.n) std::memcpy(xs.data(), v.data, static_cast<size_t>(v.n) * sizeof(float));
        out.push_back(Value::Vec(std::move(xs)));
        break;
      }
      default:
        return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------- PkIndex
bool PkIndex::find(int64_t pk, uint32_t& rowid) const {
  if (count_ == 0) return false;
  size_t i = home_of(pk) & mask_;
  while (used_[i] != 0) {
    if (used_[i] == 1 && keys_[i] == pk) {
      rowid = vals_[i];
      return true;
    }
    i = (i + 1) & mask_;
  }
  return false;
}

void PkIndex::bind(int64_t pk, uint32_t rowid) {
  if (mask_ == 0 || (filled_ + 1) * 10 >= (mask_ + 1) * 7) {
    rehash(mask_ == 0 ? 64 : (mask_ + 1) * 2);
  }
  size_t i = home_of(pk) & mask_;
  while (used_[i] == 1 && keys_[i] != pk) i = (i + 1) & mask_;
  if (used_[i] == 0) ++filled_;
  else if (used_[i] == 2) { --filled_; }   // 墓碑被复用
  if (used_[i] != 1) {
    used_[i] = 1;
    keys_[i] = pk;
    ++count_;
  }
  vals_[i] = rowid;
}

bool PkIndex::unbind(int64_t pk) {
  if (count_ == 0) return false;
  size_t i = home_of(pk) & mask_;
  while (used_[i] != 0) {
    if (used_[i] == 1 && keys_[i] == pk) {
      used_[i] = 2;
      --count_;
      return true;
    }
    i = (i + 1) & mask_;
  }
  return false;
}

void PkIndex::clear() {
  std::vector<int64_t>().swap(keys_);
  std::vector<uint32_t>().swap(vals_);
  std::vector<uint8_t>().swap(used_);
  mask_ = 0;
  count_ = 0;
  filled_ = 0;
}

void PkIndex::rehash(size_t cap) {
  size_t c = 64;
  while (c < cap) c <<= 1;
  std::vector<int64_t> old_keys;
  std::vector<uint32_t> old_vals;
  std::vector<uint8_t> old_used;
  old_keys.swap(keys_);
  old_vals.swap(vals_);
  old_used.swap(used_);
  keys_.assign(c, 0);
  vals_.assign(c, 0);
  used_.assign(c, 0);
  mask_ = c - 1;
  const size_t n = old_used.size();
  for (size_t k = 0; k < n; ++k) {
    if (old_used[k] != 1) continue;
    size_t i = home_of(old_keys[k]) & mask_;
    while (used_[i] == 1) i = (i + 1) & mask_;
    used_[i] = 1;
    keys_[i] = old_keys[k];
    vals_[i] = old_vals[k];
  }
  filled_ = count_;
}

// ---------------------------------------------------------------- RowStore
void RowStore::set_cache_budget(size_t bytes) {
  cache_budget_ = bytes;
  cache_evict_until(0);
}

uint32_t RowStore::append_slot(uint64_t off, uint32_t len, bool v) {
  const uint32_t rowid = static_cast<uint32_t>(off_.size());
  off_.push_back(off);
  len_.push_back(len);
  set_alive(rowid, v);
  return rowid;
}

void RowStore::set_slot(uint32_t rowid, uint64_t off, uint32_t len, bool v) {
  off_[rowid] = off;
  len_[rowid] = len;
  set_alive(rowid, v);
}

void RowStore::truncate_slots(uint32_t n) {
  if (n >= off_.size()) return;
  off_.resize(n);
  len_.resize(n);
  const size_t words = (static_cast<size_t>(n) + 63) / 64;
  if (alive_.size() > words) alive_.resize(words);
  if (!alive_.empty() && (n & 63) != 0) {
    alive_.back() &= (1ull << (n & 63)) - 1ull;   // 清掉被截断行残留的存活位
  }
}

bool RowStore::alive(uint32_t rowid) const {
  const size_t w = rowid >> 6;
  return w < alive_.size() && ((alive_[w] >> (rowid & 63)) & 1ull) != 0;
}

void RowStore::set_alive(uint32_t rowid, bool v) {
  const size_t w = rowid >> 6;
  if (w >= alive_.size()) alive_.resize(w + 1, 0);
  const uint64_t bit = 1ull << (rowid & 63);
  if (v) alive_[w] |= bit;
  else alive_[w] &= ~bit;
}

bool RowStore::read_views(uint32_t rowid, PayloadView& out, std::string& err) {
  if (rowid >= off_.size() || !alive(rowid)) {
    err = "storage: row is not readable";
    return false;
  }
  if (!st_->read_payload(off_[rowid], len_[rowid], readbuf_, err)) return false;
  if (!Storage::decode_payload(readbuf_.data(), readbuf_.size(), out)) {
    err = "storage: row payload malformed";
    return false;
  }
  return true;
}

std::shared_ptr<const Row> RowStore::fetch(uint32_t rowid, std::string& err) {
  if (rowid >= off_.size() || !alive(rowid)) {
    err = "storage: row is not readable";
    return nullptr;
  }
  const uint64_t off = off_[rowid];
  auto it = lru_map_.find(off);
  if (it != lru_map_.end()) {
    lru_.splice(lru_.begin(), lru_, it->second);   // 命中即提升到 MRU
    return it->second->row;
  }
  PayloadView pv;
  if (!read_views(rowid, pv, err)) return nullptr;
  auto row = std::make_shared<Row>();
  if (!decode_row_values(pv, *row)) {
    err = "storage: row payload malformed";
    return nullptr;
  }
  size_t bytes = 96;   // 缓存条目自身开销
  for (const Value& v : *row) bytes += sizeof(Value) + v.s.capacity() + v.v.capacity() * 4;
  cache_put(off, row, bytes);
  return row;
}

void RowStore::forget_cache() {
  lru_.clear();
  lru_map_.clear();
  cache_used_ = 0;
}

void RowStore::cache_evict_until(size_t want) {
  while (cache_used_ > want && !lru_.empty()) {
    const CacheNode& back = lru_.back();
    cache_used_ -= back.bytes;
    lru_map_.erase(back.off);
    lru_.pop_back();
  }
}

void RowStore::cache_put(uint64_t off, const std::shared_ptr<const Row>& row, size_t bytes) {
  if (cache_budget_ == 0 || bytes > cache_budget_) return;
  if (cache_used_ + bytes > cache_budget_) cache_evict_until(cache_budget_ - bytes);
  lru_.push_front(CacheNode{off, bytes, row});
  lru_map_[off] = lru_.begin();
  cache_used_ += bytes;
}

size_t RowStore::meta_bytes() const {
  return sizeof(*this) + off_.capacity() * sizeof(uint64_t) + len_.capacity() * sizeof(uint32_t) +
         alive_.capacity() * sizeof(uint64_t) + pk_index_.bytes();
}

// ---------------------------------------------------------------- 向量行镜像 arena
// 记录布局（记录的起始偏移恒为 32B 对齐，故向量浮点也是 32B 对齐，可直接喂给 vec::l2_distance）：
//   [0, dim*4)      向量列的 float（4 字节小端，与负载里的编码一致）
//   [tail_off, len) u32 ncols，随后逐列 [u8 tag][data]：
//                     tag 0 null：无 data
//                     tag 1 int / tag 2 float：8 字节
//                     tag 3 str：u32 长度 + 字节
//                     tag 4 vec：无 data（浮点在记录头部，解码时指回去）
// 追加顺序严格等于 WAL 记录顺序，因此遍历 arena 与流式扫描的访问顺序一致（含 UPDATE 后行位置）。
RowStore::ArenaSlab::~ArenaSlab() {
  if (p) ::operator delete[](p, std::align_val_t(32));
}

void RowStore::configure_arena(int ncols, int vec_col, int dim, size_t budget) {
  arena_disable();
  arena_ncols_ = ncols;
  arena_vec_col_ = vec_col;
  arena_dim_ = dim;
  arena_head_ = static_cast<size_t>(dim > 0 ? dim : 0) * sizeof(float);
  arena_tail_off_ = (arena_head_ + 31) & ~size_t(31);
  arena_budget_ = budget;
  // 没有向量列 / 无预算 / 单行尾部就撑满 slab：不启用（永远走流式路径）。
  if (vec_col < 0 || dim <= 0 || ncols <= 0 || budget == 0) return;
  if (arena_tail_off_ + 32 > kArenaChunk) return;
  arena_state_ = ArenaState::Dirty;
}

void RowStore::arena_free() {
  slabs_.clear();
  arena_bytes_ = 0;
  arena_fill_ = 0;
  arena_off_.clear();
  arena_len_.clear();
  arena_log_.clear();
  arena_building_ = false;
}

void RowStore::arena_disable() {
  arena_free();
  arena_state_ = ArenaState::Off;
}

void RowStore::arena_invalidate() {
  if (arena_state_ == ArenaState::Ready) arena_state_ = ArenaState::Dirty;
}

void RowStore::arena_begin_build() {
  if (arena_state_ == ArenaState::Off) return;
  arena_free();
  arena_state_ = ArenaState::Dirty;
  arena_building_ = true;
}

void RowStore::arena_finish_build() {
  arena_building_ = false;
  if (arena_state_ == ArenaState::Dirty) arena_state_ = ArenaState::Ready;
}

size_t RowStore::arena_index_bytes() const {
  return arena_off_.capacity() * sizeof(uint64_t) + arena_len_.capacity() * sizeof(uint32_t) +
         arena_log_.capacity() * sizeof(ArenaLogEntry) +
         slabs_.capacity() * sizeof(std::unique_ptr<ArenaSlab>);
}

bool RowStore::arena_may_build() const {
  if (arena_state_ != ArenaState::Dirty) return false;
  // 估计：行负载（WAL 记录长度已覆盖向量与标量列）+ 对齐余量 + 每行索引。
  size_t est = 0;
  const size_t n = off_.size();
  for (size_t i = 0; i < n; ++i) {
    est += static_cast<size_t>(len_[i]) + 40;
    if (est > arena_budget_) return false;
  }
  est += n * (sizeof(uint64_t) + sizeof(uint32_t) + sizeof(ArenaLogEntry));
  return est + (kArenaChunk >> 8) <= arena_budget_;
}

bool RowStore::arena_reserve(size_t len, uint64_t& off) {
  if (len == 0 || len > kArenaChunk || arena_budget_ == 0) return false;
  size_t pos = (arena_fill_ + 31) & ~size_t(31);
  const size_t in_chunk = pos & (kArenaChunk - 1);
  if (in_chunk + len > kArenaChunk) pos += kArenaChunk - in_chunk;   // 记录不跨 slab
  const size_t want_slabs = (pos >> kArenaChunkShift) + 1;
  while (slabs_.size() < want_slabs) {
    if (arena_bytes_ + kArenaChunk + arena_index_bytes() > arena_budget_) return false;
    auto slab = std::make_unique<ArenaSlab>();
    try {
      slab->p = static_cast<unsigned char*>(::operator new[](kArenaChunk, std::align_val_t(32)));
    } catch (...) {
      return false;
    }
    slabs_.push_back(std::move(slab));
    arena_bytes_ += kArenaChunk;
  }
  off = pos;
  arena_fill_ = pos + len;
  return true;
}

bool RowStore::arena_store_refs(uint32_t rowid, const std::vector<ArenaColRef>& refs) {
  if (arena_state_ == ArenaState::Off) return false;
  if (arena_state_ != ArenaState::Ready && !arena_building_) return true;  // 待重建：留给扫描
  if (static_cast<int>(refs.size()) != arena_ncols_) return true;
  const int ncols = arena_ncols_;
  if (arena_vec_col_ >= ncols) return true;
  const ArenaColRef& vec = refs[static_cast<size_t>(arena_vec_col_)];
  if (vec.tag != 4 || vec.n != static_cast<uint32_t>(arena_dim_)) return true;  // 形状不符：不镜像

  size_t tail = 4;
  for (int i = 0; i < ncols; ++i) {
    const ArenaColRef& c = refs[static_cast<size_t>(i)];
    tail += 1;
    if (c.tag == 1 || c.tag == 2) tail += 8;
    else if (c.tag == 3) tail += 4 + static_cast<size_t>(c.n);
    else if (c.tag != 0 && c.tag != 4) return true;
  }
  const size_t len = arena_tail_off_ + tail;
  if (len > kArenaChunk) return true;             // 单行超过 slab：不镜像

  // 索引会按行号/记录数增长，一起计入预算
  const size_t idx_after = static_cast<size_t>(rowid + 1) * (sizeof(uint64_t) + sizeof(uint32_t)) +
                           (arena_log_.size() + 1) * sizeof(ArenaLogEntry) +
                           (slabs_.size() + 1) * sizeof(std::unique_ptr<ArenaSlab>);
  if (arena_bytes_ + len + idx_after > arena_budget_) {
    arena_disable();
    return false;
  }
  uint64_t off = 0;
  if (!arena_reserve(len, off)) {
    arena_disable();
    return false;
  }
  if (arena_off_.size() <= rowid) {
    arena_off_.resize(static_cast<size_t>(rowid) + 1, kNoRec);
    arena_len_.resize(static_cast<size_t>(rowid) + 1, 0);
  }
  unsigned char* rec = arena_base(off);
  if (vec.n) std::memcpy(rec, vec.data, static_cast<size_t>(vec.n) * sizeof(float));
  unsigned char* p = rec + arena_tail_off_;
  const uint32_t nc = static_cast<uint32_t>(ncols);
  std::memcpy(p, &nc, sizeof(nc));
  p += sizeof(nc);
  for (int i = 0; i < ncols; ++i) {
    const ArenaColRef& c = refs[static_cast<size_t>(i)];
    *p++ = c.tag;
    if (c.tag == 1 || c.tag == 2) {
      if (c.data) std::memcpy(p, c.data, 8);
      p += 8;
    } else if (c.tag == 3) {
      const uint32_t sl = c.n;
      std::memcpy(p, &sl, sizeof(sl));
      p += sizeof(sl);
      if (sl && c.data) std::memcpy(p, c.data, sl);
      p += sl;
    }
  }
  arena_off_[rowid] = off;
  arena_len_[rowid] = static_cast<uint32_t>(len);
  arena_log_.push_back(ArenaLogEntry{off, rowid, static_cast<uint32_t>(len)});
  return true;
}

bool RowStore::arena_put(uint32_t rowid, const Row& row) {
  if (arena_state_ == ArenaState::Off) return false;
  if (arena_state_ != ArenaState::Ready && !arena_building_) return true;
  if (static_cast<int>(row.size()) != arena_ncols_) return true;
  refs_scratch_.clear();
  refs_scratch_.reserve(row.size());
  for (const Value& v : row) {
    ArenaColRef c;
    switch (v.k) {
      case Value::K::Int: c.tag = 1; c.data = &v.i; c.n = 8; break;
      case Value::K::Float: c.tag = 2; c.data = &v.d; c.n = 8; break;
      case Value::K::Str: c.tag = 3; c.data = v.s.data(); c.n = static_cast<uint32_t>(v.s.size()); break;
      case Value::K::Vec: c.tag = 4; c.data = v.v.data(); c.n = static_cast<uint32_t>(v.v.size()); break;
      default: break;
    }
    refs_scratch_.push_back(c);
  }
  return arena_store_refs(rowid, refs_scratch_);
}

bool RowStore::arena_put_views(uint32_t rowid, const std::vector<ValueView>& cols) {
  if (arena_state_ == ArenaState::Off) return false;
  if (static_cast<int>(cols.size()) != arena_ncols_) return true;
  refs_scratch_.clear();
  refs_scratch_.reserve(cols.size());
  for (const ValueView& v : cols) {
    ArenaColRef c;
    c.tag = v.tag;
    c.data = v.data;
    c.n = v.tag == 3 || v.tag == 4 ? v.n : static_cast<uint32_t>(v.tag == 0 ? 0 : 8);
    refs_scratch_.push_back(c);
  }
  return arena_store_refs(rowid, refs_scratch_);
}

bool RowStore::arena_decode(uint64_t off, uint32_t len, std::vector<ValueView>& out) const {
  if (off == kNoRec || arena_tail_off_ + 4 > len) return false;
  if ((off >> kArenaChunkShift) >= slabs_.size()) return false;
  const unsigned char* base = arena_base(off);
  const unsigned char* p = base + arena_tail_off_;
  const unsigned char* end = base + len;
  uint32_t nc = 0;
  std::memcpy(&nc, p, sizeof(nc));
  p += sizeof(nc);
  if (nc != static_cast<uint32_t>(arena_ncols_)) return false;
  out.clear();
  out.reserve(nc);
  for (uint32_t i = 0; i < nc; ++i) {
    if (p >= end) return false;
    ValueView v;
    v.tag = *p++;
    if (v.tag == 1 || v.tag == 2) {
      if (p + 8 > end) return false;
      v.data = p;
      p += 8;
    } else if (v.tag == 3) {
      if (p + 4 > end) return false;
      uint32_t sl = 0;
      std::memcpy(&sl, p, sizeof(sl));
      p += sizeof(sl);
      if (p + sl > end) return false;
      v.data = p;
      v.n = sl;
      p += sl;
    } else if (v.tag == 4) {
      if (static_cast<int>(i) != arena_vec_col_) return false;
      v.data = base;
      v.n = static_cast<uint32_t>(arena_dim_);
    } else if (v.tag != 0) {
      return false;
    }
    out.push_back(v);
  }
  return true;
}

bool RowStore::row_views(uint32_t rowid, PayloadView& out, std::string& err) {
  if (arena_state_ == ArenaState::Ready && rowid < arena_off_.size() && rowid < off_.size() &&
      alive(rowid) && arena_off_[rowid] != kNoRec &&
      arena_decode(arena_off_[rowid], arena_len_[rowid], out.cols)) {
    out.table = nullptr;
    out.table_len = 0;
    return true;
  }
  return read_views(rowid, out, err);
}
