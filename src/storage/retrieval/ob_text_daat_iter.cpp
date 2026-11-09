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

#include "ob_text_daat_iter.h"

namespace oceanbase
{
namespace storage
{
int ObTextDaaTIter::init(const ObTextDaaTParam &param)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(param.base_param_) || OB_ISNULL(param.dim_iters_) || OB_ISNULL(param.allocator_)
      || OB_ISNULL(param.relevance_collector_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObSRDaaTIterImpl::init(*param.base_param_, *param.dim_iters_,
                                            *param.allocator_, *param.relevance_collector_))) {
  } else if (OB_FAIL(bm25_param_estimator_.init(param.bm25_param_est_ctx_))) {
  } else {
    mode_flag_ = param.mode_flag_;
    function_lookup_mode_ = param.function_lookup_mode_;
    // The DAAT text engine is only ever built with one ObTextRetrievalDaaTTokenIter per
    // query token (ObDASLegacyTRMergeIter::create_dim_iters, daat_mode_), i.e. plain
    // forward postings cursors: no skip list, no block max. With at most
    // FAST_MERGE_MAX_DIMS of them the generic round runs through ObSimpleRowsMerger,
    // which the fast path replaces one-for-one. Anything else keeps the generic path.
    use_fast_merge_ = (dim_iters_->count() > 0 && dim_iters_->count() <= FAST_MERGE_MAX_DIMS);
    // Natural language mode only: boolean mode scores through ObFtsEvalNode instead of
    // the inner product collector, and the query expansion modes rewrite the token set,
    // so in both the pushed-down filter is not the plain score coercion.
    use_inline_match_filter_ = (sql::NATURAL_LANGUAGE_MODE == param.mode_flag_)
                               && is_canonical_match_filter();
    LOG_DEBUG("daat text retrieval fast merge gate", K(use_fast_merge_), "dim_cnt", dim_iters_->count());
  }
  return ret;
}

void ObTextDaaTIter::reset()
{
  bm25_param_estimator_.reset();
  // this class can know the type of dim_iters_, but ObSRDaaTIterImpl maybe not
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(i))->reset();
    }
  }
  ObSRDaaTIterImpl::reset();
}

void ObTextDaaTIter::reuse(const bool switch_tablet)
{
  if ((OB_NOT_NULL(dim_iters_) && 0 == dim_iters_->count())) {
    // do nothing
  } else {
    bm25_param_estimator_.reuse(switch_tablet);
  }
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(i))->reuse();
    }
  }
  ObSRDaaTIterImpl::reuse(switch_tablet);
}

int ObTextDaaTIter::pre_process()
{
  int ret = OB_SUCCESS;
  if (dim_iters_->count() == 0) {
    ret = OB_ITER_END;
  } else if (iter_param_->need_project_relevance()) {
    if (OB_FAIL(bm25_param_estimator_.do_estimation(*iter_param_->eval_ctx_))) {
    }
  }
  return ret;
}

ObTextBMWIter::ObTextBMWIter()
  : ObSRBMWIterImpl(),
    bm25_param_estimator_() {}

void ObTextBMWIter::reuse(const bool switch_tablet)
{
  bm25_param_estimator_.reuse(switch_tablet);
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i))->reuse();
    }
  }
  ObSRBMWIterImpl::reuse(switch_tablet);
}

void ObTextBMWIter::reset()
{
  bm25_param_estimator_.reset();
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i))->reset();
    }
  }
  ObSRBMWIterImpl::reset();
}

int ObTextBMWIter::init(const ObTextDaaTParam &param)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(param.base_param_) || OB_ISNULL(param.dim_iters_) || OB_ISNULL(param.allocator_)
      || OB_ISNULL(param.relevance_collector_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObSRBMWIterImpl::init(*param.base_param_, *param.dim_iters_,
                                           *param.allocator_, *param.relevance_collector_))) {
  } else if (OB_FAIL(bm25_param_estimator_.init(param.bm25_param_est_ctx_))) {
  }
  return ret;
}

int ObTextBMWIter::get_next_rows(const int64_t capacity, int64_t &count)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (dim_iters_->count() == 0) {
    ret = OB_ITER_END;
  } else if (OB_FAIL(bm25_param_estimator_.do_estimation(*iter_param_->eval_ctx_))) {
  }

  if (FAILEDx(ObSRBMWIterImpl::get_next_rows(capacity, count))) {
    if (OB_UNLIKELY(OB_ITER_END != ret)) {
    }
  }
  return ret;
}

int ObTextBMWIter::init_before_wand_process()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!bm25_param_estimator_.is_estimated())) {
    ret = OB_ERR_UNEXPECTED;
  }

  for (int64_t i = 0; OB_SUCC(ret) && i < dim_iters_->count(); ++i) {
    ObTextRetrievalBlockMaxIter *block_max_iter = static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i));
    if (OB_FAIL(block_max_iter->init_block_max_iter(
        bm25_param_estimator_.get_total_doc_cnt(), bm25_param_estimator_.get_avg_doc_token_cnt()))) {
    }
  }
  return ret;
}

ObTextDaaTTopKIter::ObTextDaaTTopKIter()
  : ObSRBMWIterImpl(),
    bm25_param_estimator_() {}

void ObTextDaaTTopKIter::reuse(const bool switch_tablet)
{
  bm25_param_estimator_.reuse(switch_tablet);
  reuse_dim_iters(true);
  ObSRBMWIterImpl::reuse(switch_tablet);
}

void ObTextDaaTTopKIter::reset()
{
  bm25_param_estimator_.reset();
  reuse_dim_iters(false);
  ObSRBMWIterImpl::reset();
}

// The dimensions of this engine come either as block max cursors (the shape the plan already
// asks for when top-k is pushed down) or as plain token cursors; both keep state that has to be
// released on tablet switch. Nothing here depends on which one it gets.
void ObTextDaaTTopKIter::reuse_dim_iters(const bool do_reuse)
{
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      ObISRDaaTDimIter *dim_iter = dim_iters_->at(i);
      if (OB_NOT_NULL(dim_iter)) {
        if (dim_iter->is_text_block_max_iter()) {
          ObTextRetrievalBlockMaxIter *block_max_iter = static_cast<ObTextRetrievalBlockMaxIter *>(dim_iter);
          do_reuse ? block_max_iter->reuse() : block_max_iter->reset();
        } else {
          ObTextRetrievalDaaTTokenIter *token_iter = static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iter);
          do_reuse ? token_iter->reuse() : token_iter->reset();
        }
      }
    }
  }
}

int ObTextDaaTTopKIter::init(const ObTextDaaTParam &param)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(param.base_param_) || OB_ISNULL(param.dim_iters_) || OB_ISNULL(param.allocator_)
      || OB_ISNULL(param.relevance_collector_)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(param));
  } else if (OB_FAIL(ObSRBMWIterImpl::init(*param.base_param_, *param.dim_iters_,
                                           *param.allocator_, *param.relevance_collector_))) {
    LOG_WARN("fail to init bmw iter", K(ret));
  } else if (OB_FAIL(bm25_param_estimator_.init(param.bm25_param_est_ctx_))) {
    LOG_WARN("fail to init bm25 param estimator", K(ret));
  } else {
    // The same shape gate the plain DaaT text engine uses: the round at a time merge below
    // replaces ObSimpleRowsMerger one for one, and anything wider keeps the generic merge.
    use_fast_merge_ = (dim_iters_->count() > 0 && dim_iters_->count() <= FAST_MERGE_MAX_DIMS);
  }
  return ret;
}

int ObTextDaaTTopKIter::get_next_rows(const int64_t capacity, int64_t &count)
{
  int ret = OB_SUCCESS;
  count = 0;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (OB_UNLIKELY(capacity <= 0)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (BMWStatus::FINISHED == status_) {
    // every candidate has been scored, only the projection is left
  } else if (0 == dim_iters_->count()) {
    ret = OB_ITER_END;
  } else if (OB_FAIL(bm25_param_estimator_.do_estimation(*iter_param_->eval_ctx_))) {
    LOG_WARN("fail to estimate bm25 params", K(ret));
  } else if (OB_FAIL(init_before_wand_process())) {
    LOG_WARN("fail to init dim iter block caps", K(ret));
  } else if (OB_FAIL(drain_candidates())) {
    if (OB_UNLIKELY(OB_ITER_END != ret)) {
      LOG_WARN("fail to drain candidates", K(ret));
    } else {
      ret = OB_SUCCESS;
      status_ = BMWStatus::FINISHED;
    }
  } else {
    status_ = BMWStatus::FINISHED;
  }

  if (FAILEDx(project_rows_from_top_k_heap(capacity, count))) {
    if (OB_UNLIKELY(OB_ITER_END != ret)) {
      LOG_WARN("fail to project rows from top k heap", K(ret));
    }
  }
  return ret;
}

int ObTextDaaTTopKIter::drain_candidates()
{
  int ret = OB_SUCCESS;
  int64_t projected = 0;
  while (OB_SUCC(ret)) {
    const int merge_ret = (use_fast_merge_ && use_batch_merge_)
        ? batch_do_one_merge_round(projected)
        : (use_fast_merge_ ? fast_do_one_merge_round(projected) : do_one_merge_round(projected));
    if (OB_FAIL(merge_ret) && OB_UNLIKELY(OB_ITER_END != merge_ret)) {
      LOG_WARN("fail to merge one round", K(ret));
    }
  }
  return ret;
}

int ObTextDaaTTopKIter::init_before_wand_process()
{
  int ret = OB_SUCCESS;
  // Only the block max cursor needs its block caps; the plain token cursor answers the same
  // interface without them.
  for (int64_t i = 0; OB_SUCC(ret) && i < dim_iters_->count(); ++i) {
    ObISRDaaTDimIter *dim_iter = dim_iters_->at(i);
    if (OB_ISNULL(dim_iter)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (dim_iter->is_text_block_max_iter()
        && OB_FAIL(static_cast<ObTextRetrievalBlockMaxIter *>(dim_iter)->init_block_max_iter(
            bm25_param_estimator_.get_total_doc_cnt(),
            bm25_param_estimator_.get_avg_doc_token_cnt()))) {
      LOG_WARN("fail to init block max iter", K(ret));
    }
  }
  return ret;
}

int ObTextDaaTTopKIter::filter_on_demand(const int64_t count, const double relevance,
                                         bool &need_project)
{
  UNUSEDx(count, relevance);
  need_project = false;
  return OB_SUCCESS;
}

int ObTextDaaTTopKIter::cache_result(int64_t &count, const ObDatum &id_datum,
                                     const double relevance)
{
  UNUSEDx(count, id_datum, relevance);
  return OB_SUCCESS;
}

} // namespace storage
} // namespace oceanbase
