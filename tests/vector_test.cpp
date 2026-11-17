// MiniVectorDB 向量内核单测：走 assert，Release 下 #undef NDEBUG 保证断言不被裁掉。
#undef NDEBUG

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <random>
#include <vector>

#include "vector.h"

// Release 会带 -DNDEBUG；上面的 #undef 必须让它失效，否则断言全被裁掉、单测等于空跑。
#ifdef NDEBUG
#error "vector_test 必须在启用 assert 的翻译单元里编译"
#endif

// 白盒测试钩子：vector.cpp 内部实现（不在 vector.h 契约内，仅供一致性比对）。
namespace vec {
namespace detail {
double l2_scalar(const float* a, const float* b, size_t dim);
bool cpu_has_avx2_fma();
}  // namespace detail
}  // namespace vec

namespace {

int g_checks = 0;

#define CHECK(cond)                                          \
  do {                                                       \
    ++g_checks;                                              \
    if (!(cond)) {                                           \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
      std::fflush(stdout);                                   \
      assert((cond));                                        \
      std::abort();                                          \
    }                                                        \
  } while (0)

double rel_err(double got, double ref) {
  const double scale = std::fabs(ref);
  if (scale < 1e-30) return std::fabs(got);
  return std::fabs(got - ref) / scale;
}

// 暴力参考解：按 (dist, pk) 全排序，完全并列时保留原始顺序。
std::vector<vec::Hit> brute_topk(std::vector<vec::Hit> hits, size_t k) {
  std::vector<size_t> idx(hits.size());
  std::iota(idx.begin(), idx.end(), size_t{0});
  std::sort(idx.begin(), idx.end(), [&hits](size_t x, size_t y) {
    const vec::Hit& a = hits[x];
    const vec::Hit& b = hits[y];
    if (a.dist != b.dist) return a.dist < b.dist;
    if (a.pk != b.pk) return a.pk < b.pk;
    return x < y;
  });
  if (k > idx.size()) k = idx.size();
  std::vector<vec::Hit> out;
  out.reserve(k);
  for (size_t i = 0; i < k; ++i) out.push_back(hits[idx[i]]);
  return out;
}

bool same_hits(const std::vector<vec::Hit>& a, const std::vector<vec::Hit>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i].dist != b[i].dist) return false;
    if (a[i].pk != b[i].pk) return false;
    if (a[i].row != b[i].row) return false;
  }
  return true;
}

bool sorted_per_rule(const std::vector<vec::Hit>& h) {
  for (size_t i = 1; i < h.size(); ++i) {
    const vec::Hit& p = h[i - 1];
    const vec::Hit& c = h[i];
    if (p.dist > c.dist) return false;
    if (p.dist == c.dist && p.pk > c.pk) return false;
  }
  return true;
}

// 参照实现：double 顺序累加后开方。
double ref_l2(const float* a, const float* b, size_t dim) {
  double s = 0.0;
  for (size_t i = 0; i < dim; ++i) {
    const double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
    s += d * d;
  }
  return std::sqrt(s);
}

void test_basic_known_values() {
  const float a2[2] = {3.0f, 4.0f};
  const float b2[2] = {0.0f, 0.0f};
  CHECK(vec::l2_distance(a2, b2, 2) == 5.0);
  std::printf("[PASS] dim=2  [3,4] vs [0,0] = %.17g (expect exactly 5)\n",
              vec::l2_distance(a2, b2, 2));

  const float c2[2] = {1.0f, 1.0f};
  const float d2[2] = {4.0f, 5.0f};
  CHECK(rel_err(vec::l2_distance(c2, d2, 2), 5.0) < 1e-12);

  // dim = 1
  const float a1[1] = {3.0f};
  const float b1[1] = {0.0f};
  CHECK(vec::l2_distance(a1, b1, 1) == 3.0);
  const float n1[1] = {-2.0f};
  const float m1[1] = {1.0f};
  CHECK(vec::l2_distance(n1, m1, 1) == 3.0);
  const float t1[1] = {1e-3f};
  const float z1[1] = {0.0f};
  CHECK(rel_err(vec::l2_distance(t1, z1, 1), 1e-3) < 1e-6);

  // 同向量 -> 0
  const float s3[3] = {1.5f, -2.5f, 7.25f};
  CHECK(vec::l2_distance(s3, s3, 3) == 0.0);

  // dim = 0
  CHECK(vec::l2_distance(a2, b2, 0) == 0.0);
  std::printf("[PASS] dim=1 / dim=0 / identical-vector edge cases\n");

  // dim = 128 已知序列：a[i]=0.5i, b[i]=0.25i  => diff=0.25i
  std::vector<float> av(128), bv(128);
  for (int i = 0; i < 128; ++i) {
    av[static_cast<size_t>(i)] = 0.5f * static_cast<float>(i);
    bv[static_cast<size_t>(i)] = 0.25f * static_cast<float>(i);
  }
  const double got = vec::l2_distance(av.data(), bv.data(), av.size());
  const double want = ref_l2(av.data(), bv.data(), av.size());
  CHECK(rel_err(got, want) < 1e-6);
  std::printf("[PASS] dim=128 sequential: %.17g vs ref %.17g (rel %.3g)\n", got, want,
              rel_err(got, want));
}

void test_simd_vs_scalar() {
  std::mt19937 rng(20261010u);
  std::uniform_real_distribution<float> uf(-1000.0f, 1000.0f);
  const size_t dims[] = {1,  2,  3,  7,   8,   9,   15,  16,  17,   31,    32,
                         33, 63, 64, 65,  127, 128, 129, 255, 256,  257,   1000,
                         1024, 4096};
  double worst = 0.0;
  for (size_t t = 0; t < 8; ++t) {
    for (size_t di = 0; di < sizeof(dims) / sizeof(dims[0]); ++di) {
      const size_t dim = dims[di];
      std::vector<float> a(dim), b(dim);
      for (size_t i = 0; i < dim; ++i) {
        a[i] = uf(rng);
        b[i] = uf(rng);
      }
      const double fast = vec::l2_distance(a.data(), b.data(), dim);
      const double slow = vec::detail::l2_scalar(a.data(), b.data(), dim);
      const double ref = ref_l2(a.data(), b.data(), dim);
      const double e1 = rel_err(fast, slow);
      const double e2 = rel_err(fast, ref);
      const double e3 = rel_err(slow, ref);
      if (e1 > worst) worst = e1;
      if (e2 > worst) worst = e2;
      if (e3 > worst) worst = e3;
      CHECK(e1 < 1e-6);
      CHECK(e2 < 1e-6);
      CHECK(e3 < 1e-6);
    }
  }
  std::printf("[PASS] simd(%s) vs scalar vs double-ref: worst rel err = %.3g (< 1e-6)\n",
              vec::simd_backend(), worst);
}

void test_topk_random_10000() {
  std::mt19937 rng(1337u);
  std::uniform_real_distribution<double> ud(0.0, 1000.0);
  std::uniform_int_distribution<int64_t> upk(0, 4999);

  std::vector<vec::Hit> hits;
  hits.reserve(10000);
  for (int i = 0; i < 10000; ++i) {
    vec::Hit h;
    h.dist = ud(rng);
    h.pk = upk(rng);
    h.row = static_cast<size_t>(i);
    hits.push_back(h);
  }

  std::vector<vec::Hit> cur = hits;
  vec::topk_select(cur, 10);
  const std::vector<vec::Hit> want = brute_topk(hits, 10);
  CHECK(cur.size() == 10);
  CHECK(same_hits(cur, want));
  CHECK(sorted_per_rule(cur));
  std::printf("[PASS] n=10000 k=10 heap path == brute force (exact), head: ");
  for (size_t i = 0; i < 3 && i < cur.size(); ++i)
    std::printf("(%.6f,%lld,row%zu) ", cur[i].dist, (long long)cur[i].pk, cur[i].row);
  std::printf("\n");
}

void test_topk_heavy_ties() {
  // dist 量化到 0.5 步长，pk 只取 0..49 -> 大量完全并列，检验并列与稳定性规则。
  std::mt19937 rng(7u);
  std::uniform_int_distribution<int> ud(0, 200);
  std::uniform_int_distribution<int64_t> upk(0, 49);
  std::vector<vec::Hit> hits;
  hits.reserve(10000);
  for (int i = 0; i < 10000; ++i) {
    vec::Hit h;
    h.dist = static_cast<double>(ud(rng)) * 0.5;
    h.pk = upk(rng);
    h.row = static_cast<size_t>(i);
    hits.push_back(h);
  }

  std::vector<vec::Hit> cur = hits;
  vec::topk_select(cur, 10);
  const std::vector<vec::Hit> want = brute_topk(hits, 10);
  CHECK(cur.size() == 10);
  CHECK(same_hits(cur, want));
  CHECK(sorted_per_rule(cur));

  // 并列 dist 必须按 pk 升序
  std::vector<vec::Hit> tied;
  const int64_t pks[] = {9, 3, 7, 1, 5};
  for (int i = 0; i < 5; ++i) {
    vec::Hit h;
    h.dist = 2.0;
    h.pk = pks[i];
    h.row = static_cast<size_t>(i);
    tied.push_back(h);
  }
  vec::topk_select(tied, 5);
  const int64_t want_pk[] = {1, 3, 5, 7, 9};
  for (int i = 0; i < 5; ++i) CHECK(tied[static_cast<size_t>(i)].pk == want_pk[i]);
  std::printf("[PASS] heavy ties (n=10000,k=10) == brute force; equal dist -> pk asc ");
  for (size_t i = 0; i < 5; ++i) std::printf("%lld ", (long long)tied[i].pk);
  std::printf("\n");

  // dist 与 pk 全等、仅 row 不同：必须保持入参相对顺序
  std::vector<vec::Hit> dup;
  for (int i = 0; i < 100; ++i) {
    vec::Hit h;
    h.dist = 1.0;
    h.pk = 42;
    h.row = static_cast<size_t>(1000 + i);
    dup.push_back(h);
  }
  std::vector<vec::Hit> dup_sel = dup;
  vec::topk_select(dup_sel, 10);
  for (int i = 0; i < 10; ++i) CHECK(dup_sel[static_cast<size_t>(i)].row == 1000u + static_cast<size_t>(i));
  std::vector<vec::Hit> dup_all = dup;
  vec::topk_select(dup_all, 1000);  // k >= size 分支
  CHECK(dup_all.size() == 100);
  for (int i = 0; i < 100; ++i) CHECK(dup_all[static_cast<size_t>(i)].row == 1000u + static_cast<size_t>(i));
  std::printf("[PASS] fully-tied (dist,pk) duplicates keep input order (k=10 and k>=size)\n");
}

void test_topk_branches_and_edges() {
  std::mt19937 rng(99u);
  std::uniform_real_distribution<double> ud(0.0, 100.0);
  std::vector<vec::Hit> hits;
  hits.reserve(10000);
  for (int i = 0; i < 10000; ++i) {
    vec::Hit h;
    h.dist = ud(rng);
    h.pk = static_cast<int64_t>(i % 977);
    h.row = static_cast<size_t>(i);
    hits.push_back(h);
  }

  const size_t ks[] = {1023, 1024, 1025, 2000, 5000, 9999};
  for (size_t ki = 0; ki < sizeof(ks) / sizeof(ks[0]); ++ki) {
    std::vector<vec::Hit> cur = hits;
    vec::topk_select(cur, ks[ki]);
    const std::vector<vec::Hit> want = brute_topk(hits, ks[ki]);
    CHECK(cur.size() == ks[ki]);
    CHECK(same_hits(cur, want));
    CHECK(sorted_per_rule(cur));
  }
  std::printf("[PASS] k in {1023,1024,1025,2000,5000,9999}: heap/nth_element both match brute force\n");

  // k = 0 -> 清空
  std::vector<vec::Hit> z = hits;
  vec::topk_select(z, 0);
  CHECK(z.empty());
  // k >= size -> 全保留并排序
  std::vector<vec::Hit> big = hits;
  vec::topk_select(big, 20000);
  CHECK(big.size() == hits.size());
  CHECK(sorted_per_rule(big));
  CHECK(same_hits(big, brute_topk(hits, hits.size())));
  // k == size
  std::vector<vec::Hit> eq = hits;
  vec::topk_select(eq, hits.size());
  CHECK(eq.size() == hits.size());
  CHECK(sorted_per_rule(eq));
  // 空输入
  std::vector<vec::Hit> empty;
  vec::topk_select(empty, 5);
  CHECK(empty.empty());
  std::vector<vec::Hit> empty0;
  vec::topk_select(empty0, 0);
  CHECK(empty0.empty());
  std::printf("[PASS] k=0 -> clear; k>=size -> keep-all sorted; k=size; empty input\n");
}

void test_end_to_end_distances() {
  // 真实链路：10000 条 128 维随机向量，查询取 k=10，距离由 SIMD 计算。
  std::mt19937 rng(424242u);
  std::uniform_real_distribution<float> uf(-1.0f, 1.0f);
  const size_t dim = 128;
  std::vector<float> flat(10000 * dim);
  std::vector<vec::Hit> hits;
  hits.reserve(10000);
  std::vector<float> q(dim);
  for (size_t i = 0; i < dim; ++i) q[i] = uf(rng);
  for (size_t r = 0; r < 10000; ++r) {
    float* p = flat.data() + r * dim;
    for (size_t i = 0; i < dim; ++i) p[i] = uf(rng);
    vec::Hit h;
    h.dist = vec::l2_distance(p, q.data(), dim);
    h.pk = static_cast<int64_t>(r);
    h.row = r;
    hits.push_back(h);
  }
  std::vector<vec::Hit> cur = hits;
  vec::topk_select(cur, 10);
  const std::vector<vec::Hit> want = brute_topk(hits, 10);
  CHECK(cur.size() == 10);
  CHECK(same_hits(cur, want));
  CHECK(sorted_per_rule(cur));
  std::printf("[PASS] 10000x128d query k=10 (SIMD distances) == brute force; best=%zu dist=%.6f\n",
              cur[0].row, cur[0].dist);
}

}  // namespace

int main() {
  std::printf("simd_backend() = %s\n", vec::simd_backend());
  std::printf("cpuid avx2+fma = %d\n", vec::detail::cpu_has_avx2_fma() ? 1 : 0);

  test_basic_known_values();
  test_simd_vs_scalar();
  test_topk_random_10000();
  test_topk_heavy_ties();
  test_topk_branches_and_edges();
  test_end_to_end_distances();

  std::printf("ALL TESTS PASSED (%d checks)\n", g_checks);
  return 0;
}
