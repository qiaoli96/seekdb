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

#define USING_LOG_PREFIX SQL_DAS

#include "sql/das/iter/sparse_retrieval/ob_das_tr_merge_iter.h"

#include <mutex>

#include "sql/das/iter/sparse_retrieval/ob_das_text_retrieval_engine.h"
#include "sql/engine/ob_exec_context.h"

namespace oceanbase
{
using namespace share;
namespace sql
{
namespace
{

struct ObDASTextRetrievalProviderSlot
{
  ObDASTextRetrievalProviderSlot()
    : lock_(), factory_(nullptr), query_builder_(nullptr)
  {}

  std::mutex lock_;
  ObDASTextRetrievalEngineFactory factory_;
  ObDASTextRetrievalQueryBuilder query_builder_;
};

ObDASTextRetrievalProviderSlot &provider_slot()
{
  static ObDASTextRetrievalProviderSlot slot;
  return slot;
}

} // namespace

int install_das_text_retrieval_engine_factory(
    ObDASTextRetrievalEngineFactory factory,
    ObDASTextRetrievalQueryBuilder query_builder)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(factory) || OB_ISNULL(query_builder)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObDASTextRetrievalProviderSlot &slot = provider_slot();
    std::lock_guard<std::mutex> guard(slot.lock_);
    if (nullptr == slot.factory_ && nullptr == slot.query_builder_) {
      slot.factory_ = factory;
      slot.query_builder_ = query_builder;
    } else if (slot.factory_ != factory || slot.query_builder_ != query_builder) {
      ret = OB_INIT_TWICE;
    }
  }
  return ret;
}

int create_das_text_retrieval_engine(
    common::ObIAllocator &allocator,
    ObIDASTextRetrievalEngine *&engine)
{
  int ret = OB_SUCCESS;
  ObDASTextRetrievalEngineFactory factory = nullptr;
  engine = nullptr;
  {
    ObDASTextRetrievalProviderSlot &slot = provider_slot();
    std::lock_guard<std::mutex> guard(slot.lock_);
    factory = slot.factory_;
  }
  if (OB_ISNULL(factory)) {
    ret = OB_NOT_INIT;
    LOG_ERROR("DAS text retrieval composition provider is not installed", K(ret));
  } else if (OB_FAIL(factory(allocator, engine))) {
  } else if (OB_ISNULL(engine)) {
    ret = OB_ERR_UNEXPECTED;
  }
  return ret;
}

int build_das_text_retrieval_query(
    const ObDASIRScanCtDef *ir_ctdef,
    ObDASIRScanRtDef *ir_rtdef,
    common::ObIAllocator &allocator,
    ObArray<ObString> &query_tokens,
    ObArray<double> &boost_values,
    ObFtsEvalNode *&root_node,
    bool &has_duplicate_boolean_tokens)
{
  int ret = OB_SUCCESS;
  ObDASTextRetrievalQueryBuilder query_builder = nullptr;
  {
    ObDASTextRetrievalProviderSlot &slot = provider_slot();
    std::lock_guard<std::mutex> guard(slot.lock_);
    query_builder = slot.query_builder_;
  }
  if (OB_ISNULL(query_builder)) {
    ret = OB_NOT_INIT;
    LOG_ERROR("DAS text retrieval query provider is not installed", K(ret));
  } else if (OB_FAIL(query_builder(
      ir_ctdef, ir_rtdef, allocator, query_tokens, boost_values, root_node,
      has_duplicate_boolean_tokens))) {
  }
  return ret;
}

ObDASTRMergeIter::ObDASTRMergeIter()
  : ObDASIter(ObDASIterType::DAS_ITER_TEXT_RETRIEVAL_MERGE),
    engine_allocator_(lib::ObMemAttr("DASRetrieval"), OB_MALLOC_NORMAL_BLOCK_SIZE),
    engine_(nullptr),
    is_inited_(false),
    ir_ctdef_(nullptr),
    ir_rtdef_(nullptr),
    match_count_(0),
    match_count_done_(false)
{}

int ObDASTRMergeIter::inner_init(ObDASIterParam &param)
{
  int ret = OB_SUCCESS;
  ObDASTRMergeIterParam &merge_param = static_cast<ObDASTRMergeIterParam &>(param);
  if (IS_INIT) {
    ret = OB_INIT_TWICE;
  } else if (OB_UNLIKELY(
      ObDASIterType::DAS_ITER_TEXT_RETRIEVAL_MERGE != param.type_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(create_das_text_retrieval_engine(
      engine_allocator_, engine_))) {
  } else if (OB_FAIL(engine_->init(merge_param))) {
  } else {
    ir_ctdef_ = merge_param.ir_ctdef_;
    ir_rtdef_ = merge_param.ir_rtdef_;
    match_count_ = 0;
    match_count_done_ = false;
    is_inited_ = true;
  }
  return ret;
}

int ObDASTRMergeIter::set_related_tablet_ids(
    const ObDASFTSTabletID &related_tablet_ids)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(engine_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(engine_->set_related_tablet_ids(related_tablet_ids))) {
  }
  return ret;
}

int ObDASTRMergeIter::do_table_scan()
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT || OB_ISNULL(engine_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(engine_->bind_source_tree(children_, children_cnt_))) {
  } else if (OB_FAIL(reset_match_count())) {
  } else if (OB_FAIL(engine_->do_table_scan())) {
  }
  return ret;
}

int ObDASTRMergeIter::inner_reuse()
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT || OB_ISNULL(engine_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(engine_->bind_source_tree(children_, children_cnt_))) {
  } else if (OB_FAIL(reset_match_count())) {
  } else if (OB_FAIL(engine_->reuse())) {
  }
  return ret;
}

int ObDASTRMergeIter::rescan()
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT || OB_ISNULL(engine_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(engine_->bind_source_tree(children_, children_cnt_))) {
  } else if (OB_FAIL(reset_match_count())) {
  } else if (OB_FAIL(engine_->rescan())) {
  }
  return ret;
}

int ObDASTRMergeIter::inner_release()
{
  if (OB_NOT_NULL(engine_)) {
    engine_->destroy();
    engine_ = nullptr;
  }
  engine_allocator_.reset();
  is_inited_ = false;
  return OB_SUCCESS;
}

bool ObDASTRMergeIter::need_match_count_agg() const
{
  return OB_NOT_NULL(ir_ctdef_) && ir_ctdef_->need_match_count_agg();
}

int ObDASTRMergeIter::reset_match_count()
{
  match_count_ = 0;
  match_count_done_ = false;
  return OB_SUCCESS;
}

int ObDASTRMergeIter::project_match_count()
{
  int ret = OB_SUCCESS;
  ObExpr *match_count_expr = nullptr;
  ObEvalCtx *eval_ctx = nullptr;
  if (OB_ISNULL(ir_ctdef_) || OB_ISNULL(ir_rtdef_)
      || OB_ISNULL(match_count_expr = ir_ctdef_->match_count_expr_)
      || OB_ISNULL(eval_ctx = ir_rtdef_->eval_ctx_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid match count projection context", K(ret));
  } else {
    // The scan emits exactly one row, so the value always sits at batch index 0,
    // and its single requested column is the number of matching documents. Zero
    // matches still produce this one row so that the sum above stays non-null.
    ObDatum *datums = nullptr;
    if (eval_ctx->is_vectorized()) {
      if (OB_ISNULL(datums = match_count_expr->locate_datums_for_update(*eval_ctx, 1))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("failed to locate the match count datum", K(ret));
      } else {
        datums[0].set_int(match_count_);
      }
    } else {
      match_count_expr->locate_datum_for_write(*eval_ctx).set_int(match_count_);
    }
    if (OB_SUCC(ret)) {
      match_count_expr->get_evaluated_flags(*eval_ctx).set(0);
      match_count_expr->set_evaluated_projected(*eval_ctx);
    }
  }
  return ret;
}

int ObDASTRMergeIter::inner_get_next_row()
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT || OB_ISNULL(engine_)) {
    ret = OB_NOT_INIT;
  } else if (!need_match_count_agg()) {
    ret = engine_->get_next_row();
  } else if (match_count_done_) {
    ret = OB_ITER_END;
  } else {
    // The upper aggregation only needs the number of matching documents, so drain
    // the retrieval engine here instead of materializing the documents.
    while (OB_SUCC(ret)) {
      ret = engine_->get_next_row();
      if (OB_SUCC(ret)) {
        ++match_count_;
      }
    }
    if (OB_UNLIKELY(OB_ITER_END != ret)) {
      LOG_WARN("failed to drain the text retrieval iterator", K(ret));
    } else if (OB_FAIL(project_match_count())) {
    } else {
      ret = OB_SUCCESS;
      match_count_done_ = true;
    }
  }
  return ret;
}

int ObDASTRMergeIter::inner_get_next_rows(int64_t &count, int64_t capacity)
{
  int ret = OB_SUCCESS;
  count = 0;
  if (IS_NOT_INIT || OB_ISNULL(engine_)) {
    ret = OB_NOT_INIT;
  } else if (!need_match_count_agg()) {
    ret = engine_->get_next_rows(count, capacity);
  } else if (match_count_done_) {
    ret = OB_ITER_END;
  } else {
    const int64_t batch_capacity = capacity > 0 ? capacity : 1;
    while (OB_SUCC(ret)) {
      int64_t batch_count = 0;
      ret = engine_->get_next_rows(batch_count, batch_capacity);
      if (batch_count > 0) {
        match_count_ += batch_count;
      }
    }
    if (OB_UNLIKELY(OB_ITER_END != ret)) {
      LOG_WARN("failed to drain the text retrieval iterator", K(ret));
    } else if (OB_FAIL(project_match_count())) {
    } else {
      count = 1;
      ret = OB_SUCCESS;
      match_count_done_ = true;
    }
  }
  return ret;
}

int ObDASTRMergeIter::set_children_iter_rangekey(
    const common::ObIArray<std::pair<ObDocIdExt, int>> &virtual_rangekeys,
    const int64_t batch_size)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT || OB_ISNULL(engine_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(engine_->bind_source_tree(children_, children_cnt_))) {
  } else if (OB_FAIL(engine_->set_lookup_keys(
      virtual_rangekeys, batch_size))) {
  }
  return ret;
}

bool ObDASTRMergeIter::is_taat_mode()
{
  return OB_NOT_NULL(engine_) && engine_->is_taat_mode();
}

int ObDASTRMergeIter::get_query_max_score(double &score)
{
  score = 0.0;
  return IS_NOT_INIT || OB_ISNULL(engine_)
      ? OB_NOT_INIT : engine_->get_query_max_score(score);
}

int ObDASTRMergeIter::build_query_tokens(
    const ObDASIRScanCtDef *ir_ctdef,
    ObDASIRScanRtDef *ir_rtdef,
    common::ObIAllocator &allocator,
    ObArray<ObString> &query_tokens,
    ObArray<double> &boost_values,
    ObFtsEvalNode *&root_node,
    bool &has_duplicate_boolean_tokens)
{
  return build_das_text_retrieval_query(
      ir_ctdef, ir_rtdef, allocator, query_tokens, boost_values, root_node,
      has_duplicate_boolean_tokens);
}

} // namespace sql
} // namespace oceanbase
