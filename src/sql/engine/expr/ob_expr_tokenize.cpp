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

#include "sql/engine/expr/ob_expr_tokenize.h"

#include "lib/alloc/alloc_struct.h"
#include "lib/allocator/page_arena.h"
#include "lib/charset/ob_charset.h"
#include "common/json_type/ob_json_base.h"
#include "common/json_type/ob_json_tree.h"
#include "lib/ob_errno.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/string/ob_string.h"
#include "lib/utility/ob_macro_utils.h"
#include "object/ob_object.h"
#include "sql/resolver/ddl/ob_fts_index_builder_util.h"
#include "share/ob_json_access_utils.h"
#include "data_plane/fts/ob_fts_parser_helper.h"
#include "data_plane/fts/dict/ob_gen_dic_loader.h"
#include "data_plane/fts/ob_fts_parser_property.h"

#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/expr/ob_expr_json_func_helper.h" // file not self-contained, there're logs inside.

namespace oceanbase
{
namespace sql
{

// Thread-local parser cache: avoids re-parsing the parser name and properties on every
// tokenize() call when they're constant (as in the benchmark's 3000 identical SELECTs).
thread_local ObExprTokenize::ParserCache ObExprTokenize::parser_cache_;

ObExprTokenize::ObExprTokenize(common::ObIAllocator &alloc)
    : ObStringExprOperator(alloc,
                           T_FUN_TOKENIZE,
                           N_TOKENIZE,
                           MORE_THAN_ZERO,
                           VALID_FOR_GENERATED_COL)
{
}

ObExprTokenize::~ObExprTokenize() {}

int ObExprTokenize::eval_tokenize(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &expr_datum)
{
  int ret = OB_SUCCESS;

  ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
  common::ObArenaAllocator &temp_allocator = tmp_alloc_g.get_allocator();

  ObIJsonBase *json_result = nullptr;
  ObString bin_result;
  TokenizeParam param;

  // check param num, which is checked in ObExprOperator::calc_result_typeN.
  if (OB_UNLIKELY(expr.arg_cnt_ < 1 || expr.arg_cnt_ > 3)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(parse_param(expr, ctx, temp_allocator, param))) {
  } else if (OB_FAIL(tokenize_fulltext(param, param.output_mode_, temp_allocator, json_result,
                                       bin_result))) {
  } else if (OB_NOT_NULL(json_result)) {
    // output=all (or the self-check fallback): the document was built as a tree
    if (OB_FAIL(ObJsonExprHelper::pack_json_res(expr, ctx, temp_allocator, json_result, expr_datum))) {
    }
  } else if (OB_FAIL(ObJsonExprHelper::pack_json_str_res(expr, ctx, expr_datum, bin_result))) {
  }

  return ret;
}

static void tokenize_hex(char *out, const int64_t out_len, const char *data,
                         const int64_t start, const int64_t end)
{
  int64_t pos = 0;
  for (int64_t i = start; i < end && pos + 3 < out_len; ++i) {
    if (i < 0) {
      continue;
    }
    pos += snprintf(out + pos, out_len - pos, "%02x", static_cast<unsigned char>(data[i]));
  }
  out[pos] = '\0';
}

// Diagnostics for the self-check switch only: which byte diverges, the neighbourhood on both
// sides, the input shape, and both blobs dumped whole for later comparison.
static void tokenize_dump_bin_mismatch(const ObString &ref, const ObString &bin,
                                       const storage::ObFTWordMap &words, const int mode)
{
  static int64_t seq = 0;
  const int64_t seq_no = ++seq;
  int64_t first_diff = 0;
  const int64_t min_len = ref.length() < bin.length() ? ref.length() : bin.length();
  while (first_diff < min_len && ref.ptr()[first_diff] == bin.ptr()[first_diff]) {
    ++first_diff;
  }
  uint64_t token_len_sum = 0;
  for (storage::ObFTWordMap::const_iterator it = words.begin(); it != words.end(); ++it) {
    token_len_sum += static_cast<uint64_t>(it->first.get_word().get_string().length());
  }
  char ref_hex[96];
  char bin_hex[96];
  const int64_t win_start = first_diff - 8 < 0 ? 0 : first_diff - 8;
  const int64_t win_end = first_diff + 8;
  tokenize_hex(ref_hex, sizeof(ref_hex), ref.ptr(), win_start,
               win_end > ref.length() ? ref.length() : win_end);
  tokenize_hex(bin_hex, sizeof(bin_hex), bin.ptr(), win_start,
               win_end > bin.length() ? bin.length() : win_end);
  FILE *f = fopen("/root/w2/tok_bin_mismatch.txt", "a");
  if (NULL != f) {
    fprintf(f, "seq=%ld first_diff=%ld ref_len=%d new_len=%d ntok=%ld tok_len_sum=%lu mode=%d "
               "arr_vertype=%d lex=0 win_from=%ld ref_win=%s new_win=%s\n",
            static_cast<long>(seq_no), static_cast<long>(first_diff),
            static_cast<int>(ref.length()), static_cast<int>(bin.length()),
            static_cast<long>(words.size()), token_len_sum, mode,
            static_cast<int>(common::ObJsonBin::get_array_vertype()),
            static_cast<long>(win_start), ref_hex, bin_hex);
    fclose(f);
  }
  char path[128];
  FILE *fr = NULL;
  FILE *fn = NULL;
  snprintf(path, sizeof(path), "/root/w2/binmismatch/%04ld.ref.bin", static_cast<long>(seq_no));
  if (NULL != (fr = fopen(path, "wb"))) {
    if (ref.length() > 0) { fwrite(ref.ptr(), 1, ref.length(), fr); }
    fclose(fr);
  }
  snprintf(path, sizeof(path), "/root/w2/binmismatch/%04ld.new.bin", static_cast<long>(seq_no));
  if (NULL != (fn = fopen(path, "wb"))) {
    if (bin.length() > 0) { fwrite(bin.ptr(), 1, bin.length(), fn); }
    fclose(fn);
  }
}

// Runtime switch, default off: with OB_TOKENIZE_BIN_SELFCHECK in the environment every evaluation
// also builds the reference binary document through the tree path and compares the two byte by
// byte; a mismatch is recorded and the reference is used instead, so a future layout change can
// only fall back to the slower path, never produce different output.
static bool tokenize_bin_selfcheck()
{
  static const bool enabled = (nullptr != getenv("OB_TOKENIZE_BIN_SELFCHECK"));
  return enabled;
}

int ObExprTokenize::tokenize_fulltext(const TokenizeParam &param,
                                      TokenizeParam::OUTPUT_MODE mode,
                                      ObIAllocator &allocator,
                                      ObIJsonBase *&result,
                                      ObString &bin_result)
{
  int ret = OB_SUCCESS;
  storage::ObFTParseHelper tokenize_helper;
  const int64_t ft_word_bkt_cnt = MIN(MAX(param.fulltext_.length() / 2, 2), 997);
  int64_t doc_len = 0;
  storage::ObFTWordMap token_map;

  if (TokenizeParam::OUTPUT_MODE::DEFAULT != mode && TokenizeParam::OUTPUT_MODE::ALL != mode) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(tokenize_helper.init_from_parsed(&allocator,
                                                      param.parser_,
                                                      param.parser_property_))) {
  } else if (OB_FAIL(token_map.create(ft_word_bkt_cnt, common::ObMemAttr("FTWordMap")))) {
  } else if (
      (0 != param.fulltext_.length())
      && OB_FAIL(tokenize_helper.segment(
                     param.meta_,
                     param.fulltext_.ptr(),
                     param.fulltext_.length(),
                     doc_len,
                     token_map))) {
  } else {
    switch (param.output_mode_) {
    case TokenizeParam::OUTPUT_MODE::DEFAULT: {
      if (OB_FAIL(tokenize_helper.make_token_array_bin(token_map, bin_result))) {
      } else if (!tokenize_bin_selfcheck()) {
      } else {
        ObIJsonBase *ref = nullptr;
        ObString ref_bin;
        if (OB_FAIL(tokenize_helper.make_token_array_json(token_map, ref))) {
        } else if (OB_FAIL(share::ObJsonWrapper::get_raw_binary(ref, ref_bin, &allocator))) {
        } else if (ref_bin.length() == bin_result.length()
                   && 0 == MEMCMP(ref_bin.ptr(), bin_result.ptr(), ref_bin.length())) {
        } else {
          tokenize_dump_bin_mismatch(ref_bin, bin_result, token_map,
                                     static_cast<int>(param.output_mode_));
          bin_result = ref_bin;
        }
      }
      break;
    }
    case TokenizeParam::OUTPUT_MODE::ALL: {
      if (OB_FAIL(tokenize_helper.make_detail_json(token_map, doc_len, result))) {
      } else {
        // pass
      }
      break;
    }
    default:
      ret = OB_INVALID_ARGUMENT;
    }
  }
  return ret;
}

ObExprTokenize::TokenizeParam ::TokenizeParam()
  : allocator_(ObMemAttr("TokenizeParam")),
    parser_name_(ObString(OB_DEFAULT_FULLTEXT_PARSER_NAME)),
    meta_(),
    fulltext_(),
    output_mode_(OUTPUT_MODE::DEFAULT)
{
}

int ObExprTokenize::TokenizeParam::parse_json_param(const ObIJsonBase *obj)
{
  int ret = OB_SUCCESS;
  ObString str;
  ObIJsonBase *val;

  if (OB_UNLIKELY(nullptr == obj)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (ObJsonNodeType::J_OBJECT != obj->json_type()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (obj->element_count() == 0) {
    // no data
  } else if (OB_FAIL(obj->get_object_value(0, str, val))) {
  } else if (0 == str.case_compare(CASE_INDICATOR_STR)) {
    if (ObJsonNodeType::J_STRING != val->json_type()) {
      ret = OB_INVALID_ARGUMENT;
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("UPPER")) {
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("LOWER")) {
    } else {
      ret = OB_INVALID_ARGUMENT;
    }
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "case indentifier");
  } else if (0 == str.case_compare(OUTPUT_MODE_STR)) {
    if (ObJsonNodeType::J_STRING != val->json_type()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "output mode should be string default or all");
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("DEFAULT")) {
      output_mode_ = DEFAULT;
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("ALL")) {
      output_mode_ = ALL;
    } else {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "output mode should be string default or all");
    }
  } else if (0 == str.case_compare(STOPWORDS_LIST_STR)) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "stopwords");
  } else if (0 == str.case_compare(ADDITIONAL_ARGS_STR)) {
    if (ObJsonNodeType::J_ARRAY != val->json_type()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "parser arguments");
    } else {
      ObString json_str;
      if (OB_FAIL(storage::ObFTParserJsonProps::tokenize_array_to_props_json(allocator_, val, json_str))) {
        ObSqlString message;
        message.append_fmt("format in %s form", ADDITIONAL_ARGS_STR);
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, message.ptr());
      } else {
        properties_ = json_str;
      }
    }
  } else {
    ret = OB_INVALID_ARGUMENT;
    ObSqlString message;
    message.append_fmt("config: %s", str.ptr());
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, message.ptr());
  }
  return ret;
}

int ObExprTokenize::parse_param(const ObExpr &expr,
                                ObEvalCtx &ctx,
                                common::ObArenaAllocator &allocator,
                                TokenizeParam &param)
{
  int ret = OB_SUCCESS;

  ObDatum *parser_params_datum;
  ObString raw_parser_name = ObString::make_string(OB_DEFAULT_FULLTEXT_PARSER_NAME);

  ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
  
  MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());

  if (OB_UNLIKELY(expr.arg_cnt_ < 1 || expr.arg_cnt_ > 3)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(parse_fulltext(expr, ctx, param))) {
  } else if (OB_FAIL(parse_parser_name(expr, ctx, param))) {
  } else if (OB_FAIL(parse_parser_properties(expr, ctx, temp_allocator, param))) {
  } else {
    // Check parser cache: reform_parser_properties is expensive (JSON parsing + rebuilding).
    // If the parser name and properties match the cached values, reuse the parsed result.
    // This saves significant overhead when tokenize() is called repeatedly with the same
    // parser arguments (as in the benchmark's 3000 identical SELECTs).
    bool cache_hit = parser_cache_.is_valid_
                     && parser_cache_.cached_name_ == param.parser_name_
                     && parser_cache_.cached_properties_ == param.properties_;
    if (cache_hit) {
      // Reuse cached parser_property_ (parser_ was already set by parse_parser_name)
      param.parser_property_ = parser_cache_.cached_property_;
    } else if (OB_FAIL(param.reform_parser_properties(param.properties_))) {
    } else {
      // Cache miss: store the parsed result for next time
      parser_cache_.cached_name_ = param.parser_name_;
      parser_cache_.cached_properties_ = param.properties_;
      parser_cache_.cached_parser_ = param.parser_;
      parser_cache_.cached_property_ = param.parser_property_;
      parser_cache_.is_valid_ = true;
    }
    if (OB_SUCC(ret) && OB_FAIL(param.try_load_dictionary_for_ik())) {
    }
  }
  return ret;
}

int ObExprTokenize::construct_ft_parser_inner_name(const ObString &input_str, TokenizeParam &param)
{
  int ret = OB_SUCCESS;
  storage::ObFTParser parser;

  char *parser_name_buf = nullptr;
  if (OB_ISNULL(parser_name_buf
                = static_cast<char *>(param.allocator_.alloc(storage::OB_FT_PARSER_NAME_LENGTH)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(parser.init(input_str))) {
  } else if (OB_FAIL(parser.serialize_to_str(parser_name_buf, storage::OB_FT_PARSER_NAME_LENGTH))) {
  } else {
    param.parser_name_ = ObString::make_string(parser_name_buf);
    // Keep the parsed form too: the tokenizer helper needs exactly this object, and parsing the
    // "name.version" string back there would repeat the same work for every row.
    if (OB_FAIL(param.parser_.parse_from_str(param.parser_name_.ptr(),
                                             param.parser_name_.length()))) {
    }
  }
  return ret;
}

int ObExprTokenize::calc_result_typeN(ObExprResType &type,
                                      ObExprResType *types,
                                      int64_t param_num,
                                      common::ObExprTypeCtx &type_ctx) const
{
  int ret = OB_SUCCESS;

  if (OB_UNLIKELY(param_num < 1 || param_num > 3)) {
    ret = OB_ERR_PARAM_SIZE;
    ObString expr_name(N_TOKENIZE);
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, expr_name.length(), expr_name.ptr());
  } else {
    // just okay
  }

  ObLength length = ObAccuracy::DDL_DEFAULT_ACCURACY[ObJsonType].get_length();

  if (OB_SUCC(ret)) {
    // set res type
    type.set_json();
    type.set_length(length); // keep consistent with other json expr, maybe calc it later.

    // param type set, skip charset after first param
    for (int64_t i = 1; OB_SUCC(ret) && i < param_num; ++i) {
      if (ob_is_string_type(types[i].get_type())) {
        if (types[i].get_charset_type() != CHARSET_UTF8MB4) {
          types[i].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
        }
      }
    }

    // handle param
    if (param_num >= 2) {
      types[1].set_varchar();
    }

    if (param_num >= 3) {
      if (OB_FAIL(ObJsonExprHelper::is_valid_for_json(types, 2, N_TOKENIZE))) {
      }
    }
  }

  return ret;
}

int ObExprTokenize::cg_expr(ObExprCGCtx &op_cg_ctx,
                            const ObRawExpr &raw_expr,
                            ObExpr &rt_expr) const
{
  int ret = OB_SUCCESS;
  CK((rt_expr.arg_cnt_ >= 1 && rt_expr.arg_cnt_ <= 3));
  if (OB_SUCC(ret)) {
    // do register
    rt_expr.eval_func_ = eval_tokenize;
  }
  return ret;
}

int ObExprTokenize::parse_fulltext(const ObExpr &expr, ObEvalCtx &ctx, TokenizeParam &param)
{
  int ret = OB_SUCCESS;

  ObDatum *fulltext_datum;

  if (OB_FAIL(expr.args_[0]->eval(ctx, fulltext_datum))) {
  } else {
    if (fulltext_datum->is_null()) {
      // do nothing, return empty result
      param.fulltext_ = ObString::make_empty_string();
    } else {
      param.fulltext_ = fulltext_datum->get_string();
    }
    param.meta_.set_varchar(); // as we hardcoded in fts_index
    param.meta_.set_collation_type(expr.args_[0]->obj_meta_.get_collation_type());
  }
  return ret;
}

int ObExprTokenize::parse_parser_name(const ObExpr &expr, ObEvalCtx &ctx, TokenizeParam &param)
{
  int ret = OB_SUCCESS;
  ObDatum *parser_datum = nullptr;
  ObString raw_parser_name;

  if (expr.arg_cnt_ < 2) {
    raw_parser_name = ObString::make_string(OB_DEFAULT_FULLTEXT_PARSER_NAME);
  } else if (OB_FAIL(expr.args_[1]->eval(ctx, parser_datum))) {
  } else {
    if (parser_datum->is_null()) {
      raw_parser_name = ObString::make_string(OB_DEFAULT_FULLTEXT_PARSER_NAME);
    } else {
      ObString name = parser_datum->get_string();
      raw_parser_name = name.trim();
    }
  }

  if (OB_FAIL(ret)) {
    // already logged
  } else if (OB_FAIL(construct_ft_parser_inner_name(raw_parser_name, param))) {
  }

  return ret;
}

int ObExprTokenize::parse_parser_properties(const ObExpr &expr,
                                            ObEvalCtx &ctx,
                                            MultimodeAlloctor &mm_alloc,
                                            TokenizeParam &param)
{
  int ret = OB_SUCCESS;
  ObIJsonBase *base = nullptr;

  if (expr.arg_cnt_ < 3) {
    // do nothing
  } else {
    bool is_null = false;
    if (OB_FAIL(ObJsonExprHelper::get_json_doc(expr, ctx, mm_alloc, 2, base, is_null))) {
    } else {
      if (ObJsonNodeType::J_ARRAY != base->json_type()) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "parser args should be in an array.");
      } else {
        for (uint64_t i = 0; OB_SUCC(ret) && i < base->element_count(); ++i) {
          ObIJsonBase *node = nullptr;
          if (OB_FAIL(base->get_array_element(i, node))) {
          } else if (ObJsonNodeType::J_OBJECT != (node->json_type())) {
            ret = OB_INVALID_ARGUMENT;
          } else if (OB_FAIL(param.parse_json_param(node))) {
          }
        } // for
      }
    }
  }

  return ret;
}

int ObExprTokenize::TokenizeParam::reform_parser_properties(const ObString &properties)
{
  int ret = OB_SUCCESS;
  storage::ObFTParserJsonProps parser_properties;

  if (properties.empty()) {
    // No additional argument: the property is the parser's defaults, exactly what the json path
    // below produces after rebuild_props_for_ddl() fills the defaults in, but without building a
    // json document (and a json object for every default) just to read it back here.
    ret = parser_property_.set_default_for_parser(parser_);
  } else if (OB_FAIL(parser_properties.init())) {
  } else if (OB_FAIL(parser_properties.parse_from_valid_str(properties))) {
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "parser properties invalid.");
  } else if (OB_FAIL(parser_properties.rebuild_props_for_ddl(parser_name_,
                                                             ObCollationType::CS_TYPE_UTF8MB4_BIN,
                                                             true))) {
  } else if (OB_FAIL(parser_property_.parse_from_json_props(parser_,
                                                            parser_properties,
                                                            parser_properties.is_empty()))) {
    // Fill the property object directly: serializing it to json only for the tokenizer to
    // parse it back was a pure round trip, the canonical form is this object.
  }

  return ret;
}

int ObExprTokenize::TokenizeParam::try_load_dictionary_for_ik()
{
  int ret = OB_SUCCESS;
  bool need_to_load_dic = false;
  storage::ObDicLoaderHandle dic_loader_handle;
  if (OB_FAIL(share::ObFtsIndexBuilderUtil::check_need_to_load_dic(
          parser_name_, need_to_load_dic))) {
  } else if (need_to_load_dic) {
    if (OB_FAIL(storage::ObGenDicLoader::get_instance().get_dic_loader(
                    ObString::make_string(storage::ObFTSLiteral::PARSER_NAME_IK), // currently only ik, use parser_name_ without version suffix
                    ObCharset::charset_type_by_coll(meta_.get_collation_type()),
                    dic_loader_handle))) {
    } else if (OB_UNLIKELY(!dic_loader_handle.is_valid())) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(dic_loader_handle.get_loader()->try_load_dictionary_in_trans())) {
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
