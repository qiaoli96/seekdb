# MiniVectorDB 正式用例失分点清单（formal-gap）

> 范围：仅分析与文档，不含生产代码改动。本文是本任务边界内唯一新增文件。
> 分析对象：`ref/mysqltest/cases/`（20 用例 / 95 条 SQL）、`ref/mysqltest/README.md`、`ref/protocol_check/`（run.py / _assertions.py / config.json）、`ref/STDIO_PROTOCOL.md`，对照 `src/`。
> 证据口径：每条结论给出 `.test` 行号或 `src/<文件>:<行号>`。凡未实机验证的推演，一律标注 **待验证**。

## 0. 方法与限制（先说清楚哪些是推演）

| 项 | 状态 |
| --- | --- |
| WSL 执行环境 | **不可用**：`wsl.exe -d Ubuntu-24.04` 返回 `Wsl/E_ACCESSDENIED`（注册表项访问被拒），无法编译或运行任何二进制 |
| 本机 gcc/cmake | MSYS2 侧 `gcc`、`cmake` 均不存在（`which gcc` → not found），同样无法构建 |
| 因此 | 全部结论为**静态代码推演**；涉及运行期数值/时序的条目已标 **待验证**，并给出上游可直接执行的复现序列 |
| 推演可靠性 | 解析/执行路径逐语句走查（lexer → parser → engine → json 输出）；给出行号即代表代码确实这样走，非猜测 |
| 分析时点 | **20:52 快照**为主。复查时间线：20:59 `src/storage.cpp` 出现（27437 B）、21:01 `src/vector.cpp` 出现（均由并行任务产出），即 G01 的「文件缺失」在分析过程中被并行修复；但 `engine.cpp`/`parser.cpp`/`main.cpp`/`json.cpp` 的 mtime 始终为 20:49-20:50，**未随之改动**，故本文对这些文件引用的行号全部有效，G02（storage 未接线）、G23（引擎仍用私有 l2_distance）依旧成立。**编译/链接是否通过，本环境无法验证 → 待验证** |

---

## 1. 公开用例逐条语义点（20 用例 / 95 条 SQL）

判定按 `_assertions.py:14-41` + `config.json` 的 `error_codes: {}`（空映射 → 只校验错误码非空，见 `_assertions.py:19-21`）推演。

| 用例 | SQL 行 | 检验语义点 | 公开判定（推演） |
| --- | --- | --- | --- |
| sql_crud | `sql_crud.test:1-7` | 建表 / 单行插入 / 投影 / UPDATE 单行 / DELETE / 空结果集 | 通过 |
| sql_comparisons | `sql_comparisons.test:1-4` | BIGINT 比较 `>=`、`<`、AND 合取（期望 id=2） | 通过（`engine.cpp:302-304` 逐条 AND） |
| sql_ordering | `sql_ordering.test:1-4` | ORDER BY 升/降序、LIMIT 截断 | 通过（`engine.cpp:324-337`） |
| sql_empty | `sql_empty.test:1-3` | 空表查询返回列名 + 空 rows、空表 DELETE → affected_rows 0 | 通过 |
| sql_primary_key | `sql_primary_key.test:1-4` | 重复主键报 `duplicate_key` 且保留原记录 | 通过（`engine.cpp:214-215`） |
| sql_unicode | `sql_unicode.test:1-3` | 中文字符串写入/读出（UTF-8 原样回传） | 通过（`json.cpp:199-207` 原字节输出） |
| sql_bigint | `sql_bigint.test:1-3` | ±9007199254740993 精确读写（>2^53，不得经 double） | 通过（`parser.cpp:57-60` strtoll / `main.cpp:13` %lld） |
| sql_unsupported | `sql_unsupported.test:1-3` | ALTER TABLE 必须报错且报错后继续服务 | 通过（错误码名不一致见 G18/G24） |
| storage_restart | `storage_restart.test:1-4`（:3 为 --restart） | 自动提交记录正常重启后保留 | **失败**（无持久化，G02） |
| vector_euclidean | `vector_euclidean.test:1-3` | `[3,4]`→`[0,0]` 欧氏距离 5.0（非平方） | 通过（`engine.cpp:96-104`） |
| transaction_commit | `transaction_commit.test:1-6` | BEGIN/UPDATE/COMMIT 后可见 | 通过（`engine.cpp:118-127`） |
| transaction_rollback | `transaction_rollback.test:1-6` | ROLLBACK 后恢复 | 通过（`engine.cpp:128-134`） |
| storage_mutations | `storage_mutations.test:1-7`（:5 restart） | UPDATE+DELETE+向量改写在重启后一致 | **失败**（G02） |
| storage_multiple_tables | `storage_multiple_tables.test:1-7`（:5 restart） | 多表定义与行持久化 | **失败**（G02） |
| vector_ties_and_filter | `vector_ties_and_filter.test:1-5` | 先过滤后 TopK；距离并列按主键升序 | 通过（`:302-305` 先过滤、`:330` pk 升序作并列键） |
| vector_float32 | `vector_float32.test:1-3` | `16777217` 存为 float32 → 与 `16777216` 距离 0.0，并列按 pk 升序 | 通过（`engine.cpp:34` 转 float、`:330`） |
| vector_index_maintenance | `vector_index_maintenance.test:1-11`（:8 restart、:10 --mode ann） | 建索引后增删改一致；重启一致；ann 结果相同 | **失败**（:8 走 G02；ann 分支本身通过，G16/G17） |
| transaction_failed_insert | `transaction_failed_insert.test:1-9`（:8 restart） | 事务内多行 INSERT 失败原子性、失败后仍可 COMMIT | **失败**（:8 走 G02；1-7 行语义通过） |
| transaction_autocommit_atomic | `transaction_autocommit_atomic.test:1-5`（:4 restart） | 自动提交语句失败不得留部分数据 | **失败**（:4 走 G02；维度错整条拒绝本身正确，G08） |
| transaction_shutdown_rollback | `transaction_shutdown_rollback.test:1-6`（:5 restart） | 正常关闭回滚未提交，重启后看到 3 行 | **失败**（G02：重启后表不存在，返回 no_such_table，与「回滚」不可区分） |

**关键结论：「公开用例能过」这个前提不成立。** 含 `--restart` 的公开用例共 **7 个**（storage_restart、storage_mutations、storage_multiple_tables、vector_index_maintenance、transaction_failed_insert、transaction_autocommit_atomic、transaction_shutdown_rollback），它们全部依赖持久化；当前 `src/` 无任何落盘代码，故这 7 个用例在其 restart 环节必然失败。

---

## 2. 失分点清单

风险：**高** = 必然扣分且牵连多个用例；**中** = 正式用例很可能覆盖、当前行为与语义不符；**低** = 语义正确或边角，仅记录。

| 编号 | 语义点 | 来源 | 当前实现行为 | 风险 | 建议修复 |
| --- | --- | --- | --- | --- | --- |
| G01 | 工程可构建性 | 仅正式用例（基础设施） | 20:52 快照：`CMakeLists.txt:12-18` 的 MVDB_SOURCES 列出的 `src/storage.cpp`、`src/vector.cpp` **两文件都不存在** → cmake 配置阶段报 Cannot find source file。复查：20:59 storage.cpp、21:01 vector.cpp 先后由并行任务产出，**源码文件已齐**；但两者从未被编译过一次 → 编译/链接是否通过 **待验证**（`vector.h:10-21` 声明 `vec::l2_distance/topk_select/simd_backend`，实现与声明必须逐字对齐，否则链接失败） | 高 | 立刻跑一次 `cmake -S . -B build && cmake --build build -j` 并把结果贴进决策记录。已核对：`src/vector.cpp:135/139/141` 分别定义了 `l2_distance`、`simd_backend`、`topk_select`，与 `vector.h:10-21` 的声明逐一对应；`src/storage.cpp:591/647/654/698/787/829` 定义了 `Storage::open/close/commit_batch/replay/save_catalog/load_catalog` 全套接口 → 命名层面无缺口，只差一次真实编译 |
| G02 | 持久化 / 重启（catalog + 行数据），即**模块已写但未接线** | 公开用例 7 个含 restart 的用例 | `main.cpp:89-93`：`--data-dir` 解析后 `(void)data_dir;`；21:00 复查 `grep -rn "Storage" src/*.cpp src/*.h`（排除 storage.cpp 自身）**零命中**，`engine.h` 不 include `storage.h` → 存储模块与执行器完全脱耦，数据仍只活在 `engine.h:40` 的进程内 `std::map`；`engine.cpp` 也没有产生 `Mutation`（`storage.h:9-14`）的路径 | 高 | 三处接线：①`main.cpp:89-93` open/replay/close（`Storage::open` 已实现于 `storage.cpp:591`）；②`Engine` 在 COMMIT（`engine.cpp:122-127`）与每条自动提交语句成功后输出 `Mutation`（`storage.h:9-14`）批次调 `commit_batch`（`storage.cpp:654`）；③未提交事务不落盘——回滚就是「从未写过」。接口齐全，缺的只是调用点 |
| G03 | 正常关闭回滚未提交（持久化层面） | 公开用例 `transaction_shutdown_rollback.test:3-6` | 内存层正确（`main.cpp:118-123` → `engine.cpp:149-155` rollback_on_shutdown）；但库不在磁盘，重启后 `engine.cpp:231-232` 返回 `no_such_table` | 高 | 随 G02；验收见 §3.4 |
| G04 | 强杀后已提交保留、未提交不部分生效、恢复途中再退出仍可恢复 | 仅正式用例（`ref/mysqltest/README.md`「已确认提交在进程强杀后保留…恢复途中再次退出仍能恢复」） | 无 WAL、无 fsync、无长度/CRC 校验（`storage.h:18-20` 仅注释） | 高 | COMMIT 前 append + crc + fdatasync；恢复遇长度/CRC 不合法即截断；重放幂等 |
| G05 | 查询向量拒绝 NaN / Infinity / float32 溢出 | 仅正式用例（README「拒绝 NaN、Infinity、维度错误、float32 溢出等非法向量」） | **校验不对称**：INSERT 路径 `engine.cpp:30-33` 有 isfinite + fabs(v)>FLT_MAX；`L2_DISTANCE` 查询向量走 `parser.cpp:237-253` parse_float_list，**两项检查全无** | 高 | 把数值校验收敛为唯一入口（复现见 §3.1） |
| G06 | 非有限浮点不得出现在 stdout | 公开协议 `STDIO_PROTOCOL.md:27` | `json.cpp:212-214`：非有限值被**静默写成 `0`**，报文合法但答案错 | 高 | 产生值处即报错（配合 G05）；该分支改为上报内部错误 |
| G07 | UPDATE 主键必须报错、不得静默执行 | 仅正式用例（README「主键更新——遇到必须返回错误，不静默执行」） | `engine.cpp:381-386` 检查被放进 `for (size_t unused_r : hit)` 循环体内 → `hit` 为空时**完全不检查**：0 命中返回 `ok:true, affected_rows:0`；命中时返回 `unsupported_update`（`:384`） | 高 | 检查移到 `hit` 计算之前、与命中数无关（§3.2） |
| G08 | 多行 INSERT 原子性 | 公开用例 `transaction_autocommit_atomic.test:3` | 正确：`engine.cpp:198-218` 先整体 coerce + 查重进 `prepared`，`:220-227` 才落地 | 低 | 无需修复（保持「先校验后落地」不变式） |
| G09 | 语句失败不留部分修改（UPDATE/DELETE） | 公开用例 `transaction_failed_insert.test:5` | 正确：`engine.cpp:348-357`（SET 先 coerce）+ `:359-368`（WHERE 先 coerce）+ `:387-388`（最后落地）；DELETE 无中途失败点 | 低 | 无需修复；但注意 G07 的校验位置缺陷 |
| G10 | 事务内读己写 | 公开用例 `transaction_commit.test:4-5` | 正确：无快照读，`engine.cpp:298-315` 直接扫 `tables_`，`:387-388` 直接改 `tables_` | 低 | 无需修复 |
| G11 | ROLLBACK 后向量检索恢复 | 公开用例 `transaction_rollback.test:1-6`（普通列）+ 正式（向量列） | 正确：`engine.cpp:130` `tables_ = snapshot_` 为整表深拷贝（`engine.h:40-41`），向量列随行恢复 | 低 | 无需修复；索引登记不随 ROLLBACK 恢复（G16，当前无功能影响） |
| G12 | 未指定 ORDER BY 的集合语义 | 仅正式用例（`_assertions.py:32-39` 提供 unordered 多重集比较） | `engine.cpp:324` 仅在 order 非空时排序 → 返回**物理插入序**（`storage_multiple_tables.test:2` 插入 3,1,2 → 输出 3,1,2） | 中 | 语义上未定义，判题器应标 unordered；若正式 .result 漏标，按主键升序返回更贴近 InnoDB 型参考（判断题，需在决策记录留痕） |
| G13 | ORDER BY 并列按主键升序 | 公开用例 `vector_ties_and_filter.result` 第 4 条断言（[3,1] 先于 [8,1]）、`vector_float32.result` | 正确：`engine.cpp:325-331` 逐键比较后 `return a.pk < b.pk;` | 低 | 无需修复 |
| G14 | LIMIT 与过滤后 TopK | 公开用例 `vector_ties_and_filter.test:3`（filter 后 LIMIT 1 → id 3）、`:5` | 正确：过滤(`:302-305`)→ 距离(`:310-315`)→ 排序(`:325`)→ LIMIT(`:336-337`) | 低 | 无需修复 |
| G15 | VARCHAR 长度按 Unicode 字符数 | 公开用例 `sql_unicode.test:1-3` + 正式边界 | 正确：`engine.cpp:10-15` utf8_length 数非续字节 = 码点数，中文/emoji 各计 1 | 低 | 本身正确；开关缺陷见 G15b |
| G15b | 长度上限被 0 / 负值整体旁路 | 仅正式用例 | `engine.cpp:51` 条件写作 `col.varchar_len > 0 && ...` → `VARCHAR(0)`、`VARCHAR(-1)`（`parser.cpp:41-42` 允许负数字面量）时**任何长度都放行**；`parser.cpp:161` 也无上下限校验 | 中 | 建表期校验 `0 < varchar_len <= 上限`；长度检查无条件执行（§3.3） |
| G16 | CREATE VECTOR INDEX 后增删改一致 | 公开用例 `vector_index_maintenance.test:3-9` + 正式（无重复 ID、过滤召回） | `engine.cpp:182-191` 只校验并往 `indexed_tables_` 插名字，查询永远暴力精确 → 一致性天然成立；但无重复建索引校验、索引状态不持久化、ROLLBACK 不回滚该集合（`:128-134` 只恢复 `tables_`） | 低 | 若坚持 D2（只做 exact），把索引登记明确排除在语义之外并文档化；重复建索引应报错 |
| G17 | exact/ann 模式切换 | 公开用例 `vector_index_maintenance.test:10-11` | `engine.cpp:113` 形参写成 `/*mode*/`，`main.cpp:131-138` 传入后完全丢弃 → 两模式结果恒同（公开断言恰好相同，通过） | 中 | 记录为有意的 D2 决策；未知 search_mode 值应报错（当前静默按 exact） |
| G18 | 不支持语句（ALTER/JOIN/聚合/NULL/子查询）必须报错 | 公开用例 `sql_unsupported.test:2` + 正式 | **会报错但语义码不一致**：`parser.cpp:349` 统一 `unsupported_statement`，而 `sql_unsupported.result` 第 2 条断言写的是 `"error":"unsupported_sql"`；公开 config 的 error_codes 为空使其侥幸通过（`_assertions.py:19-21`） | 高 | 对外错误码对齐（§4） |
| G19 | JOIN / 聚合 / 子查询 / OR 的具体拒绝路径 | 仅正式用例 | JOIN → `parser.cpp:358-364` trailing tokens（syntax_error）；COUNT(*) → `:300` expected FROM；IN (SELECT …) → `:192` expected comparison operator；NULL → `:147` expected literal | 中 | 行为正确（都报错）；建议显式识别这些关键字并统一回 `unsupported_sql`，避免错误码四散（§3.6） |
| G20 | BIGINT 越界字面量 | 仅正式用例 | `parser.cpp:57-60`：strtoll ERANGE → 词法层置 Invalid/syntax_error；`-9223372036854775809` 同路 | 中 | 数值类错误应回数值语义码；≥2^53 精确性已由 `sql_bigint` 验证（`main.cpp:13` %lld、`json.cpp:125-127` from_chars） |
| G21 | 字符串按码点比较 | 仅正式用例（`sql_unicode` 明说「不混入…字符排序」，即公开包未测） | `engine.cpp:77-80` `std::string::compare` 即 char_traits 的无符号字节比较（memcmp 语义）；UTF-8 字节序与码点序严格同序 → **正确** | 低 | 无需修复；建议补一条 `WHERE content > '中'` 自测锁定该性质 |
| G22 | float32 存储 + 距离计算精度 | 公开用例 `vector_float32.test:2-3` | 存储正确（`value.h:34` `vector<float>`、`engine.cpp:34` double→float，16777217→16777216）；累加用 double（`engine.cpp:96-104`） | 低-中（**待验证**） | 若参考按 float32 逐项累加，128 维相对差约 1e-7，仍在 1e-5 容差内；但 `vector.h:10` 与 `engine.cpp:96` 存在两份 l2_distance，必须收敛（G23） |
| G23 | 距离/TopK 单一真源 | 仅正式用例（一致性与可维护性） | `vector.h:10-21` 声明 `vec::l2_distance/topk_select/simd_backend`，引擎完全不使用（`engine.cpp:96` 私有同名实现，排序写死在 `:324-332`） | 中 | 引擎改调 `vec::` 内核，避免「单测过了、引擎里另一份挂」 |
| G24 | 全量常驻内存 | 仅正式用例（README「能处理超过内存限额的数据，不能只依赖全量常驻内存」） | `engine.h:40-41` 全量 `std::vector<Row>`；`engine.cpp:118` BEGIN 深拷贝整库 → 峰值内存约 2× 数据集 | 中 | 至少做分块 + LRU 限额；BEGIN 用 undo log 替代全量拷贝 |
| G25 | 向量字面量解析过宽松 | 仅正式用例（非法向量应拒绝） | `engine.cpp:24` 与 `parser.cpp:243` 把逗号与空白一律当分隔符跳过 → `'[1,,2]'`、`'[,1,2]'`、`'[1,2,]'`、`'[0 2]'` 全被接受；只有 `'[1,2]x'` 被拒（`engine.cpp:37-38`） | 中 | 严格文法：单逗号分隔、无空项、无尾随逗号 |
| G26 | 协议层细节 | `STDIO_PROTOCOL.md:19-30,111-128` | ready 精确且 flush（`main.cpp:96` + `:80-84`）✓；id 原类型回传（`main.cpp:35-40`）✓；shutdown 回 `{"ok":true}` 后 return 0 不再输出（`:118-123`）✓；字段名 search_mode（`:131`）✓；JSON Unicode 转义含代理对解码正确（`json.cpp:84-104`）✓ | 低 | 唯一瑕疵：解析失败时硬写 `"id":null`（`main.cpp:106-110`），且 `:107` 的 `Result bad` 是死变量 |

---

## 3. 关键失分点的可复现明细

### 3.1 G05 + G06：查询向量 NaN / Infinity 被静默算成 0

SQL 序列：

    CREATE TABLE t (id BIGINT PRIMARY KEY, v VECTOR(2));
    INSERT INTO t VALUES (1, '[0,0]');
    SELECT id, L2_DISTANCE(v, '[nan,0]') AS d FROM t;
    SELECT id, L2_DISTANCE(v, '[1e400,0]') AS d FROM t;
    SELECT id, L2_DISTANCE(v, '[3.5e38,0]') AS d FROM t;

- 期望：三条查询均 `ok:false`（非法向量），进程继续服务。
- 当前：`parser.cpp:247` strtod 接受 nan / 1e400(→inf) / 3.5e38，`:249` static_cast<float> 直接收下；`engine.cpp:255` 只校验维度（2==2 通过）；`:96-104` 得 NaN/Inf；`main.cpp:21` → `json.cpp:213` 写成 `0`。
- 实际响应：`{"id":N,"ok":true,"columns":["id","d"],"rows":[[1,0]]}` —— 既不是报错，也不是非法 JSON，而是**合法但错误的结果**。
- 反证这不是有意设计：同一个 nan 写进 INSERT 会被 `engine.cpp:30-33` 拒绝（invalid_vector），说明实现内部分叉。

### 3.2 G07：UPDATE 主键在 0 命中时静默成功

    CREATE TABLE t (id BIGINT PRIMARY KEY, c VARCHAR(8));
    UPDATE t SET id = 5 WHERE id = 1;      -- 表为空，0 命中
    UPDATE t SET id = 5;                   -- 表为空，0 命中
    INSERT INTO t VALUES (1, 'a');
    UPDATE t SET id = 5 WHERE id = 999;    -- 有行但 0 命中

- 期望：三条 UPDATE 都在「SET 主键」这一点报错。
- 当前：`engine.cpp:381-386` 把检查放进 `for (size_t unused_r : hit)`，`hit` 为空 → 一次都不执行 → 返回 `ok:true, affected_rows:0`；只有命中行时才回 `unsupported_update`（`:384`）。

### 3.3 G15b：VARCHAR(0) 让长度上限失效

    CREATE TABLE t (id BIGINT PRIMARY KEY, c VARCHAR(0));
    INSERT INTO t VALUES (1, '任意长度的字符串都会被接受');
    SELECT c FROM t;

- 期望：建表期拒绝非法长度（或插入期 value_too_long）。
- 当前：`parser.cpp:161` 无条件接受 0；`engine.cpp:51` 的 `varchar_len > 0` 前置为假 → 不校验 → 三步全 ok:true。`VARCHAR(-1)` 同路（`parser.cpp:41-42`）。

### 3.4 G02 / G03：重启后整库消失（不是「回滚」，是「不存在」）

进程 1：

    CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(8), content VARCHAR(16));
    INSERT INTO docs VALUES (3,'乙','三'),(1,'甲','一'),(2,'甲','二');
    BEGIN;
    DELETE FROM docs WHERE id >= 1;
    -- 正常关闭（op=shutdown）

进程 2（同一 --data-dir）：

    SELECT id FROM docs ORDER BY id;

- 期望（`transaction_shutdown_rollback.result` 第 5 条断言）：`rows:[[1],[2],[3]]`。
- 当前：`main.cpp:93` `(void)data_dir;` → 进程 2 的 Engine 是空 map（`engine.h:40`）→ `engine.cpp:231-232` → `no_such_table`。
- 同一根因覆盖 §1 表中 7 个含 restart 的用例。

### 3.5 G12：无 ORDER BY 时的物理顺序

    CREATE TABLE docs (id BIGINT PRIMARY KEY, content VARCHAR(16));
    INSERT INTO docs VALUES (3,'三'),(1,'一'),(2,'二');
    SELECT id FROM docs;

- 若正式 .result 未标 unordered 且期望 `[[1],[2],[3]]`：当前返回 `[[3],[1],[2]]`（`engine.cpp:324` 不排序）。
- 公开包规避了这个坑：`storage_multiple_tables.test:7`、`storage_mutations.test:6` 都写了 `ORDER BY id`。正式用例是否同样谨慎，**待验证**。

### 3.6 G18 / G19：不支持语句的错误码分布

| SQL | 当前 error.code | 依据 |
| --- | --- | --- |
| `ALTER TABLE docs ADD extra BIGINT` | `unsupported_statement` | `parser.cpp:349` |
| `SELECT * FROM a JOIN b ON a.id=b.id` | `syntax_error`（trailing tokens） | `parser.cpp:358-364` |
| `SELECT COUNT(*) FROM docs` | `syntax_error`（expected FROM） | `parser.cpp:300` |
| `SELECT id FROM docs WHERE id IN (SELECT id FROM t)` | `syntax_error`（expected comparison operator） | `parser.cpp:192` |
| `INSERT INTO docs VALUES (1, NULL)` | `syntax_error`（expected literal） | `parser.cpp:147` |
| `SELECT id FROM docs WHERE a=1 OR b=2` | `syntax_error`（trailing tokens） | `parser.cpp:197-198` + `:358-364` |

全部「报错而非静默」✓，但 5 种语义不同的拒绝只用了两个码，而 .result 词汇表是 `unsupported_sql`。

---

## 4. 错误码映射：现状 vs .result 词汇表

公开 config 的 error_codes 为空（`ref/protocol_check/config.json`），故 `_assertions.py:19-21` 只要求非空字符串；正式映射一旦冻结，名称必须逐字命中。

| .result 中的语义名 | 出处 | 当前返回 | 是否命中 |
| --- | --- | --- | --- |
| `duplicate_key` | `sql_primary_key.result` 第 3 条、`transaction_failed_insert.result` 第 5 条 | `duplicate_key`（`engine.cpp:215`） | 命中 |
| `dimension_mismatch` | `transaction_autocommit_atomic.result` 第 3 条 | `dimension_mismatch`（`engine.cpp:63`） | 命中 |
| `unsupported_sql` | `sql_unsupported.result` 第 2 条 | `unsupported_statement`（`parser.cpp:349`） | **不命中（必然失分）** |

无公开背书的自造码（正式映射未知，**待验证**）：table_exists、no_such_table、no_such_column、invalid_schema、invalid_index、type_mismatch、value_too_long、column_count_mismatch、invalid_vector、invalid_distance、unsupported_comparison、unsupported_update、nested_transaction、syntax_error、invalid_json、unsupported_op、invalid_request。

建议：在 `main.cpp:47-53` 的错误序列化处加一层「内部细码 → 对外语义码」别名表，改名只改一处，不牵动 `engine.cpp` 判断逻辑。

---

## 5. 任务要求覆盖核对（17 项 → 条目）

| 要求覆盖点 | 落点 | 结论 |
| --- | --- | --- |
| VARCHAR 长度按 Unicode 字符数 | G15 / G15b | 码点计数正确；0/负长度旁路 |
| 字符串按码点比较 | G21 | 正确（UTF-8 字节序 = 码点序） |
| BIGINT 超 2^53 | G20 + §1 sql_bigint | 精确读写正确；越界字面量错误码待定 |
| float32 存储与距离计算 | G22 + §1 vector_float32 | 存储正确；累加精度待验证；双实现待收敛 |
| 维度不符 / NaN / Inf / float32 溢出拒绝 | G05 / G06 / G25 | INSERT 侧正确，**查询向量侧失守（最高价值修复）** |
| 主键重复 | G08 + §1 sql_primary_key | 正确 |
| UPDATE 主键 | G07 | **0 命中静默** |
| 多行 INSERT 原子性 | G08 | 正确 |
| 语句失败不留部分修改 | G09 | 正确 |
| 事务内读己写 | G10 | 正确 |
| ROLLBACK 后向量检索恢复 | G11 | 正确（内存层） |
| 正常关闭回滚未提交 | G03 + G02 | 内存层正确 / 持久化层缺失 |
| 未指定 ORDER BY 的集合语义 | G12 | 中风险，取决于正式 .result 是否标 unordered |
| ORDER BY 并列按主键升序 | G13 | 正确 |
| LIMIT 与过滤后 TopK | G14 | 正确 |
| CREATE VECTOR INDEX 后增删改一致 | G16 | 结果一致；索引状态不持久化、无重复校验 |
| exact / ann 模式切换 | G17 | 模式被丢弃（有意 D2），公开过、正式待验证 |
| 不支持语句必须报错 | G18 / G19 | 报错正确；错误码名不符 |

---

## 6. 按风险排序的修复优先级

| 序 | 条目 | 为什么排这里 | 验收方式 |
| --- | --- | --- | --- |
| 1 | G01 首次真实构建（源码已齐、从未编译） | 构建不起来 = 全 0 分，且掩盖下面全部条目；两个 .cpp 是并行产出、无人编译过 → 待验证 | `cmake -S . -B build && cmake --build build -j` 产出 build/minivectordb，零 error |
| 2 | G02 storage 模块接线（open / replay / emit Mutation） | 独占 7 个公开用例的 restart 行 + 正式 storage 全类；`storage.cpp` 已存在但无人调用 | `bash tools/run_public.sh`，7 个 restart 用例全绿 |
| 3 | G03 关闭回滚 + 重启可见 | 是 G02 的语义验收面，必须一起验 | 单跑 transaction_shutdown_rollback：重启后 3 行 |
| 4 | G04 强杀持久性 / CRC 截断恢复 | 正式明确要求，且「恢复途中再退出仍能恢复」要求幂等 | COMMIT 后 kill -9，重启数据在；写到一半再杀仍能起 |
| 5 | G05 + G06 查询向量非法值 + 非有限输出 | 唯一「静默给错答案」的路径 | §3.1 三条 SQL 全 ok:false |
| 6 | G07 UPDATE 主键 0 命中 | 一行位置的 bug，命中即错 | §3.2 三条 UPDATE 全 ok:false |
| 7 | G18 错误码对齐 unsupported_sql | 公开 .result 已写死该词 | sql_unsupported 用冻结映射自测 |
| 8 | G15b VARCHAR(0)/负长度旁路 | 一行条件即可修 | §3.3 建表即报错 |
| 9 | G19 拒绝路径收敛 | 当前是「顺带报错」，错误码四散 | §3.6 六条 SQL 全 ok:false 且码在词汇表内 |
| 10 | G23 统一到 vec:: 内核 | 与第 5 项同文件范围，顺路做，防「修一处漏一处」 | src/vector.cpp 有实现，引擎调用点唯一 |
| 11 | G22 float32 累加口径 | 需先确认正式容差口径 → 待验证 | 256/1024 维对比 double 与 float 累加差值是否超 1e-5 |
| 12 | G12 无 ORDER BY 的默认顺序 | 判断题，改与不改都可能错 | 先确认正式 .result 是否标 unordered |
| 13 | G17 ann/exact 与非法 search_mode | 公开已过；非法值静默最危险 | `search_mode:"ivf"` 应报错 |
| 14 | G24 内存上限 | 需架构改动，仅在正式压到超内存时才有分 | 超容器内存数据集装载不被 OOM |
| 15 | G16 / G25 / G26 边角 | 影响面小、修起来碎 | 各一条 SQL / 报文级断言 |

---

## 附：本次实际核对过的文件

- `ref/mysqltest/cases/*.test`（20 个 / 95 条 SQL，逐行读）
- `ref/mysqltest/cases/*.result`（20 个，全部断言）
- `ref/mysqltest/README.md`（正式要求章节「正式验收要求仍然公开」）
- `ref/mysqltest/manifest.json`
- `ref/STDIO_PROTOCOL.md`（全文 311 行）
- `ref/protocol_check/_assertions.py`、`run.py`（前 70 行）、`config.json`
- `src/engine.cpp`（全文 421 行）、`src/parser.cpp`（全文 366 行）、`src/main.cpp`（全文 144 行）、`src/json.cpp`（全文 220 行）
- `src/engine.h`、`src/value.h`、`src/storage.h`、`src/vector.h`
- `CMakeLists.txt`、`README.md`、`docs/决策记录.md`、`tools/run_public.sh`
