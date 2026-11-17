#pragma once
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "parser.h"
#include "rowstore.h"
#include "storage.h"
#include "value.h"

// 执行器：单客户端、至多一个显式事务。语句级原子性与持久化边界都由执行器保证。
//   行内容只落 wal.log（append-only 行日志）；内存里每行只留槽位/存活位/pk 索引 + 有上限的行缓存，
//   所以表可以远大于内存限额。
//   全表扫描是流式的：逐条从日志解码负载视图，谓词命中才算距离、才可能被物化；
//   TopK 用 vec::topk_select 做有界剪枝（不全排序、不为每行构造 Row）。
//   显式事务不再深拷贝整库：BEGIN 只记日志长度标记，变更的前像（元数据级）进 undo 日志，
//   ROLLBACK 逆序回滚并把日志截回事务起点 —— 事务内存开销与改动量成正比。
//   自动提交语句：整批记录 + COMMIT 一次落地、一次 fdatasync，成功后才改内存；
//   显式事务：每条语句追加记录并 flush（内存立即生效、日志尚未提交），COMMIT 追加 COMMIT 记录再 fdatasync。
//   崩溃恢复：未 COMMIT 的事务在日志里没有 COMMIT 记录，回放时整段丢弃并把文件截回最后一个提交边界；
//             恢复只重建 pk→槽位 索引与存活位，不把整表读进内存。
class Engine {
 public:
  // 打开数据目录：载入 catalog → 重放已提交 WAL（只重建索引）→ 返回 true 之后才允许输出 ready。
  bool open(const std::string& data_dir, std::string& err);
  void close();

  Result execute(const std::string& sql, const std::string& mode);
  void rollback_on_shutdown();

 private:
  struct Table {
    Schema schema;
    RowStore store;
  };

  // 变更前像：只记恢复索引所必需的元数据。行内容不拷贝 —— 旧版本仍在日志里，偏移没变。
  struct UndoEntry {
    enum class K : uint8_t { Insert = 1, Update = 2, Delete = 3 };
    Table* t = nullptr;
    uint64_t prev_off = 0;
    int64_t pk = 0;
    uint32_t rowid = 0;
    uint32_t prev_len = 0;
    uint8_t k = 0;
    uint8_t prev_alive = 1;
  };

  // 一条语句的落地边界：失败时按它整体回退（日志截回 + undo 逆序）。
  struct StmtScope {
    uint64_t file = 0;
    size_t undo = 0;
    uint64_t txn = 0;
    bool touched = false;
  };

  // 恢复期的事务分组回放：只重建 pk→槽位 与存活位
  class RecoverySink;

  Result exec_create_table(const Statement& st);
  Result exec_create_index(const Statement& st);
  Result exec_insert(const Statement& st);
  Result exec_select(const Statement& st);
  Result exec_update(const Statement& st);
  Result exec_delete(const Statement& st);

  Table* table(const std::string& name);

  // 语句落地协议：begin → append_apply* → end_stmt（失败走 abort_stmt）
  StmtScope begin_stmt();
  Result append_apply(Table& t, std::vector<Mutation>& muts, StmtScope& sc);
  Result end_stmt(StmtScope& sc, int64_t affected);
  Result abort_stmt(StmtScope& sc, const Result& r);

  void apply_insert(Table& t, int64_t pk, const RecordLoc& loc, const Row* row);
  void apply_update(Table& t, int64_t pk, const RecordLoc& loc, const Row* row);
  void apply_delete(Table& t, int64_t pk);
  void undo_to(size_t mark);

  // 流式扫描某表当前的活行版本：fn(rowid, pk, 列视图) 返回 false 即停止。
  template <typename Fn>
  bool scan_table(Table& t, Fn&& fn, std::string& err);

  // 向量行镜像 arena：脏标记下按预算一次性重建（顺序扫一遍日志）。
  // 估计/实际超预算则永久关闭，调用方照旧走 scan_table 的流式路径。
  bool arena_refresh(Table& t, std::string& err);

  // 谓词命中的行：fn(rowid, pk, 列视图) 返回 false 即停止。主键等值时走索引点查。
  template <typename Fn>
  bool for_each_hit(Table& t, const std::vector<std::pair<int, Value>>& bound,
                    const std::vector<Predicate>& where, Fn&& fn, std::string& err);

  bool persist_catalog(std::string& err);
  bool rollback_transaction(std::string& err);

  std::map<std::string, Table> tables_;
  std::vector<UndoEntry> undo_;
  std::vector<const std::string*> created_in_txn_;   // 事务内新建的表（ROLLBACK 撤掉）
  Storage storage_;
  bool storage_open_ = false;
  bool catalog_dirty_ = false;
  bool in_txn_ = false;
  bool txn_dirty_ = false;
  uint64_t txn_id_ = 0;
  uint64_t txn_file_start_ = 0;
  size_t cache_budget_ = 64u << 20;
  size_t vec_arena_budget_ = size_t{64} << 20;
  std::set<std::string> indexed_tables_;
};
