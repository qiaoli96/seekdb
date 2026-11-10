// MiniVectorDB 向量内核：L2 距离（运行时 CPUID 派发）与 TopK 选择。
#include "vector.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <utility>
#include <vector>

#if defined(__x86_64__) || defined(__i386__)
#define MVDB_X86 1
#include <immintrin.h>
#endif

namespace vec {
namespace detail {

// 标量参考实现：double 累加，作为一致性与精度基线。
double l2_scalar(const float* a, const float* b, size_t dim) {
  double sum = 0.0;
  for (size_t i = 0; i < dim; ++i) {
    const double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
    sum += d * d;
  }
  return std::sqrt(sum);
}

#if MVDB_X86
__attribute__((target("avx2,fma"))) double l2_avx2(const float* a, const float* b,
                                                   size_t dim) {
  size_t i = 0;
  __m256 acc0 = _mm256_setzero_ps();
  __m256 acc1 = _mm256_setzero_ps();
  __m256 acc2 = _mm256_setzero_ps();
  __m256 acc3 = _mm256_setzero_ps();

  for (; i + 32 <= dim; i += 32) {
    const __m256 a0 = _mm256_loadu_ps(a + i);
    const __m256 b0 = _mm256_loadu_ps(b + i);
    const __m256 a1 = _mm256_loadu_ps(a + i + 8);
    const __m256 b1 = _mm256_loadu_ps(b + i + 8);
    const __m256 a2 = _mm256_loadu_ps(a + i + 16);
    const __m256 b2 = _mm256_loadu_ps(b + i + 16);
    const __m256 a3 = _mm256_loadu_ps(a + i + 24);
    const __m256 b3 = _mm256_loadu_ps(b + i + 24);
    const __m256 d0 = _mm256_sub_ps(a0, b0);
    const __m256 d1 = _mm256_sub_ps(a1, b1);
    const __m256 d2 = _mm256_sub_ps(a2, b2);
    const __m256 d3 = _mm256_sub_ps(a3, b3);
    acc0 = _mm256_fmadd_ps(d0, d0, acc0);
    acc1 = _mm256_fmadd_ps(d1, d1, acc1);
    acc2 = _mm256_fmadd_ps(d2, d2, acc2);
    acc3 = _mm256_fmadd_ps(d3, d3, acc3);
  }
  for (; i + 8 <= dim; i += 8) {
    const __m256 d = _mm256_sub_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i));
    acc0 = _mm256_fmadd_ps(d, d, acc0);
  }

  // 水平归约：float 累加后转 double 再开方，误差远低于 1e-6。
  const __m256 acc = _mm256_add_ps(_mm256_add_ps(acc0, acc1), _mm256_add_ps(acc2, acc3));
  const __m128 lo = _mm256_castps256_ps128(acc);
  const __m128 hi = _mm256_extractf128_ps(acc, 1);
  const __m128 s = _mm_add_ps(lo, hi);
  const __m128d s01 = _mm_cvtps_pd(s);
  const __m128d s23 = _mm_cvtps_pd(_mm_movehl_ps(s, s));
  const __m128d t = _mm_add_pd(s01, s23);
  double sum = _mm_cvtsd_f64(_mm_hadd_pd(t, t));

  for (; i < dim; ++i) {
    const double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
    sum += d * d;
  }
  return std::sqrt(sum);
}
#endif  // MVDB_X86

// CPUID 探针：仅在运行时判定，不做全局 -mavx2 编译。
bool cpu_has_avx2_fma() {
#if MVDB_X86
  __builtin_cpu_init();
  return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
#else
  return false;
#endif
}

}  // namespace detail

namespace {

using distance_fn = double (*)(const float*, const float*, size_t);

struct Backend {
  distance_fn fn;
  const char* name;
};

Backend select_backend() {
#if MVDB_X86
  if (detail::cpu_has_avx2_fma()) {
    return Backend{&detail::l2_avx2, "avx2"};
  }
#endif
  return Backend{&detail::l2_scalar, "scalar"};
}

const Backend& backend() {
  static const Backend b = select_backend();
  return b;
}

// TopK 键序：dist 升序，dist 相等按 pk 升序。
bool hit_less(const Hit& a, const Hit& b) {
  if (a.dist != b.dist) return a.dist < b.dist;
  return a.pk < b.pk;
}

// 对原下标排序的比较器；末位用原始位置保证完全并列（dist、pk 均相同）时稳定。
struct HitIndexLess {
  const std::vector<Hit>* hits;
  bool operator()(size_t x, size_t y) const {
    const Hit& a = (*hits)[x];
    const Hit& b = (*hits)[y];
    if (a.dist != b.dist) return a.dist < b.dist;
    if (a.pk != b.pk) return a.pk < b.pk;
    return x < y;
  }
};

}  // namespace

double l2_distance(const float* a, const float* b, size_t dim) {
  return backend().fn(a, b, dim);
}

const char* simd_backend() { return backend().name; }

void topk_select(std::vector<Hit>& hits, size_t k) {
  const size_t n = hits.size();
  if (k == 0) {
    hits.clear();
    return;
  }
  if (k >= n) {
    // 全保留：stable_sort 保证 dist/pk 完全并列时维持入参顺序。
    std::stable_sort(hits.begin(), hits.end(), hit_less);
    return;
  }

  constexpr size_t kHeapLimit = 1024;
  std::vector<size_t> pos(n);
  std::iota(pos.begin(), pos.end(), size_t{0});
  const HitIndexLess less{&hits};
  const auto kk = static_cast<std::ptrdiff_t>(k);

  if (k <= kHeapLimit) {
    // 大顶堆只压 k 个元素，O(n log k)，不全排序。
    std::make_heap(pos.begin(), pos.begin() + kk, less);
    for (size_t i = k; i < n; ++i) {
      if (less(i, pos[0])) {
        std::pop_heap(pos.begin(), pos.begin() + kk, less);
        pos[k - 1] = i;
        std::push_heap(pos.begin(), pos.begin() + kk, less);
      }
    }
    pos.resize(k);
  } else {
    // k 较大时堆无优势：nth_element 均摊 O(n)。
    std::nth_element(pos.begin(), pos.begin() + kk, pos.end(), less);
    pos.resize(k);
  }

  std::sort(pos.begin(), pos.end(), less);
  std::vector<Hit> out(k);
  for (size_t i = 0; i < k; ++i) out[i] = hits[pos[i]];
  for (size_t i = 0; i < k; ++i) hits[i] = out[i];
  hits.resize(k);
}

}  // namespace vec
