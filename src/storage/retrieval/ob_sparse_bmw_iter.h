/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef OB_SPARSE_BMW_ITER_H_
#define OB_SPARSE_BMW_ITER_H_

#include "lib/container/ob_heap.h"
#include "ob_sparse_daat_iter.h"
#include "ob_sparse_retrieval_util.h"

namespace oceanbase
{
namespace storage
{
class ObTextRetrievalBlockMaxIter;

class ObTextRetrievalBlockMaxIter;

class ObTextRetrievalBlockMaxIter;

class ObSRBMWIterImpl : public ObSRDaaTIterImpl
{
public:
  ObSRBMWIterImpl();
  virtual ~ObSRBMWIterImpl() {}

  virtual void reuse(const bool switch_tablet = false) override;
  void reset();
  int init(
      ObSparseRetrievalMergeParam &iter_param,
      ObIArray<ObISRDaaTDimIter *> &dim_iters,
      ObIAllocator &iter_allocator,
      ObSRDaaTRelevanceCollector &relevance_collector);
  virtual int get_next_rows(const int64_t capacity, int64_t &count) override;
protected:
  virtual int process_collected_row(const ObDatum &id_datum, const double relevance) override;
  virtual int init_before_wand_process() { return OB_SUCCESS; }

  // Text fast path: every dimension is a text block-max cursor, so the per candidate calls bind
  // statically instead of through ObISRDimBlockMaxIter. fast_dim_iters_ stays null (and every
  // helper below falls back to the interface) as soon as one dimension is another implementation.
  int build_fast_dim_iters();
  ObTextRetrievalBlockMaxIter *fast_iter(const int64_t idx) const
  {
    return (nullptr == fast_dim_iters_) ? nullptr : fast_dim_iters_[idx];
  }
  int fast_advance_to(const int64_t idx, ObISRDimBlockMaxIter *iter, const ObDatum &id);
  int fast_advance_shallow(const int64_t idx, ObISRDimBlockMaxIter *iter, const ObDatum &id,
                           const bool inclusive);
  int fast_get_curr_id(const int64_t idx, ObISRDimBlockMaxIter *iter, const ObDatum *&id) const;
  int fast_get_curr_score(const int64_t idx, ObISRDimBlockMaxIter *iter, double &score) const;
  int fast_get_next_row(const int64_t idx, ObISRDimBlockMaxIter *iter);
  int fast_get_dim_max_score(const int64_t idx, ObISRDimBlockMaxIter *iter, double &score);
  int fast_get_curr_block_max_info(const int64_t idx, ObISRDimBlockMaxIter *iter,
                                   const ObMaxScoreTuple *&max_score_tuple);
  bool fast_in_shallow_status(const int64_t idx, ObISRDimBlockMaxIter *iter) const;



  int top_k_search();
  int build_top_k_heap();
  int next_pivot(int64_t &pivot_iter_idx);
  int next_pivot_range(int64_t &skip_range_cnt);
  int evaluate_pivot(const int64_t pivot_iter_idx);
  int evaluate_pivot_range(const int64_t pivot_iter_idx, bool &is_candidate);
  double get_top_k_threshold()
  {
    return top_k_heap_.empty() ? 0.0 : top_k_heap_.top().relevance_;
  }

private:
  ObISRDimBlockMaxIter *get_iter(const int64_t iter_idx) const
  {
    return static_cast<ObISRDimBlockMaxIter *>(dim_iters_->at(iter_idx));
  }
  int fill_merge_heap_with_shallow_dims(const ObDatum *last_range_border_id, const bool inclusive);
  int try_generate_next_range_from_merge_heap(
      bool &is_candidate_range,
      const ObDatum *&min_domain_id_with_pivot,
      const ObDatum *&max_domain_id_without_pivot);
  int advance_dim_iters_for_next_round(const ObDatum &target_id, const bool iter_end_available);
  void set_next_round_iter_end(const int64_t idx) { next_round_iter_idxes_[idx] = -1; }
  bool is_next_round_iter_end(const int64_t idx) const { return next_round_iter_idxes_[idx] == -1; }

protected:
  // Shared with the DaaT top-k engine, which fills the same heap and projects it the same way.
  int project_rows_from_top_k_heap(const int64_t capacity, int64_t &count);
  enum BMWStatus{
    FIND_NEXT_PIVOT,
    EVALUATE_PIVOT_RANGE,
    EVALUATE_PIVOT,
    FIND_NEXT_PIVOT_RANGE,
    FINISHED,
    MAX_STATUS,
  };
  struct TopKItem {
    TopKItem() : relevance_(0.0), cache_idx_(-1) {}
    TopKItem(const double &relevance, const int64_t &cache_idx) : relevance_(relevance), cache_idx_(cache_idx) {}
    ~TopKItem() = default;
    TO_STRING_KV(K_(relevance), K_(cache_idx));
    double relevance_;
    int64_t cache_idx_;
  };

  struct TopKItemCmp {
    bool operator()(const TopKItem &a, const TopKItem &b) const {
      return a.relevance_ > b.relevance_;
    }
    int get_error_code() { return common::OB_SUCCESS; }
  };
  typedef common::ObBinaryHeap<TopKItem, TopKItemCmp> TopKHeap;

  DISALLOW_COPY_AND_ASSIGN(ObSRBMWIterImpl);
  ObArenaAllocator allocator_;
  TopKItemCmp less_score_cmp_;
  // Maybe domain id not need to be cached within the topk heap since there would be top-n sort with datum store above
  TopKHeap top_k_heap_;
  int64_t top_k_count_;
  ObTextRetrievalBlockMaxIter **fast_dim_iters_;
  ObDomainIdCmp domain_id_cmp_;
  ObFixedArray<ObDocIdExt, ObIAllocator> id_cache_;
  BMWStatus status_;
  // Block level pruning is adaptive: once the range probe has rejected no pivot in
  // this scan it is pure overhead, and scoring a pivot is always sound.
  int64_t range_probe_cnt_;
  int64_t range_prune_cnt_;
  bool skip_range_check_;
};


} // namespace storage
} // namespace oceanbase
#endif
