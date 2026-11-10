#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "value.h"

// 一条逻辑变更。Update/Delete 以主键定位记录，Row 只需填充主键列与变更后的完整行。
struct Mutation {
  enum class Op : uint8_t { Insert = 1, Update = 2, Delete = 3 };
  Op op = Op::Insert;
  std::string table;
  Row row;   // Insert/Update：完整行；Delete：整行（payload 需要满足列数一致性）
};

// 一条记录在 wal.log 里的位置：offset 是记录起点（含 17 字节前缀），len 覆盖整条记录。
struct RecordLoc {
  uint64_t offset = 0;
  uint32_t len = 0;
};

// 行负载里单个列的零拷贝视图：data 指向 wal 读缓冲，只在当次回调/缓冲生命周期内有效。
struct ValueView {
  uint8_t tag = 0;                 // 0 null / 1 int / 2 float / 3 str / 4 vec
  const unsigned char* data = nullptr;
  uint32_t n = 0;                  // str: 字节数；vec: 元素个数
};

// 一条行负载的零拷贝视图：表名 + 逐列视图（cols 由调用方复用，不重新分配）。
struct PayloadView {
  const char* table = nullptr;
  uint32_t table_len = 0;
  std::vector<ValueView> cols;
};

// 落盘契约：
//   /data/catalog.dat  表定义（每次 CREATE TABLE 覆写）
//   /data/wal.log      append-only 行日志；每条记录 = [u32 len][u32 crc32][u64 txn][u8 type][payload]
//                      事务以 type=COMMIT 收尾；COMMIT 之前 fdatasync 一次。
// 行内容只落这一份：执行器把 wal.log 同时当作行存储，内存里只保留槽位/存活位/pk 索引，
// 因此数据量可以超过内存限额。崩溃恢复：从头顺序重放，遇到长度/CRC 不合法或尾部截断即停止；
// 无 COMMIT 收尾的事务整段丢弃，随后把文件截到最后一个已提交边界。
class Storage {
 public:
  // 流式扫描交给调用方的一条记录（payload 只在回调内有效）。
  struct View {
    uint8_t type = 0;        // 1 Insert / 2 Update / 3 Delete
    uint64_t txn = 0;
    uint64_t offset = 0;     // 记录起点
    uint32_t len = 0;        // 整条记录长度（含前缀）
    uint32_t payload_len = 0;
    const unsigned char* payload = nullptr;   // = 记录起点 + 17
  };

  // 恢复期的事务分组回放接口：abort_group 用于丢弃未提交的尾部（崩溃残留或回滚痕迹）。
  class GroupSink {
   public:
    virtual ~GroupSink() = default;
    virtual void begin_group(uint64_t txn) = 0;
    virtual void record(const View& v) = 0;
    virtual void commit_group() = 0;
    virtual void abort_group() = 0;
  };

  bool open(const std::string& dir, std::string& err);
  void close();

  // 表定义持久化
  bool save_catalog(const std::vector<Schema>& schemas, std::string& err);
  bool load_catalog(std::vector<Schema>& schemas, std::string& err);

  // 单次提交：整批记录 + COMMIT 一次写 + 一次 fdatasync
  bool commit_batch(const std::vector<Mutation>& muts, std::string& err);

  // 全量重放（遗留接口，供存储单测与工具使用）：把已提交事务整批读进内存
  bool replay(std::vector<Mutation>& out, std::string& err);

  // ---- 流式写入：语句级批量追加 ----
  uint64_t begin_txn() { return ++txn_seq_; }
  bool append(const std::vector<Mutation>& muts, uint64_t txn, std::vector<RecordLoc>* locs,
              std::string& err);
  bool flush(std::string& err);                      // 缓冲落地（不 fsync），使行可被扫描到
  bool commit(uint64_t txn, std::string& err);        // 追加 COMMIT + 落地 + 一次 fdatasync
  bool rollback_to(uint64_t size, std::string& err);  // 丢弃未落地的尾部（语句失败/事务回滚）
  uint64_t file_end() const { return file_size_ + pending_.size(); }
  uint64_t file_size() const { return file_size_; }

  // ---- 随机读与顺序扫描 ----
  bool read_payload(uint64_t offset, uint32_t len, std::vector<unsigned char>& out,
                    std::string& err);
  // 单遍顺序扫描（不校验 CRC）：把文件里的全部记录按顺序交给 sink，sink 返回 false 即停止。
  bool scan(const std::function<bool(const View&)>& sink, std::string& err);
  // 校验 CRC 的事务分组回放：只回放已提交事务，committed_end 为最后一个已提交边界。
  bool replay_groups(GroupSink& sink, uint64_t* committed_end, std::string& err);
  bool truncate(uint64_t size, std::string& err);

  // 行负载零拷贝解码：表名 + 逐列视图
  static bool decode_payload(const unsigned char* p, size_t n, PayloadView& out);

  // 统计（性能与自检用）
  uint64_t bytes_written() const { return bytes_written_; }

 private:
  bool write_pending(std::string& err);

  std::string dir_;
  std::string wal_path_;
  std::string catalog_path_;
  uint64_t txn_seq_ = 0;
  uint64_t bytes_written_ = 0;
  uint64_t file_size_ = 0;      // 已落地的字节数
  uint64_t pending_base_ = 0;   // 缓冲起点 == 缓冲为空时的 file_size_
  int wal_fd_ = -1;
  std::vector<char> pending_;          // 尚未落地的记录
  std::vector<char> payload_scratch_;  // 编码用复用缓冲
};
