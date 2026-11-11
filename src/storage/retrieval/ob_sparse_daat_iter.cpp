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

#define USING_LOG_PREFIX STORAGE

#include "ob_sparse_daat_iter.h"

namespace oceanbase
{
namespace storage
{

ObSRMergeCmp::ObSRMergeCmp()
  : cmp_func_(nullptr),
    iter_ids_(nullptr),
    is_inited_(false),
    fast_cmp_(false)
{
}

int ObSRMergeCmp::init(
    sql::ObDatumMeta id_meta,
    const ObFixedArray<const ObDatum *, ObIAllocator> *iter_ids)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(iter_ids)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    iter_ids_ = iter_ids;
    common::ObDatumBasicFuncs *basic_funcs = ObDatumFuncs::get_basic_func(id_meta.type_, id_meta.cs_type_);
    cmp_func_ = basic_funcs->null_first_cmp_;
    if (OB_ISNULL(cmp_func_)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      fast_cmp_ = ob_sparse_is_binary_string_id(id_meta.type_, id_meta.cs_type_);
      is_inited_ = true;
    }
  }
  return ret;
}

int ObSRMergeCmp::cmp_datum(const ObDatum &lhs_id, const ObDatum &rhs_id, int64_t &cmp_ret)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else {
    int tmp_ret = 0;
    if (fast_cmp_ && ob_sparse_is_memcmpable(lhs_id, rhs_id)) {
      if (OB_FAIL(ob_sparse_memcmp_id(lhs_id, rhs_id, tmp_ret))) {
      } else {
        cmp_ret = tmp_ret;
      }
    } else if (OB_FAIL(cmp_func_(lhs_id, rhs_id, tmp_ret, nullptr))) {
    } else {
      cmp_ret = tmp_ret;
    }
  }
  return ret;
}

int ObSRMergeCmp::cmp(
    const ObSRMergeItem &l,
    const ObSRMergeItem &r,
    int64_t &cmp_ret)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(cmp_datum(get_id_datum(l.iter_idx_), get_id_datum(r.iter_idx_), cmp_ret))) {
  }
  return ret;
}

ObSRDaaTIterImpl::ObSRDaaTIterImpl()
  : ObISparseRetrievalMergeIter(),
    iter_allocator_(nullptr),
    iter_param_(nullptr),
    dim_iters_(nullptr),
    merge_cmp_(),
    merge_heap_(nullptr),
    relevance_collector_(nullptr),
    iter_domain_ids_(),
    buffered_domain_ids_(),
    buffered_relevances_(),
    next_round_iter_idxes_(),
    next_round_cnt_(0),
    set_datum_func_(nullptr),
    fast_item_cnt_(0),
    use_fast_merge_(false),
    use_inline_match_filter_(false),
    use_batch_merge_(false),
    batch_inited_(false),
    batch_active_cnt_(0)
{
  batch_reset_state();
}

int ObSRDaaTIterImpl::init(
    ObSparseRetrievalMergeParam &iter_param,
    ObIArray<ObISRDaaTDimIter *> &dim_iters,
    ObIAllocator &iter_allocator,
    ObSRDaaTRelevanceCollector &relevance_collector)
{
  int ret = OB_SUCCESS;
  if (IS_INIT) {
    ret = OB_INIT_TWICE;
  } else if (OB_UNLIKELY(iter_param.max_batch_size_ <= 0)) {
      ret = OB_ERR_UNEXPECTED;
  } else {
    iter_allocator_ = &iter_allocator;
    iter_param_ = &iter_param;
    dim_iters_ = &dim_iters;
    relevance_collector_ = &relevance_collector;
    const int64_t max_batch_size = OB_MAX(iter_param_->eval_ctx_->max_batch_size_, iter_param_->max_batch_size_);
    if (iter_param_->id_proj_expr_->datum_meta_.type_ == common::ObUInt64Type) {
      set_datum_func_ = ObISparseRetrievalMergeIter::set_datum_int;
    } else {
      set_datum_func_ = ObISparseRetrievalMergeIter::set_datum_shallow;
    }
    if (OB_UNLIKELY(dim_iters.count() == 0)) {
    } else if (OB_NOT_NULL(iter_param_->dim_weights_) && dim_iters.count() != iter_param_->dim_weights_->count()) {
      ret = OB_ERR_UNEXPECTED;
    } else if (FALSE_IT(iter_domain_ids_.set_allocator(iter_allocator_))) {
    } else if (OB_FAIL(iter_domain_ids_.init(dim_iters.count()))) {
    } else if (OB_FAIL(iter_domain_ids_.prepare_allocate(dim_iters.count()))) {
    } else if (FALSE_IT(next_round_iter_idxes_.set_allocator(iter_allocator_))) {
    } else if (OB_FAIL(next_round_iter_idxes_.init(dim_iters.count()))) {
    } else if (OB_FAIL(next_round_iter_idxes_.prepare_allocate(dim_iters.count()))) {
    } else if (FALSE_IT(buffered_domain_ids_.set_allocator(iter_allocator_))) {
    } else if (OB_FAIL(buffered_domain_ids_.init(max_batch_size))) {
    } else if (OB_FAIL(buffered_domain_ids_.prepare_allocate(max_batch_size))) {
    } else if (FALSE_IT(buffered_relevances_.set_allocator(iter_allocator_))) {
    } else if (OB_FAIL(buffered_relevances_.init(max_batch_size))) {
    } else if (OB_FAIL(buffered_relevances_.prepare_allocate(max_batch_size))) {
    } else if (OB_FAIL(merge_cmp_.init(iter_param_->id_proj_expr_->datum_meta_, &iter_domain_ids_))) {
    } else if (OB_FAIL(init_merge_heap(dim_iters_->count()))) {
    } else {
      for (int64_t i = 0; i < dim_iters.count(); ++i) {
        next_round_iter_idxes_[i] = i;
      }
      // The batch shape needs every dimension to hand out the rows it already holds;
      // anything else keeps the row at a time paths untouched.
      use_batch_merge_ = (dim_iters.count() > 0 && dim_iters.count() <= FAST_MERGE_MAX_DIMS);
      for (int64_t i = 0; use_batch_merge_ && i < dim_iters.count(); ++i) {
        use_batch_merge_ = dim_iters.at(i)->support_batch_view();
      }
      LOG_INFO("Q2ENGAGE sparse merge shape", "dims", dim_iters.count(),
               "batch_view", use_batch_merge_, "id_is_string",
               ob_is_string_type(iter_param_->id_proj_expr_->datum_meta_.type_));
    }

    if (OB_SUCC(ret)) {
      next_round_cnt_ = dim_iters.count();
      input_row_cnt_ = 0;
      output_row_cnt_ = 0;
      is_inited_ = true;
    }
  }
  return ret;
}

void ObSRDaaTIterImpl::reset()
{
  if (OB_NOT_NULL(merge_heap_)) {
    merge_heap_->~ObSRMergeHeap();
    merge_heap_ = nullptr;
  }
  if (OB_NOT_NULL(relevance_collector_)) {
    relevance_collector_->reset();
    relevance_collector_ = nullptr;
  }
  iter_domain_ids_.reset();
  buffered_domain_ids_.reset();
  buffered_relevances_.reset();
  next_round_iter_idxes_.reset();
  next_round_cnt_ = 0;
  fast_item_cnt_ = 0;
  use_fast_merge_ = false;
  use_inline_match_filter_ = false;
  batch_reset_state();
  input_row_cnt_ = 0;
  output_row_cnt_ = 0;
  is_inited_ = false;
}

void ObSRDaaTIterImpl::reuse(const bool switch_tablet)
{
  if (OB_NOT_NULL(dim_iters_)) {
    if (OB_NOT_NULL(merge_heap_)) {
      merge_heap_->reuse();
      merge_heap_->open(dim_iters_->count());
    }
    next_round_cnt_ = dim_iters_->count();
    for (int64_t i = 0; i < next_round_cnt_; ++i) {
      next_round_iter_idxes_[i] = i;
    }
    fast_item_cnt_ = 0;
    batch_reset_state();
  }
  if (OB_NOT_NULL(relevance_collector_)) {
    relevance_collector_->reuse();
  }
  input_row_cnt_ = 0;
  output_row_cnt_ = 0;
}

  int ObSRDaaTIterImpl::get_query_max_score(double &score)
  {
    int ret = OB_SUCCESS;
    score = 0.0;
    for (int64_t i = 0; OB_SUCC(ret) && i < dim_iters_->count(); ++i) {
      ObISRDaaTDimIter *dim_iter = dim_iters_->at(i);
      double dim_score = 0.0;
      if (OB_ISNULL(dim_iter) || OB_ISNULL(iter_param_->dim_weights_)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(dim_iter->get_dim_max_score(dim_score))) {
      } else {
        score += dim_score * iter_param_->field_boost_ * iter_param_->dim_weights_->at(i);
      }
    }

    return ret;
  }

int ObSRDaaTIterImpl::get_next_row()
{
  int ret = OB_SUCCESS;
  int64_t count = 0;
  if (OB_FAIL(get_next_rows(1, count))) {
    if (OB_UNLIKELY(OB_ITER_END != ret)) {
    } else if (OB_UNLIKELY(count != 0)) {
      ret = OB_ERR_UNEXPECTED;
    }
  } else if (OB_UNLIKELY(count != 1)) {
    ret = OB_ERR_UNEXPECTED;
  }
  return ret;
}

int ObSRDaaTIterImpl::get_next_rows(const int64_t capacity, int64_t &count)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (OB_UNLIKELY(0 == capacity)) {
    count = 0;
  } else if (0 == dim_iters_->count()) {
    ret = OB_ITER_END;
  } else if (iter_param_->limit_param_->is_valid() && output_row_cnt_ >= iter_param_->limit_param_->limit_) {
    ret = OB_ITER_END;
  } else if (OB_FAIL(pre_process())) {
    if (OB_UNLIKELY(OB_ITER_END != ret)) {
    } else {
      count = 0;
    }
  } else {
    count = 0;
    const int64_t real_capacity = MIN(capacity, iter_param_->max_batch_size_);
    while (OB_SUCC(ret) && count < real_capacity) {
      const int merge_ret = (use_fast_merge_ && use_batch_merge_)
          ? batch_do_one_merge_round(count)
          : (use_fast_merge_ ? fast_do_one_merge_round(count) : do_one_merge_round(count));
      if (OB_FAIL(merge_ret)) {
        if (OB_UNLIKELY(OB_ITER_END != merge_ret)) {
        }
      }
    }

    if (OB_SUCC(ret) || OB_ITER_END == ret) {
      if (count > 0 && OB_FAIL(project_results(count))) {
      }
    }

  }
  return ret;
}

int ObSRDaaTIterImpl::pre_process()
{
  return OB_NOT_IMPLEMENT;
}

int ObSRDaaTIterImpl::do_one_merge_round(int64_t &count)
{
  int ret = OB_SUCCESS;
  bool need_project = true;
  double relevance = 0.0;
  const ObDatum *id_datum = nullptr;
  if (OB_FAIL(fill_merge_heap())) {
    if (OB_UNLIKELY(OB_ITER_END != ret)) {
    }
  } else if (OB_FAIL(collect_dims_by_id(id_datum, relevance, need_project))) {
  } else if (need_project && OB_FAIL(filter_on_demand(count, relevance, need_project))) {
  } else if (need_project && OB_FAIL(cache_result(count, *id_datum, relevance))) {
    if (OB_ITER_END != ret) {
    }
  }
  return ret;
}

int ObSRDaaTIterImpl::fill_merge_heap()
{
  int ret = OB_SUCCESS;
  ObSRMergeItem item;
  for (int64_t i = 0; OB_SUCC(ret) && i < next_round_cnt_; ++i) {
    const int64_t iter_idx = next_round_iter_idxes_[i];
    ObISRDaaTDimIter *dim_iter = nullptr;
    if (OB_ISNULL(dim_iter = dim_iters_->at(iter_idx))) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(dim_iter->get_next_row())) {
      if (OB_UNLIKELY(OB_ITER_END != ret)) {
        LOG_WARN("fail to try load next batch dimension data", K(ret));\
      } else {
        ret = OB_SUCCESS;
      }
    } else if (OB_FAIL(dim_iter->get_curr_score(item.relevance_))) {
    } else if (OB_NOT_NULL(iter_param_->dim_weights_) && FALSE_IT(item.relevance_ = item.relevance_ * iter_param_->field_boost_ * iter_param_->dim_weights_->at(iter_idx))) {
    } else if (OB_FAIL(dim_iter->get_curr_id(iter_domain_ids_[iter_idx]))) {
    } else if (FALSE_IT(item.iter_idx_ = iter_idx)) {
    } else if (OB_FAIL(merge_heap_->push(item))) {
    } else {
      LOG_DEBUG("push item to merge heap", K(ret), K(i), K(iter_idx), K(next_round_cnt_), K(item), K(iter_domain_ids_[iter_idx]));
    }
  }

  if (OB_FAIL(ret)) {
  } else if (merge_heap_->empty()) {
    ret = OB_ITER_END;
  } else if (0 != next_round_cnt_ && OB_FAIL(merge_heap_->rebuild())) {
  } else {
    next_round_cnt_ = 0;
  }
  return ret;
}

int ObSRDaaTIterImpl::collect_dims_by_id(const ObDatum *&id_datum, double &relevance, bool &got_valid_id)
{
  int ret = OB_SUCCESS;

  const ObSRMergeItem *top_item = nullptr;
  bool curr_doc_end = false;
  int64_t iter_idx = 0;
  relevance = 0.0;
  got_valid_id = false;

  while (OB_SUCC(ret) && !merge_heap_->empty() && !curr_doc_end) {
    if (merge_heap_->is_unique_champion()) {
      curr_doc_end = true;
    }
    if (OB_FAIL(merge_heap_->top(top_item))) {
    } else if (OB_FAIL(relevance_collector_->collect_one_dim(top_item->iter_idx_, top_item->relevance_))) {
    } else if (FALSE_IT(iter_idx = top_item->iter_idx_)) {
    } else if (OB_FAIL(merge_heap_->pop())) {
    } else {
      next_round_iter_idxes_[next_round_cnt_++] = iter_idx;
    }
  }

  if (OB_SUCC(ret)) {
    id_datum = iter_domain_ids_[iter_idx];
    if (OB_ISNULL(id_datum)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(relevance_collector_->get_result(relevance, got_valid_id))) {
    } else if (got_valid_id && OB_FAIL(process_collected_row(*id_datum, relevance))) {
    }
  }

  return ret;
}

int ObSRDaaTIterImpl::process_collected_row(const ObDatum &id_datum, const double relevance)
{
  return OB_SUCCESS;
}

// The match filter pushed into the text retrieval scan is, for the ordinary
// "MATCH(...) AGAINST(...)" predicate, the bool coercion operator applied to the very
// relevance value this iterator already computed (ObExprBool, T_OP_BOOL). Both sides of
// that identity have to be checked, because ObRawExprUtils::try_create_bool_expr may
// hand back the source expression unchanged instead of wrapping it:
//   * the runtime expression is T_OP_BOOL with exactly one argument,
//   * that argument is the same ObExpr object the relevance is written into
//     (relevance_proj_expr_), so the filter cannot depend on any other datum,
//   * the argument is a double, i.e. ObExprBool dispatches to its double path.
// Anything else (boolean mode, query expansion, a predicate over more than the score,
// a non-double score) keeps the generic path.
bool ObSRDaaTIterImpl::is_canonical_match_filter() const
{
  bool bret = false;
  const ObExpr *filter_expr = nullptr;
  const ObExpr *relevance_proj_expr = nullptr;
  if (OB_NOT_NULL(iter_param_)) {
    filter_expr = iter_param_->filter_expr_;
    relevance_proj_expr = iter_param_->relevance_proj_expr_;
  }
  if (OB_NOT_NULL(filter_expr) && OB_NOT_NULL(relevance_proj_expr)
      && T_OP_BOOL == filter_expr->type_
      && 1 == filter_expr->arg_cnt_
      && OB_NOT_NULL(filter_expr->args_)
      && filter_expr->args_[0] == relevance_proj_expr
      && (common::ObDoubleType == relevance_proj_expr->datum_meta_.type_
          || common::ObUDoubleType == relevance_proj_expr->datum_meta_.type_)) {
    bret = true;
  }
  return bret;
}

int ObSRDaaTIterImpl::filter_on_demand(const int64_t count, const double relevance, bool &need_project)
{
  int ret = OB_SUCCESS;
  ObExpr *filter_expr = iter_param_->filter_expr_;
  ObExpr *relevance_proj_expr = iter_param_->relevance_proj_expr_;
  ObEvalCtx *eval_ctx = iter_param_->eval_ctx_;

  if (!iter_param_->need_filter()) {
    need_project = true;
  } else if (OB_UNLIKELY(use_inline_match_filter_)) {
    // ObExprBool's double path is res = (0 == child) ? 0 : 1, with NULL propagating as
    // NULL (see the CHECK_IS_TRUE_FUNC_NAME(double_type) expansion). The child here is
    // the datum written just below, which the iterator itself filled from a plain double,
    // so it is never NULL and the predicate reduces to the comparison below -- exact for
    // every value, including -0.0 and NaN (NaN != 0 keeps the row exactly as the generic
    // eval does). is_valid / should_match_ filtering happens in collect_dims_by_id
    // before this point and is untouched; offset and limit stay in cache_result.
    if (eval_ctx->is_vectorized()) {
      if (OB_UNLIKELY(count >= eval_ctx->max_batch_size_)) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        if (!iter_param_->need_project_relevance()) {
          // nothing downstream rewrites the relevance datum, keep the generic write
          ObEvalCtx::BatchInfoScopeGuard guard(*eval_ctx);
          guard.set_batch_idx(count);
          ObDatum &relevance_proj_datum = relevance_proj_expr->locate_datum_for_write(*eval_ctx);
          relevance_proj_datum.set_double(relevance);
          relevance_proj_expr->get_evaluated_flags(*eval_ctx).set(count);
          relevance_proj_expr->set_evaluated_projected(*eval_ctx);
        }
        need_project = (0 != relevance);
      }
    } else if (OB_UNLIKELY(0 != count)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      // the generic path does not evaluate the filter in the non-vectorized case either
      ObEvalCtx::BatchInfoScopeGuard guard(*eval_ctx);
      guard.set_batch_idx(0);
      ObDatum &relevance_proj_datum = relevance_proj_expr->locate_datum_for_write(*eval_ctx);
      relevance_proj_datum.set_double(relevance);
      relevance_proj_expr->set_evaluated_projected(*eval_ctx);
    }
  } else if (eval_ctx->is_vectorized()) {
    ObEvalCtx::BatchInfoScopeGuard guard(*eval_ctx);
    if (OB_UNLIKELY(count >= eval_ctx->max_batch_size_)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      guard.set_batch_idx(count);
      ObDatum &relevance_proj_datum = relevance_proj_expr->locate_datum_for_write(*eval_ctx);
      relevance_proj_datum.set_double(relevance);
      relevance_proj_expr->get_evaluated_flags(*eval_ctx).set(count);
      relevance_proj_expr->set_evaluated_projected(*eval_ctx);
      ObDatum *filter_res = nullptr;
      filter_expr->clear_evaluated_flag(*eval_ctx);
      if (OB_FAIL(filter_expr->eval(*eval_ctx, filter_res))) {
      } else {
        need_project = !(filter_res->is_null() || 0 == filter_res->get_int());
      }
    }
  } else if (OB_UNLIKELY(0 != count)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    ObEvalCtx::BatchInfoScopeGuard guard(*eval_ctx);
    guard.set_batch_idx(0);
    ObDatum &relevance_proj_datum = relevance_proj_expr->locate_datum_for_write(*eval_ctx);
    relevance_proj_datum.set_double(relevance);
    relevance_proj_expr->set_evaluated_projected(*eval_ctx);
  }
  return ret;
}

int ObSRDaaTIterImpl::cache_result(int64_t &count, const ObDatum &id_datum, const double relevance)
{
  int ret = OB_SUCCESS;
  const common::ObLimitParam *limit_param= iter_param_->limit_param_;
  const int64_t limit = limit_param->limit_;
  const int64_t offset = limit_param->offset_;
  ++input_row_cnt_;
  if (limit_param->is_valid() && input_row_cnt_ <= offset) {
    // TODO: Maybe we should not process offset logic here
    // don't need to project
  } else if (OB_UNLIKELY(count >= buffered_domain_ids_.count() || count >= buffered_relevances_.count())) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    buffered_domain_ids_[count].from_datum(id_datum);
    buffered_relevances_[count] = relevance;
    ++count;
    ++output_row_cnt_;
    if (limit_param->is_valid() && output_row_cnt_ >= limit) {
      ret = OB_ITER_END;
    }
  }
  return ret;
}

int ObSRDaaTIterImpl::project_results(const int64_t count)
{
  int ret = OB_SUCCESS;
  ObExpr *relevance_proj_expr = iter_param_->relevance_proj_expr_;
  ObExpr *id_proj_expr = iter_param_->id_proj_expr_;
  ObEvalCtx *eval_ctx = iter_param_->eval_ctx_;
  ObEvalCtx::BatchInfoScopeGuard guard(*eval_ctx);

  if (eval_ctx->is_vectorized()) {
    sql::ObBitVector &id_evaluated_flags = id_proj_expr->get_evaluated_flags(*eval_ctx);
    for (int64_t i = 0; i < count; ++i) {
      guard.set_batch_idx(i);
      ObDatum &id_proj_datum = id_proj_expr->locate_datum_for_write(*eval_ctx);
      set_datum_func_(id_proj_datum, buffered_domain_ids_[i]);
      id_evaluated_flags.set(i);
      id_proj_expr->set_evaluated_projected(*eval_ctx);
    }
    if (iter_param_->need_project_relevance()) {
      sql::ObBitVector &relevance_evaluated_flags = relevance_proj_expr->get_evaluated_flags(*eval_ctx);
      for (int64_t i = 0; i < count; ++i) {
        guard.set_batch_idx(i);
        ObDatum &relevance_proj_datum = relevance_proj_expr->locate_datum_for_write(*eval_ctx);
        relevance_proj_datum.set_double(buffered_relevances_[i]);
        relevance_evaluated_flags.set(i);
        relevance_proj_expr->set_evaluated_projected(*eval_ctx);
      }
    }
  } else if (OB_UNLIKELY(1 != count)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    guard.set_batch_idx(0);
    ObDatum &id_proj_datum = id_proj_expr->locate_datum_for_write(*eval_ctx);
    set_datum_func_(id_proj_datum, buffered_domain_ids_[0]);
    id_proj_expr->set_evaluated_projected(*eval_ctx);
    ObDatum &relevance_proj_datum = relevance_proj_expr->locate_datum_for_write(*eval_ctx);
    relevance_proj_datum.set_double(buffered_relevances_[0]);
    relevance_proj_expr->set_evaluated_projected(*eval_ctx);
  }
  return ret;
}

// ---------------------------------------------------------------------------
// Fast path for the simple-merger shape.
//
// The generic round (fill_merge_heap + collect_dims_by_id) keeps its k-way state in
// an ObSimpleRowsMerger: a heap-allocated object, reached through virtual push/pop/
// top/is_unique_champion, whose comparisons go back through iter_ids_->at(). With at
// most USE_SIMPLE_MERGER_MAX_TABLE_CNT live items that machinery dominates the round.
//
// The fast path keeps the assignment of work identical:
//   * only the dimensions consumed by the previous round advance (next_round_cnt_),
//   * the live items stay sorted by (doc id, dim index), the same order and the same
//     ObSRMergeCmp call ObSimpleRowsMerger::push produces,
//   * equal_with_next_ is maintained exactly as push() does, so a document hit by
//     several dimensions is consumed as one row,
//   * a dimension that runs out of postings drops out of the round (not re-pushed),
//   * rows are consumed front-to-back, i.e. dimensions accumulate in ascending dim
//     index, which is what keeps the relevance sum bit-identical (double addition is
//     not associative).
// Only the data structure changes: a fixed inline array instead of the merger object.
// filter_on_demand / cache_result / project_results are untouched and shared with the
// generic path.
// ---------------------------------------------------------------------------
int ObSRDaaTIterImpl::fast_push_item(const ObSRMergeItem &item)
{
  int ret = OB_SUCCESS;
  bool equal_with_next = false;
  if (OB_UNLIKELY(fast_item_cnt_ >= FAST_MERGE_MAX_DIMS)) {
    ret = OB_SIZE_OVERFLOW;
  } else if (0 == fast_item_cnt_) {
    fast_items_[0] = item;
  } else {
    int64_t cmp_ret = 0;
    int64_t i = fast_item_cnt_ - 1;
    for (; OB_SUCC(ret) && i >= 0; --i) {
      if (OB_FAIL(merge_cmp_.cmp(item, fast_items_[i], cmp_ret))) {
      } else if (cmp_ret < 0) {
        fast_items_[i + 1] = fast_items_[i];
      } else if (0 == cmp_ret) {
        // same doc id: keep dim index ascending, and remember the run of equal ids
        if (item.iter_idx_ < fast_items_[i].iter_idx_) {
          fast_items_[i + 1] = fast_items_[i];
          equal_with_next = true;
        } else {
          fast_items_[i].equal_with_next_ = true;
          break;
        }
      } else {
        break;
      }
    }
    if (OB_SUCC(ret)) {
      fast_items_[i + 1] = item;
      if (equal_with_next) {
        fast_items_[i + 1].equal_with_next_ = true;
      }
    }
  }
  if (OB_SUCC(ret)) {
    ++fast_item_cnt_;
  }
  return ret;
}

int ObSRDaaTIterImpl::fast_fill_merge_array()
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < next_round_cnt_; ++i) {
    const int64_t iter_idx = next_round_iter_idxes_[i];
    ObISRDaaTDimIter *dim_iter = nullptr;
    ObSRMergeItem item;
    if (OB_ISNULL(dim_iter = dim_iters_->at(iter_idx))) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(dim_iter->get_next_row())) {
      if (OB_UNLIKELY(OB_ITER_END != ret)) {
      } else {
        ret = OB_SUCCESS;
      }
    } else if (OB_FAIL(dim_iter->get_curr_score(item.relevance_))) {
    } else if (OB_NOT_NULL(iter_param_->dim_weights_) && FALSE_IT(item.relevance_ = item.relevance_ * iter_param_->field_boost_ * iter_param_->dim_weights_->at(iter_idx))) {
    } else if (OB_FAIL(dim_iter->get_curr_id(iter_domain_ids_[iter_idx]))) {
    } else if (FALSE_IT(item.iter_idx_ = iter_idx)) {
    } else if (OB_FAIL(fast_push_item(item))) {
    }
  }

  if (OB_FAIL(ret)) {
  } else if (0 == fast_item_cnt_) {
    ret = OB_ITER_END;
  } else {
    next_round_cnt_ = 0;
  }
  return ret;
}

int ObSRDaaTIterImpl::fast_collect_dims_by_id(const ObDatum *&id_datum, double &relevance, bool &got_valid_id)
{
  int ret = OB_SUCCESS;
  bool curr_doc_end = false;
  int64_t iter_idx = 0;
  relevance = 0.0;
  got_valid_id = false;

  while (OB_SUCC(ret) && fast_item_cnt_ > 0 && !curr_doc_end) {
    if (!fast_items_[0].equal_with_next_) {
      // no other dimension carries this doc id, so this is the last item of the row
      curr_doc_end = true;
    }
    iter_idx = fast_items_[0].iter_idx_;
    if (OB_FAIL(relevance_collector_->collect_one_dim(iter_idx, fast_items_[0].relevance_))) {
    } else {
      for (int64_t i = 0; i < fast_item_cnt_ - 1; ++i) {
        fast_items_[i] = fast_items_[i + 1];
      }
      --fast_item_cnt_;
      next_round_iter_idxes_[next_round_cnt_++] = iter_idx;
    }
  }

  if (OB_SUCC(ret)) {
    id_datum = iter_domain_ids_[iter_idx];
    if (OB_ISNULL(id_datum)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(relevance_collector_->get_result(relevance, got_valid_id))) {
    } else if (got_valid_id && OB_FAIL(process_collected_row(*id_datum, relevance))) {
    }
  }
  return ret;
}

int ObSRDaaTIterImpl::fast_do_one_merge_round(int64_t &count)
{
  int ret = OB_SUCCESS;
  bool need_project = true;
  double relevance = 0.0;
  const ObDatum *id_datum = nullptr;
  if (OB_FAIL(fast_fill_merge_array())) {
    if (OB_UNLIKELY(OB_ITER_END != ret)) {
    }
  } else if (OB_FAIL(fast_collect_dims_by_id(id_datum, relevance, need_project))) {
  } else if (need_project && OB_FAIL(filter_on_demand(count, relevance, need_project))) {
  } else if (need_project && OB_FAIL(cache_result(count, *id_datum, relevance))) {
    if (OB_ITER_END != ret) {
    }
  }
  return ret;
}

// ---------------------------------------------------------------------------
// Batch shape of the same merge.
//
// The text dimension iterator keeps a whole batch of the postings it read in two fixed
// arrays (doc ids and raw per token relevances) and serves one row at a time through
// three virtual calls, of which a merge round uses exactly one id datum and one double.
// This path takes a zero copy view of those arrays and merges on them directly, so a
// round indexes arrays instead of paying that interface per row.
//
// The assignment of work is unchanged:
//   * a row is consumed only when its id takes part in the current row, and the
//     dimensions taking part are collected in ascending dimension order, which is the
//     order the row at a time paths accumulate in (double addition is not associative),
//   * a dimension that runs out of postings drops out of the merge,
//   * field boost and dimension weights are applied to the raw relevance at the same
//     point in the expression as before.
// filter_on_demand / cache_result / project_results stay shared with the other paths.
// ---------------------------------------------------------------------------
void ObSRDaaTIterImpl::batch_reset_state()
{
  batch_inited_ = false;
  batch_active_cnt_ = 0;
  for (int64_t i = 0; i < FAST_MERGE_MAX_DIMS; ++i) {
    batch_active_[i] = 0;
    batch_cur_[i] = 0;
    batch_views_[i].ids_ = nullptr;
    batch_views_[i].scores_ = nullptr;
    batch_views_[i].count_ = 0;
  }
}

int ObSRDaaTIterImpl::batch_refill_dim(const int64_t dim)
{
  int ret = OB_SUCCESS;
  bool exhausted = false;
  ObSRDimBatchView &view = batch_views_[dim];
  while (OB_SUCC(ret) && !exhausted && batch_cur_[dim] >= view.count_) {
    view.ids_ = nullptr;
    view.scores_ = nullptr;
    view.count_ = 0;
    if (OB_FAIL(dim_iters_->at(dim)->get_batch_view(view))) {
      if (OB_UNLIKELY(OB_ITER_END != ret)) {
        LOG_WARN("failed to get batch view of dimension", K(ret), K(dim));
      } else {
        ret = OB_SUCCESS;
        exhausted = true;
      }
    } else if (OB_UNLIKELY(view.count_ <= 0)) {
      exhausted = true;
    } else {
      batch_cur_[dim] = 0;
    }
  }
  return ret;
}

int ObSRDaaTIterImpl::batch_do_one_merge_round(int64_t &count)
{
  int ret = OB_SUCCESS;
  bool need_project = true;
  double relevance = 0.0;
  const ObDatum *id_datum = nullptr;
  if (OB_UNLIKELY(!batch_inited_)) {
    batch_active_cnt_ = dim_iters_->count();
    for (int64_t i = 0; i < batch_active_cnt_; ++i) {
      batch_active_[i] = i;
      batch_cur_[i] = 0;
      batch_views_[i].ids_ = nullptr;
      batch_views_[i].scores_ = nullptr;
      batch_views_[i].count_ = 0;
    }
    batch_inited_ = true;
  }
  // Retire what the previous round consumed, and drop the dimensions that have no
  // more postings -- the same set that the row at a time path stops re-pushing.
  int64_t kept = 0;
  for (int64_t i = 0; OB_SUCC(ret) && i < batch_active_cnt_; ++i) {
    const int64_t dim = batch_active_[i];
    if (OB_FAIL(batch_refill_dim(dim))) {
    } else if (batch_cur_[dim] < batch_views_[dim].count_) {
      batch_active_[kept++] = dim;
    }
  }
  if (OB_FAIL(ret)) {
  } else if (FALSE_IT(batch_active_cnt_ = kept)) {
  } else if (0 == batch_active_cnt_) {
    ret = OB_ITER_END;
  } else {
    const int64_t min_dim = batch_active_[0];
    const ObDatum *min_id = &batch_views_[min_dim].ids_[batch_cur_[min_dim]].get_datum();
    int64_t matching_dims[FAST_MERGE_MAX_DIMS] = {min_dim};
    int64_t matching_count = 1;
    for (int64_t i = 1; OB_SUCC(ret) && i < batch_active_cnt_; ++i) {
      const int64_t dim = batch_active_[i];
      const ObDatum *id = &batch_views_[dim].ids_[batch_cur_[dim]].get_datum();
      int64_t cmp_ret = 0;
      if (OB_FAIL(merge_cmp_.cmp_datum(*id, *min_id, cmp_ret))) {
      } else if (cmp_ret < 0) {
        min_id = id;
        matching_dims[0] = dim;
        matching_count = 1;
      } else if (0 == cmp_ret) {
        matching_dims[matching_count++] = dim;
      }
    }
    // Collect every dimension carrying this id -- the run of equal ids -- ascending by
    // dimension index, then advance past the consumed rows.
    for (int64_t i = 0; OB_SUCC(ret) && i < matching_count; ++i) {
      const int64_t dim = matching_dims[i];
      double score = batch_views_[dim].scores_[batch_cur_[dim]++];
      if (OB_NOT_NULL(iter_param_->dim_weights_)
          && FALSE_IT(score = score * iter_param_->field_boost_ * iter_param_->dim_weights_->at(dim))) {
      } else if (OB_FAIL(relevance_collector_->collect_one_dim(dim, score))) {
      }
    }
    if (OB_SUCC(ret)) {
      bool got_valid_id = false;
      id_datum = min_id;
      if (OB_ISNULL(id_datum)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(relevance_collector_->get_result(relevance, got_valid_id))) {
      } else if (got_valid_id && OB_FAIL(process_collected_row(*id_datum, relevance))) {
      } else {
        need_project = got_valid_id;
      }
    }
    if (OB_SUCC(ret) && need_project && OB_FAIL(filter_on_demand(count, relevance, need_project))) {
    } else if (OB_SUCC(ret) && need_project && OB_FAIL(cache_result(count, *id_datum, relevance))) {
      if (OB_ITER_END != ret) {
      }
    }
  }
  return ret;
}

int ObSRDaaTIterImpl::init_merge_heap(const int64_t count)
{
  int ret = OB_SUCCESS;
  ObSRSimpleMerger *simple_merge = nullptr;
  ObSRMergeHeap *loser_tree = nullptr;
  if (count <= ObSRSimpleMerger::USE_SIMPLE_MERGER_MAX_TABLE_CNT) {
    if (OB_ISNULL(simple_merge = OB_NEWx(ObSRSimpleMerger, iter_allocator_, merge_cmp_))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      merge_heap_ = simple_merge;
    }
  } else if (OB_ISNULL(loser_tree = OB_NEWx(ObSRLoserTree, iter_allocator_, merge_cmp_))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    merge_heap_ = loser_tree;
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(merge_heap_->init(count, count, *iter_allocator_))) {
  } else if (OB_FAIL(merge_heap_->open(count))) {
  }
  return ret;
}

} // namespace storage
} // namespace oceanbase
