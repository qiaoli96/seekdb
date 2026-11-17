#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// 向量距离与 TopK 的独立内核：不依赖执行器，可单测。
namespace vec {

// L2（欧氏）距离；输入为已存储的 float32。
double l2_distance(const float* a, const float* b, size_t dim);

// TopK 选择：dist 升序；dist 相同按 pk 升序。保留前 k 条。
struct Hit {
  double dist = 0;
  int64_t pk = 0;
  size_t row = 0;   // 调用方的行下标
};
void topk_select(std::vector<Hit>& hits, size_t k);

// 运行时 CPU 特性（供测试与日志使用）
const char* simd_backend();

}  // namespace vec
