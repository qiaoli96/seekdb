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

#include "data_plane/fts/ob_fts_struct.h"

#include "lib/charset/ob_charset.h"
#include "share/datum/ob_datum_funcs.h"
#include <stdlib.h>
#include "storage/ob_storage_util.h"

namespace oceanbase
{
namespace storage
{

// The datum hash function and the datum comparison function are pure functions of the column
// metadata, and that metadata is constant for the whole duration of one tokenize/index call.
// Caching the last lookup makes the two hot paths (hash, equality) skip the per-call table lookup
// without changing which function is used: the cache key is the very pair that selects it.
namespace {
struct ObFTWordFuncCache
{
  ObObjType type_;
  ObCollationType cs_;
  const ObDatumBasicFuncs *basic_;
  bool basic_valid_;
  ObCollationType r_cs_;
  ObDatumCmpFuncType cmp_;
  bool cmp_valid_;
};
thread_local ObFTWordFuncCache g_ft_word_func_cache;

bool ob_ft_word_fast_on()
{
  static const bool off = nullptr != ::getenv("OB_AW_FAST_OFF")
                          || nullptr != ::getenv("OB_AW_FUNC_OFF");
  return !off;
}
}  // namespace

int ObFTWord::hash(uint64_t &hash_val) const
{
  int ret = OB_SUCCESS;
  common::ObDatumBasicFuncs *funcs = nullptr;
  if (ob_ft_word_fast_on() && g_ft_word_func_cache.basic_valid_
      && g_ft_word_func_cache.type_ == meta_.get_type()
      && g_ft_word_func_cache.cs_ == meta_.get_collation_type()
      && nullptr != g_ft_word_func_cache.basic_
      && nullptr != g_ft_word_func_cache.basic_->default_hash_) {
    funcs = const_cast<common::ObDatumBasicFuncs *>(g_ft_word_func_cache.basic_);
  } else {
    funcs = ObDatumFuncs::get_basic_func(meta_.get_type(), meta_.get_collation_type());
    if (ob_ft_word_fast_on() && nullptr != funcs && nullptr != funcs->default_hash_) {
      g_ft_word_func_cache.type_ = meta_.get_type();
      g_ft_word_func_cache.cs_ = meta_.get_collation_type();
      g_ft_word_func_cache.basic_ = funcs;
      g_ft_word_func_cache.basic_valid_ = true;
    }
  }
  if (OB_ISNULL(funcs)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (funcs->default_hash_ == nullptr) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    ret = funcs->default_hash_(word_, 0, hash_val, nullptr);
  }
  return ret;
}
bool ObFTWord::operator==(const ObFTWord &other) const
{
  bool is_equal = false;
  int ret = OB_SUCCESS;
  int cmp_ret = 0;
  ObDatumCmpFuncType func = nullptr;
  if (ob_ft_word_fast_on() && g_ft_word_func_cache.cmp_valid_
      && g_ft_word_func_cache.r_cs_ == other.get_collation_type()
      && g_ft_word_func_cache.cs_ == get_collation_type()
      && nullptr != g_ft_word_func_cache.cmp_) {
    func = g_ft_word_func_cache.cmp_;
  } else {
    func = get_datum_cmp_func(meta_, other.meta_);
    if (ob_ft_word_fast_on() && nullptr != func) {
      g_ft_word_func_cache.r_cs_ = other.get_collation_type();
      g_ft_word_func_cache.cs_ = get_collation_type();
      g_ft_word_func_cache.cmp_ = func;
      g_ft_word_func_cache.cmp_valid_ = true;
    }
  }
  if (func == nullptr) {
    ob_abort();
  } else if (OB_FAIL(func(word_, other.word_, cmp_ret, nullptr))) {
    ob_abort();
  } else {
    is_equal = (cmp_ret == 0);
  }
  return is_equal;
}
} // namespace storage
} // namespace oceanbase
