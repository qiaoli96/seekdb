# MiniVectorDB

OceanBase seekdb 赛题一「完成一个轻量向量数据库」的实现。
单机、单进程、单客户端、最多一个显式事务；通过 stdin/stdout 的 JSONL 协议与判题器通信。

## 构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

## 运行

```bash
./build/minivectordb --data-dir /data
```

启动后先输出一行 `{"type":"ready"}`，随后逐行读取请求、逐行返回响应；日志一律写 stderr。

## 协议（摘要）

| 方向 | 报文 |
| --- | --- |
| 启动 | `{"type":"ready"}` |
| 请求 | `{"id":1,"op":"execute","sql":"...","search_mode":"exact"}` |
| 查询成功 | `{"id":1,"ok":true,"columns":["id"],"rows":[[1]]}` |
| 修改成功 | `{"id":2,"ok":true,"affected_rows":1}` |
| 执行失败 | `{"id":3,"ok":false,"error":{"code":"duplicate_key","message":"..."}}` |
| 关闭 | `{"id":4,"op":"shutdown"}` → 回滚未提交事务后以退出码 0 结束 |

## 数据目录

所有持久化文件位于 `--data-dir`（默认 `/data`）：`catalog.dat`（表定义）、`wal.log`（追加日志，事务提交处一次 fdatasync）。

## SQL 支持范围

建表（BIGINT 主键 / VARCHAR(n) / VECTOR(n)）、INSERT（含多行）、SELECT（投影、`*`、`L2_DISTANCE`、`=`/`<`/`<=`/`>`/`>=`/`<>`、AND、ORDER BY、LIMIT）、UPDATE、DELETE、BEGIN/COMMIT/ROLLBACK、CREATE VECTOR INDEX。
不支持 NULL、JOIN、聚合、子查询、ALTER TABLE、外键、嵌套事务、主键更新——遇到必须返回错误，不静默执行。

## 本地自测

```bash
# 官方驱动，本地二进制模式（无需 Docker）
bash tools/run_public.sh

# Docker 模式
docker build -t minivectordb:latest .
python3 ref/protocol_check/run.py --image minivectordb:latest --output /tmp/pr
```


## 状态与实测（2026-11-10）

| 项 | 结果 |
| --- | --- |
| 公开自测用例 | 20/20（本地二进制模式与 Docker 镜像模式一致） |
| 判题同款约束 | `--read-only --network=none --cpus=2 --memory=2g --pids-limit=128` 下握手与 CRUD 全通过 |
| 崩溃恢复 | COMMIT 后 kill -9 数据保留；未提交 kill -9 整段丢弃 |
| 内存可扩展 | 1.5M 行 / 843 MB 数据，峰值 RSS 85.4 MiB；事务内存增幅 0 |
| 性能（10,000×128 / 100 批 / 1,000 次 TopK） | 写入 0.415 s（4.15 ms/批）、查询 0.389 ms/次 |

内存与性能之间的开关：`MVDB_VEC_ARENA_MB`（默认 64，0 表示强制流式）、`MVDB_ROW_CACHE_MB`、`MVDB_MEM_LIMIT_MB`。
