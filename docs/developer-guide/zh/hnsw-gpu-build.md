# HNSW 索引构建加速（AMD GPU / hipVS）

本文说明 `2026-competition` 分支上新增的 HNSW 索引构建加速链路，以及如何在带 AMD 显卡的机器上构建与实测。
英文版见 `../en/hnsw-gpu-build.md`。

## 1. 背景

seekdb 的普通 `TYPE=hnsw` 向量索引，在 DDL 构建阶段是**逐行**调用 vsag 的 `Add()` 建图的
（`src/observer/vector_index/ob_plugin_vector_index_adaptor.cpp` 的 `add_snap_index()`）。
逐行插入式建图远慢于一次性批量建图。

本次改动分两层，且是叠加关系：

1. **批量建图**：普通 HNSW 改为先累积向量、再一次性调用 vsag 的批量 `Build()`
   —— **纯 CPU 收益，没有 GPU 也生效**；
2. **GPU 建图**：批量构建时优先用 AMD hipVS 在 Radeon GPU 上建 kNN 图，
   再导出成 hnswlib 二进制，由 vsag 直接加载 —— 第 1 层是第 2 层的入口。

## 2. GPU 路径

```
cuvsCagraBuild      在 GPU 上建 kNN 图（最耗时的部分）
      ↓
cuvsHnswFromCagra   转成多层 HNSW（hierarchy=CPU，保持 hnswlib 布局兼容）
      ↓
cuvsHnswSerialize   导出 hnswlib 二进制
      ↓
vsag::BinarySet[HNSW_DATA] = 导出的字节
      ↓
vsag::Index::Deserialize()   ← 落盘格式、检索路径完全不变
```

**关键事实**：vsag 内嵌的 hnswlib 与上游 hnswlib 的序列化格式逐字段一致，
所以 hipVS 导出的字节可以被 vsag 直接加载，**不需要任何格式转换**。

## 3. 启用方式

GPU 路径**默认不编译**，普通构建完全不依赖 ROCm：

```bash
bazel build -c opt //src/observer:observer \
  --copt=-DSEEKDB_WITH_HIPVS \
  --copt=-I$HIPVS_PREFIX/include --copt=-I$ROCM/include \
  --linkopt=-L$HIPVS_PREFIX/lib -Wl,-rpath,$HIPVS_PREFIX/lib \
  --linkopt=-L$ROCM/lib -Wl,-rpath,$ROCM/lib --linkopt=-lhipvs
```

运行时行为：

- 只有**纯 float32 的普通 HNSW**（无量化、无 extra_info、度量是 l2/ip、vid 是 0..N-1 连续）才走 GPU；
- 其余情况，或 GPU 构建/导入/校验任一步失败，**自动回落到主机构建**，结果与改造前一致；
- 导入后校验索引元素数与输入行数一致，不一致即丢弃并回落。

## 4. 在带 AMD 显卡的机器上实测

### 4.1 准备 ROCm

```bash
curl -s https://repo.radeon.com/rocm/apt/rocm.gpg.key | sudo gpg --dearmor -o /usr/share/keyrings/rocm.gpg
echo "deb [arch=amd64 signed-by=/usr/share/keyrings/rocm.gpg] https://repo.radeon.com/rocm/apt/<版本> noble main" \
  | sudo tee /etc/apt/sources.list.d/rocm.list
sudo apt update && sudo apt install -y hip-runtime-amd hip-dev hipcc
hipcc --version
```

### 4.2 构建 hipVS

```bash
cd seekdb
HIPVS_ARCH=gfx1100 bash scripts/build_hipvs.sh ../hipvs-install
```

`HIPVS_ARCH` 换成你显卡对应的架构（RX 7900 = `gfx1100`，MI300 = `gfx942`）。

### 4.3 编两个 seekdb（基线 + GPU）

```bash
# 基线：主机构建
bazel build -c opt //src/observer:observer

# GPU 构建
HIPVS_PREFIX=$PWD/../hipvs-install ROCM=/opt/rocm \
bazel build -c opt //src/observer:observer \
  --copt=-DSEEKDB_WITH_HIPVS \
  --copt=-I$HIPVS_PREFIX/include --copt=-I$ROCM/include \
  --linkopt=-L$HIPVS_PREFIX/lib -Wl,-rpath,$HIPVS_PREFIX/lib \
  --linkopt=-L$ROCM/lib -Wl,-rpath,$ROCM/lib --linkopt=-lhipvs
```

### 4.4 跑基准

```bash
ROWS=200000 DIM=256 scripts/bench_hnsw_build.sh "host build"
ROWS=200000 DIM=256 scripts/bench_hnsw_build.sh "gpu build (hipVS)"
```

脚本会建表、灌数据、计时 `CREATE VECTOR INDEX`，打印：

```
label                : gpu build (hipVS)
HNSW build time (s)  : 123.45
vectors per second   : 1620.30
```

两个数字一减就是加速比。日志里出现这两行即说明走的确实是 GPU 路径：

```
[OBVSAG] hnsw index built on AMD GPU by hipVS
```

## 5. 代码位置

| 文件 | 作用 |
|---|---|
| `src/oblib/lib/vector/ob_hipvs_adaptor.h/.cpp` | hipVS C API 封装（设备探测、GPU 建图、导出 hnswlib） |
| `src/oblib/lib/vector/ob_vsag_adaptor.cpp` | `build_index_gpu()`：导入 vsag、校验、回落 |
| `src/observer/vector_index/ob_plugin_vector_index_adaptor.cpp` | 普通 HNSW 改为批量建图 |
| `scripts/build_hipvs.sh` | 构建 hipVS |
| `scripts/gen_vec_data.py`、`scripts/bench_hnsw_build.sh` | 基准数据生成与计时 |
