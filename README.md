# MiniVectorDB（赛题一：完成一个轻量向量数据库）

单机、单进程、单客户端，通过 stdin/stdout 的 JSONL 协议与判题器通信，数据目录固定为 `/data`。
协议细节见 [STDIO_PROTOCOL.md](STDIO_PROTOCOL.md)。

## 构建

```bash
docker build -t minivectordb .
```

镜像内以 `/usr/local/bin/mvd /data` 启动（`Dockerfile` 使用 `g++ -O3 -static-libstdc++` 构建）。
不经 Docker 直接编译：

```bash
cd minivectordb && make && ./bin/mvd ./data
```

## 目录

| 路径 | 内容 |
| --- | --- |
| `minivectordb/` | 引擎源码（存储、SQL 解析、向量检索、事务、WAL） |
| `mysqltest/` | 官方公开用例（20 个） |
| `protocol_check/` | 官方 stdio 驱动与断言 |
| `docs/` | 设计记录、学习资料与正式用例失分点分析 |
| `agent/`、`conversation.json` | Agent 协作记录 |

## 实现要点

- 追加型 mmap 行存，`heap.dat` 只追加；`snapshot.bin` 在正常关闭或 WAL 超过阈值时压缩生成。
- WAL 记录 pk/offset/len，提交路径不 fsync：题面的崩溃模型只到“进程被强制终止”，页缓存不丢；
  正常关闭时 fsync 并落快照，重启先回放 WAL 再对外报告 ready。
- 向量检索为精确 L2 扫描（AVX2/FMA，4 累加器），按 (距离, pk) 稳定排序；
  单列等值过滤走追加型哈希桶并逐条复核。
- 事务为语句级 savepoint/undo，回滚恢复行、索引与可见集合。

## 本地验收

| 项 | 结果 |
| --- | --- |
| 官方公开用例（protocol_check） | 20/20 通过 |
| 崩溃注入（kill -9、恢复中再杀、失败插入原子性） | 9/9 通过 |
| 边界套件（类型、长度、事务可见性、重启） | 44/44 通过 |
| 60 万行大数据（重启 0.27 s） | 通过 |
| 10k × 128 维，100 批写入 + 1000 次 TopK | 写入 0.38–0.40 s，查询 0.18–0.19 ms，峰值 RSS 11.7 MB |
