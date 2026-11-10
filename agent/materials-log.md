# Agent Prompt 材料采集日志（实时）

规则：只记真实发生的事；每条必须能对应到具体输出或文件位置。评审入口是仓库里的 `agent/conversation.json`，本文件只是采集底稿。

## 实例 1 · 质疑题面转述，回到原始文档核对
- 学生输入（原文）：「不太对吧，你访问 https://oceanbase.yuque.com/... 详细看一遍」
- 触发的问题：Agent 总结了 5 条错误判断（"整题只提测一次"、"题包拿不到"、"stdin 收 SQL/stdout 返表格"、数据规模 1M×128、"近似索引先不做"）。
- 验证动作：直接抓语雀原文（16,028 字）+ GitHub 分支 `2026-competition-round2` 的 `mysqltest/`、`STDIO_PROTOCOL.md`、`protocol_check/`。
- 结果：5 条全部证伪/修正——每天最多 2 次正式提交；题包在 GitHub 上；协议是 JSONL；规模 10,000×128；ann 反而亏。
- 证据位置：`_yuque/赛题一-完成一个轻量向量数据库.md`、`_yuque/STDIO_PROTOCOL.md`、`ref/mysqltest/manifest.json`（20 条 case）。
- 归属维度：独立验证与质疑。

## 实例 2 · 用官方驱动量化基线，而不是靠推测
- 动作：`tools/run_public.sh` 驱动 20 个公开用例跑本地二进制。
- 结果：11 PASS / 9 FAIL，9 个失败全部集中在 `--restart` 类（`no_such_table`），把缺口精确锁定在「持久化 + 恢复」，而不是语法或类型。
- 证据位置：`/root/pr-public/result.json`（运行产物）、本仓库 `tools/run_public.sh`。
- 归属维度：实验与反馈迭代。


## 实例 3 · 用「复现脚本」否证并行分析结论，而不是直接采信
- 学生/流程输入：子代理 C 提交 26 条失分点清单，但明确声明「WSL 起不来，全程零执行，结论为静态推演」。
- 验证动作：协调侧自建 `tools/probe_gaps.sh`（10 条 JSONL 请求），对真实二进制逐条复现。
- 结果：C 报的 4 条高危**全部复现属实**——
  - `VARCHAR(0)` + 10 字符 → `{"ok":true}`（应拒绝）
  - `L2_DISTANCE(e,'[nan,0]')` → `{"ok":true,"rows":[[1,0]]}`（应拒绝，且非有限值被静默写成 0）
  - `L2_DISTANCE(e,'[1e400,0]')` → 同上
  - `UPDATE v SET id=5 WHERE id=999` → `{"ok":true,"affected_rows":0}`（主键更新必须报错）
- 证据位置：`tools/probe_gaps.sh` + 本机运行输出（见 PR 记录）。
- 归属维度：独立验证与质疑。

## 实例 4 · 拒绝「看起来跑过」的自报，用 ctest 独立复核
- 动作：不采信子代理自述，自己跑 `/root/build/storage_test` 与 `ctest`。
- 结果：storage 379 assertions PASS、vector 735 checks PASS、`100% tests passed, 0 failed out of 2`。
- 顺带核实实现细节：记录头 `[u32 len][u32 crc32][u64 txn][u8 type][payload]`，CRC 覆盖 txn+type+payload；提交 = 整批 + COMMIT 一次 write + 一次 fdatasync；写失败时 ftruncate 回滚半条事务。
- 归属维度：实验与反馈迭代。

## 实例 5 · 环境障碍的解法也要留档（可迁移）
- 现象：`win_bash` 里调 `wsl.exe` 一律 `Wsl/E_ACCESSDENIED`；Windows 侧只有 MSYS2 的 cmake、没有编译器；VS 只装了 Installer 无 VC++ 工具集。
- 判定与绕行：`run_code` 的 Node 进程令牌不同，`child_process.spawnSync("wsl.exe", ...)` 正常 → 构建与全部验证改走这条通道。
- 迁移价值：同类「工具表面被降权但宿主进程仍有权限」的场景可复用该判据（换执行宿主，而不是换工具链）。
- 归属维度：技术理解与迁移。


## 实例 6 · 门槛判断优先于优化冲动
- 学生侧约束：只剩有限比赛时间，性能与材料都折算排名分，但两者都要求正式用例全过。
- 判断链：读题面 3.2 节与 mysqltest/README 末段 → 发现「数据量超过内存限额」是明确正式要求 → 估量（512 B/行 × 数十万行即触 2 GiB）→ 结论：外存化优先级高于查询提速，因为门槛不过两项同时归零。
- 结果：并行派发「外存行存储」与「Docker 端到端验证」两任务，把查询微优化延后到架构改造之后（避免在会被重写的热路径上做无用功）。
- 证据位置：docs/决策记录.md D8、D10；tools/bench.py 实测数字。
- 归属维度：取舍与反思。


## 实例 7 · 「本地能过」不等于「平台能过」——用判题同款约束复验
- 判断：判题器用 docker run -i 起容器、不补参数、根文件系统只读、无网络、2 GiB 内存。
- 动作：不只跑 docker build，而是用 `--read-only --network=none --cpus=2 --memory=2g --pids-limit=128 --cap-drop=ALL --security-opt=no-new-privileges` + bind /data 复跑握手与 CRUD。
- 结果：镜像 28 MiB、冷构建 74.7s、镜像模式公开用例 20/20、加固运行 ALL PASS；根 fs 零写入。
- 顺带纠错：发现 `-j\$(nproc)` 在受限容器里会取宿主核数（易 OOM）、`# syntax` 指令在本机 legacy builder 下无意义 → 一并改掉。
- 证据位置：Dockerfile、tools/docker_check.sh（幂等，失败退出 1）。
- 归属维度：取舍与反思 / 实验与反馈迭代。

## 待补
- 外存改造后的 RSS 与 20/20 回归（子代理 H）
- 最终快照的 docker_check.sh 全绿证据
- 优化前后 T/B/M 对比（tools/bench.py）
