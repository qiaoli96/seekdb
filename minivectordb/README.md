# MiniVectorDB（赛题一）

单机、单进程、单连接的轻量向量数据库，通过 stdin/stdout 的 JSONL 协议与评测驱动交互。
实现语言为 C++17，无第三方运行库依赖（仅 libc/libm），存储、SQL 执行、事务与恢复均为自研。

## 1. 构建与运行

### 1.1 本地构建

```bash
cd minivectordb
make            # 生成 bin/mvd，默认 -O2
make CXXFLAGS="-O3 -std=c++17 -Wall -Wextra"
```

要求：Linux x86_64、g++ >= 9（使用 C++17 与 GCC 的 AVX2/FMA target 属性）。

### 1.2 镜像构建（仓库根目录）

```bash
docker build -t minivectordb .
```

多阶段构建：`ubuntu:24.04` 构建阶段安装 `g++ make` 并从源码编译，运行阶段仅保留
`/usr/local/bin/mvd`，数据目录为 `/data`，入口即前台运行：

```bash
docker run --rm -i --network=none --cpus=2 --memory=2g \
  -v /tmp/mvd-data:/data minivectordb
```

程序不监听端口；日志写入 stderr，stdout 只输出协议响应。
数据目录按 `--data-dir=<dir>` / `--data-dir <dir>` / 位置参数的顺序优先，其次环境变量
`VECTORDB_DATA_DIR`，最后默认 `/data`。每个候选目录都会先尝试创建，不可写（例如容器
根文件系统只读、环境变量指向宿主路径）时自动跳到下一个候选，因此判题容器以
`--read-only` 运行、仅在 `/data` 挂载可写卷时也能正常启动。

### 1.3 协议示例

进程就绪后先输出一行（仅此字段）：

```json
{"type":"ready"}
```

之后每行一个请求，`execute` 回显请求中的 `id`（保持 JSON 类型）：

```json
{"id":1,"op":"execute","sql":"CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(32), embedding VECTOR(4));"}
{"id":1,"ok":true}
{"id":2,"op":"execute","sql":"INSERT INTO docs VALUES (1,'database','[1,0,0,0]'),(2,'database','[0,1,0,0]');"}
{"id":2,"ok":true,"affected_rows":2}
{"id":3,"op":"execute","sql":"SELECT id, L2_DISTANCE(embedding,'[0,1,0,0]') AS distance FROM docs ORDER BY distance LIMIT 2;"}
{"id":3,"ok":true,"columns":["id","distance"],"rows":[[2,0],[1,1.4142135623730951]]}
{"id":4,"op":"execute","sql":"BEGIN;"}
{"id":4,"ok":true}
{"id":5,"op":"execute","sql":"DELETE FROM docs WHERE id = 2;"}
{"id":5,"ok":true,"affected_rows":1}
{"id":6,"op":"execute","sql":"ROLLBACK;"}
{"id":6,"ok":true}
{"id":7,"op":"shutdown"}
{"id":7,"ok":true}
```

`shutdown` 会回滚未提交事务、落盘并退出（stdout 不再输出）。
`--restart` 场景等同正常关闭后以同一 `/data` 重新启动；数据在重启后保持一致。

### 1.4 从空目录手动验证

```bash
DIR=$(mktemp -d)
printf '%s\n' \
 '{"id":1,"op":"execute","sql":"CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(32), content VARCHAR(128), embedding VECTOR(4));"}' \
 '{"id":2,"op":"execute","sql":"INSERT INTO docs VALUES (1,'"'"'db'"'"','"'"'事务'"'"','"'"'[1,0,0,0]'"'"'),(2,'"'"'db'"'"','"'"'索引'"'"','"'"'[0,1,0,0]'"'"');"}' \
 '{"id":3,"op":"execute","sql":"SELECT * FROM docs ORDER BY id;"}' \
 '{"id":4,"op":"execute","sql":"SELECT id, L2_DISTANCE(embedding,'"'"'[0,1,0,0]'"'"') AS d FROM docs WHERE category='"'"'db'"'"' ORDER BY d LIMIT 10;"}' \
 '{"id":5,"op":"execute","sql":"BEGIN;"}' \
 '{"id":6,"op":"execute","sql":"UPDATE docs SET content='"'"'向量索引'"'"' WHERE id=2;"}' \
 '{"id":7,"op":"execute","sql":"COMMIT;"}' \
 '{"id":8,"op":"execute","sql":"BEGIN;"}' \
 '{"id":9,"op":"execute","sql":"DELETE FROM docs WHERE id=1;"}' \
 '{"id":10,"op":"execute","sql":"ROLLBACK;"}' \
 '{"id":11,"op":"shutdown"}' \
 | ./bin/mvd "$DIR"
# 重启后再次查询：已提交的修改仍在，已回滚的删除不生效
printf '%s\n' \
 '{"id":1,"op":"execute","sql":"SELECT id, content FROM docs ORDER BY id;"}' \
 '{"id":2,"op":"shutdown"}' \
 | ./bin/mvd "$DIR"
```

### 1.5 Agent prompt 材料位置

评测默认读取 `agent/conversation.json`；仓库根目录另存一份同内容副本 `conversation.json`，
以防材料路径配置调整。两份文件与源码、Dockerfile 由同一 commit 提交。

## 2. SQL 支持范围

- `CREATE TABLE t (col BIGINT [PRIMARY KEY], col2 VARCHAR(n), col3 VECTOR(n))`，
  主键可写在列上或表级 `PRIMARY KEY (col)`；每表恰好一个 BIGINT 主键、至多一个向量列，
  列数 <= 64，VARCHAR 长度按 Unicode 码点计。
- `INSERT INTO t [(col, ...)] VALUES (...), (...)`：多行插入；带列名列表时未列出的列取
  0 / 空串 / 零向量，列数不匹配、列重复或列不存在均报错；主键重复报错。
- `SELECT * | col [AS alias] | L2_DISTANCE(vec_col, '[..]') [AS alias]`
  `FROM t [WHERE col = <lit> AND col < <lit> ...]`
  `[ORDER BY col | 别名 | L2_DISTANCE(...)] [ASC|DESC] [LIMIT n]`。
  比较运算符 `= < <= > >=`；字符串比较按 UTF-8 字节序（等价 Unicode 码点序）。
- `UPDATE t SET col = <lit>[, ...] [WHERE ...]`（不允许更新主键）。
- `DELETE FROM t [WHERE ...]`。
- `CREATE VECTOR INDEX name ON t (vec_col)`：记录索引元数据。
- `BEGIN` / `START TRANSACTION` / `COMMIT` / `ROLLBACK`（不支持嵌套事务）。

不支持的语句（JOIN、聚合、子查询、ALTER、多语句等）返回错误，不会静默执行；
`CREATE INDEX`（非向量）等未定义语法同样报错。

**检索语义**：精确 TopK。距离为存储后 float32 向量上的欧氏距离，输出允许浮点误差；
并列距离按主键升序；带 WHERE 时只在过滤后的集合内取 TopK；无 ORDER BY 时行序不作保证。
`search_mode=ann` 请求同样执行精确扫描，召回率为 1.0。

## 3. 错误处理

所有错误以 `{"ok":false,"error":{"code":"...","message":"..."}}` 返回，`code` 非空。
主要错误码：

| 类别 | 错误码 |
| --- | --- |
| 语法/协议 | `syntax_error`、`unsupported_sql`、`invalid_request`、`unsupported_op` |
| 对象 | `table_exists`、`table_not_found`、`column_not_found`、`duplicate_column`、`index_exists` |
| 数据 | `type_mismatch`、`value_out_of_range`、`string_too_long`、`duplicate_key`、`column_count_mismatch` |
| 向量 | `invalid_vector`（NaN/Infinity/非法字面量）、`dimension_mismatch`、`invalid_schema` |
| 事务 | `transaction_active`（重复 BEGIN） |
| 存储 | `storage_error`、`internal_error` |

语句级原子性：一条多行 INSERT/UPDATE/DELETE 失败时整体回滚，不留部分修改。
自动提交语句在执行成功后立即提交；显式事务内读到自己的修改。

## 4. 设计概要（4.3）

### 4.1 记录布局

`/data/heap.dat` 为只追加的行堆：每条记录以 `u32` 长度前缀开头，后接列值：

- BIGINT：8 字节小端有符号整数；
- VARCHAR：`u32` 字节长度 + UTF-8 字节；
- VECTOR：`dim * 4` 字节 float32 小端。

内存中每表维护 `slots`：`{pk, offset, length, vec_off, alive}`。`vec_off` 是该行向量列在
记录内的字节偏移，插入/更新/恢复时计算并缓存，无过滤的向量扫描据此跳过整行解析。
另有主键到 slot 的哈希表。UPDATE 追加新版本并改指 slot（保留旧版本供回滚）；
DELETE 仅置 `alive=false`。

### 4.2 向量索引方案

`CREATE VECTOR INDEX` 记录索引名称（用于模式兼容），实际检索为精确扫描：
AVX2+FMA 内核（运行时 `__builtin_cpu_supports` 检测，否则标量回退），
用 float 累加平方距离，TopK 用有界优先队列，仅在输出时开方。
这样 `exact` 与 `ann` 请求的 Recall@K 都是 1.0，且索引维护用例与普通记录保持一致。

### 4.3 事务与提交持久化

- 单连接、最多一个显式事务；自动提交为默认。
- 每条修改语句建立 savepoint，记录 undo 项（删除表/索引、插入 slot、旧版本指针等），
  语句失败或 ROLLBACK 逆序回放，保证语句级原子性。
- 修改同时写入内存中的 `pending` 操作列表；COMMIT（或自动提交）时分配 LSN，
  将整批操作编码为一条 WAL 记录写入 `wal.log`，`fdatasync` 后才返回成功。
- WAL 记录帧为 `[u32 payload_len][u32 crc32][payload]`；payload 含 LSN 与操作
  （建表/建索引/插入/更新/删除）。
- 行数据本身写入 mmap 的 heap，不逐条 fsync；崩溃时以 WAL 为准重放，已提交数据不丢失。
- 干净的 shutdown 回滚未提交事务、写快照并截断 WAL。

### 4.4 恢复流程

1. 打开 `heap.dat`（mmap）与 `snapshot.bin`，校验魔数、版本与 CRC，恢复表结构、alive slot 与
   LSN；
2. 顺序读 `wal.log`，逐帧校验长度与 CRC；只重放 LSN 大于快照 LSN 的记录，损坏的尾部帧截断；
3. 重放幂等（插入/更新/删除按主键归并），恢复途中再次被强杀后仍可继续。
4. WAL 超过 64 MiB 时在提交后写快照（tmp + rename + 目录 fsync），随后清空 WAL。

进程被 kill -9 时：已收到 COMMIT 成功的修改必须存在；未提交事务完全回滚；
“已发出提交但未收到响应”的事务可能完整生效或完整不生效（WAL 帧 CRC 保证不会部分生效）。

### 4.5 已知限制

- 单线程执行、单连接，不支持并发客户端；
- 删除行只在快照时压缩，长时间只删不插会浪费堆空间；
- 精确扫描复杂度 O(N·d)，未实现 IVF/HNSW 等近似索引；
- 不支持 NULL、JOIN、聚合、子查询、ALTER、外键、主键更新、嵌套事务。

## 5. 本地验证记录

使用仓库内 `protocol_check`（官方协议检查器，20 个公开用例）与本地故障注入脚本：

| 项目 | 结果 |
| --- | --- |
| 官方协议检查 `protocol_check/run.py` | 20/20 PASS（SQL、存储、精确检索、事务、恢复、索引维护） |
| kill -9 故障注入（9 项） | 全部通过：自动提交持久性、未提交回滚、提交中断全有或全无、重复恢复幂等、失败多行插入无残留、非法向量拒绝、语句语义、VARCHAR 码点长度 |
| 独立暴力核对（随机数据） | TopK/过滤/并列与 Python 暴力结果一致，最大距离绝对误差 7.087e-07 |
| 性能自测（10,000×128 维，1,000 TopK，K=10，一半带过滤） | 批量写入约 0.66–0.69 s；查询合计约 0.32–0.36 s（单查询均值约 0.33 ms）；峰值 RSS 约 11.8 MB；重启就绪约 10 ms |
| 大数据量（600,000 行 × 128 维，数据文件 552 MB） | 写入 37.4 s；重启就绪 0.19 s（快照装载不读堆内行数据）；全表 TopK 约 63 ms、过滤 TopK 约 22 ms（多次运行随机器负载波动）；重启后 RSS 46 MB，结果与重启前一致 |
| 数据目录解析（容器兼容） | 判题容器以 `--read-only` 运行、`VECTORDB_DATA_DIR` 指向容器内不可写路径时，自动回退到可写的 `/data`；显式参数、环境变量、`--data-dir` 三种形式均验证通过 |

性能自测脚本为本地工具，非正式成绩；正式成绩以平台对锁定版本的评测为准。
