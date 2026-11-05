/*
 * Copyright (c) 2025 OceanBase.
 * Licensed under the Apache License, Version 2.0 (the "License");
 */
#define USING_LOG_PREFIX SQL_RESV
#include "sql/resolver/dml/ob_dml_resolver.h"
#include "sql/resolver/dml/ob_dml_stmt.h"
#include "sql/resolver/expr/ob_raw_expr_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace oceanbase
{
namespace sql
{
namespace
{
int append_one_char(ObSqlString &s, char c)
{
  char b[2];
  b[0] = c;
  b[1] = '\0';
  return s.append(b, 1);
}

// 极简 CSV 解析：引号包裹、"" 转义、字段内逗号与换行、CRLF、空字段
int parse_csv_text(const char *buf, const int64_t len,
                   ObIAllocator &alloc,
                   ObIArray<ObString> &col_names,
                   ObIArray<ObSEArray<ObString, 8>> &rows,
                   int64_t &max_columns)
{
  int ret = OB_SUCCESS;
  int64_t i = 0;
  ObSEArray<ObString, 8> cur_row;
  ObSqlString cur_field;
  bool in_quote = false;
  bool first_row = true;
  max_columns = 0;

  for (i = 0; OB_SUCC(ret) && i <= len; ++i) {
    const char c = (i < len) ? buf[i] : '\n';
    const bool eof = (i == len);
    if (in_quote) {
      if (!eof && '"' == c) {
        if (i + 1 < len && '"' == buf[i + 1]) {
          if (OB_FAIL(append_one_char(cur_field, c))) {}
          ++i;
        } else {
          in_quote = false;
        }
      } else if (!eof) {
        if (OB_FAIL(append_one_char(cur_field, c))) {}
      }
      if (eof) {
        break;
      }
      continue;
    }
    if (!eof && '"' == c) {
      in_quote = true;
    } else if (eof || '\n' == c || ',' == c) {
      char *p = NULL;
      if (cur_field.empty()) {
        if (OB_ISNULL(p = static_cast<char *>(alloc.alloc(1)))) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else if (OB_FAIL(cur_row.push_back(ObString(0, p)))) {}
      } else if (OB_ISNULL(p = static_cast<char *>(alloc.alloc(cur_field.length())))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        MEMCPY(p, cur_field.ptr(), cur_field.length());
        if (OB_FAIL(cur_row.push_back(ObString(static_cast<int32_t>(cur_field.length()), p)))) {}
      }
      cur_field.reset();
      if (OB_SUCC(ret) && (eof || '\n' == c)) {
        if (first_row) {
          for (int64_t k = 0; OB_SUCC(ret) && k < cur_row.count(); ++k) {
            if (OB_FAIL(col_names.push_back(cur_row.at(k)))) {}
          }
          first_row = false;
        } else if (cur_row.count() > 0 &&
                   !(1 == cur_row.count() && cur_row.at(0).empty())) {
          ObSEArray<ObString, 8> r;
          for (int64_t k = 0; OB_SUCC(ret) && k < cur_row.count(); ++k) {
            if (OB_FAIL(r.push_back(cur_row.at(k)))) {}
          }
          if (OB_SUCC(ret)) {
            if (cur_row.count() > max_columns) {
              max_columns = cur_row.count();
            }
            if (OB_FAIL(rows.push_back(r))) {}
          }
        }
        cur_row.reset();
        if (eof) {
          break;
        }
      }
    } else if ('\r' != c) {
      if (OB_FAIL(append_one_char(cur_field, c))) {}
    }
  }
  return ret;
}
}  // namespace

int ObDMLResolver::gen_file_table_column_items(const int64_t column_cnt,
                                               const ObIArray<ObRawExprResType> &res_types,
                                               const ObIArray<ObString> &col_names,
                                               TableItem &table_item)
{
  int ret = OB_SUCCESS;
  ObValuesTableDef *table_def = table_item.values_table_def_;
  if (OB_ISNULL(params_.expr_factory_) || OB_ISNULL(allocator_) || OB_ISNULL(get_stmt()) ||
      OB_ISNULL(table_def) ||
      OB_UNLIKELY(column_cnt <= 0 || table_def->access_exprs_.empty() ||
                  table_def->access_exprs_.count() % column_cnt != 0 ||
                  res_types.count() != column_cnt || col_names.count() != column_cnt)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_INFO("gen file table column items invalid", K(ret), K(column_cnt));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < column_cnt; ++i) {
      ObColumnRefRawExpr *column_expr = NULL;
      if (OB_FAIL(params_.expr_factory_->create_raw_expr(T_REF_COLUMN, column_expr))) {
      } else if (OB_ISNULL(column_expr) || OB_ISNULL(table_def->access_exprs_.at(i))) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        column_expr->set_result_type(res_types.at(i));
        column_expr->set_result_flag(table_def->access_exprs_.at(i)->get_result_flag());
        column_expr->set_ref_id(table_item.table_id_, i + OB_APP_MIN_COLUMN_ID);
        const ObString &src_name = col_names.at(i);
        char *buf = NULL;
        if (OB_ISNULL(buf = static_cast<char *>(allocator_->alloc(src_name.length() + 1)))) {
          ret = common::OB_ALLOCATE_MEMORY_FAILED;
        } else {
          MEMCPY(buf, src_name.ptr(), src_name.length());
          buf[src_name.length()] = '\0';
          ObString column_name(static_cast<int32_t>(src_name.length()), buf);
          column_expr->set_column_attr(table_item.table_name_, column_name);
          if (OB_FAIL(column_expr->add_flag(IS_COLUMN))) {
          } else {
            ColumnItem column_item;
            column_item.expr_ = column_expr;
            column_item.table_id_ = column_expr->get_table_id();
            column_item.column_id_ = column_expr->get_column_id();
            column_item.column_name_ = column_expr->get_column_name();
            if (OB_FAIL(get_stmt()->add_column_item(column_item))) {
            }
          }
        }
      }
    }
  }
  return ret;
}

int ObDMLResolver::resolve_file_table_item(const ParseNode &parse_tree, TableItem *&table_item)
{
  int ret = OB_SUCCESS;
  ObDMLStmt *stmt = get_stmt();
  TableItem *item = NULL;
  ObValuesTableDef *table_def = NULL;
  ObString file_path;
  char *file_data = NULL;
  int64_t file_size = 0;
  ObSEArray<ObString, 8> col_names;
  ObSEArray<ObSEArray<ObString, 8>, 8> rows;
  int64_t max_columns = 0;
  ObSEArray<ObObjType, 8> col_types;
  const int64_t MAX_FILE_SIZE = 64LL * 1024 * 1024;

  if (OB_ISNULL(stmt) || OB_ISNULL(allocator_) || OB_ISNULL(params_.expr_factory_) || OB_ISNULL(session_info_)) {
    ret = OB_NOT_INIT;
  } else if (T_FILE_TABLE != parse_tree.type_ || parse_tree.num_child_ < 2) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    bool enable_file_sql = false;
    ObObj var_obj;
    ObString var_name("enable_file_sql");
    if (OB_FAIL(session_info_->get_sys_variable_by_name(var_name, var_obj))) {
    } else if (OB_FALSE_IT(enable_file_sql = var_obj.get_bool())) {
    } else if (!enable_file_sql) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "file sql is disabled, set session enable_file_sql = on first");
    } else {
      // 文件表在 resolve 阶段就把文件内容读成常量表；若计划进入全局缓存，
      // 不同文件路径/文件变更都会复用旧计划（读到旧数据）。
      // 等价于用户写了 NO_USE_PLAN_CACHE hint：本条语句既不写入也不命中计划缓存。
      // ObQueryCtx::get_global_hint() 只提供 const 引用，但该对象本身是可写的
      ObGlobalHint &global_hint = const_cast<ObGlobalHint &>(params_.query_ctx_->get_global_hint());
      global_hint.merge_plan_cache_hint(ObPlanCachePolicy::OB_USE_PLAN_CACHE_NONE);
    }
  }

  if (OB_FAIL(ret)) {
  } else {
    const ParseNode *arg_list = parse_tree.children_[1];
    const ParseNode *path_node = NULL;
    if (OB_ISNULL(arg_list)) {
      ret = OB_INVALID_ARGUMENT;
    } else if (T_EXPR_LIST == arg_list->type_ || T_LINK_NODE == arg_list->type_) {
      if (arg_list->num_child_ < 1 || OB_ISNULL(path_node = arg_list->children_[0])) {
        ret = OB_INVALID_ARGUMENT;
      }
    } else {
      path_node = arg_list;
    }
    if (OB_FAIL(ret)) {
    } else if (OB_ISNULL(path_node->str_value_) || path_node->str_len_ <= 0) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      file_path.assign_ptr(path_node->str_value_, static_cast<int32_t>(path_node->str_len_));
      // 参数处理：第 1 个是路径；其余为可选参数
      //   字符串字面量 -> 格式名（csv/json/parquet），其他 1064
      //   命名参数 header=<bool> 接受；sample_size=-1 接受；其他命名参数/取值 -> 1235
      int64_t total_args = (T_LINK_NODE == arg_list->type_) ? 0 : arg_list->num_child_;
      if (T_LINK_NODE == arg_list->type_) {
        const ParseNode *walk = arg_list;
        while (OB_NOT_NULL(walk)) { ++total_args; walk = walk->children_[1]; }
      }
      for (int64_t ai = 1; OB_SUCC(ret) && ai < total_args; ++ai) {
        const ParseNode *anode = NULL;
        if (T_LINK_NODE == arg_list->type_) {
          const ParseNode *walk = arg_list;
          for (int64_t k = 0; OB_NOT_NULL(walk) && k < ai; ++k) { walk = walk->children_[1]; }
          if (OB_NOT_NULL(walk)) { anode = walk->children_[0]; }
        } else {
          anode = arg_list->children_[ai];
        }
        if (OB_ISNULL(anode)) { continue; }
        if (OB_NOT_NULL(anode->str_value_) && anode->str_len_ > 0) {
          ObString fmt(anode->str_len_, anode->str_value_);
          if (0 != fmt.case_compare("csv") && 0 != fmt.case_compare("json")
              && 0 != fmt.case_compare("parquet")) {
            ret = OB_ERR_PARSE_SQL;  // 1064
            LOG_WARN("unsupported file format", K(fmt), K(ret));
          }
        } else if (2 == anode->num_child_ && OB_NOT_NULL(anode->children_[0])
                   && OB_NOT_NULL(anode->children_[0]->str_value_)) {
          ObString pname(anode->children_[0]->str_len_, anode->children_[0]->str_value_);
          const ParseNode *pval = anode->children_[1];
          if (0 == pname.case_compare("header")) {
            // 首行固定当表头，header=true/false 均接受
          } else if (0 == pname.case_compare("sample_size")) {
            // -1 在语法树里可能是 T_INT(-1)，也可能是 一元负号(T_INT(1))
            bool is_minus_one = false;
            if (OB_NOT_NULL(pval)) {
              if (OB_NOT_NULL(pval->str_value_) && pval->str_len_ > 0) {
                // 形如 "-1" 的文本字面量
                ObString txt(pval->str_len_, pval->str_value_);
                if (0 == txt.compare("-1")) {
                  is_minus_one = true;
                }
              } else if (T_INT == pval->type_ && -1 == pval->value_) {
                is_minus_one = true;
              } else if (1 == pval->num_child_ && OB_NOT_NULL(pval->children_[0])
                         && T_INT == pval->children_[0]->type_
                         && 1 == pval->children_[0]->value_) {
                is_minus_one = true;
              }
            }
            if (!is_minus_one) {
              LOG_INFO("sample_size_ast", "null", pval == NULL,
                       "type", pval == NULL ? -1 : static_cast<int>(pval->type_),
                       "nchild", pval == NULL ? -1 : static_cast<int>(pval->num_child_),
                       "c0type", (pval == NULL || pval->num_child_ < 1 || pval->children_[0] == NULL)
                                 ? -1 : static_cast<int>(pval->children_[0]->type_),
                       "c0val", (pval == NULL || pval->num_child_ < 1 || pval->children_[0] == NULL)
                                 ? -1 : static_cast<int>(pval->children_[0]->value_),
                       "val", pval == NULL ? -1 : static_cast<int>(pval->value_));
              ret = OB_NOT_SUPPORTED;  // 1235
              LOG_USER_ERROR(OB_NOT_SUPPORTED, "read_csv sample_size (only -1 is supported)");
            }
          } else {
            ret = OB_NOT_SUPPORTED;
            LOG_USER_ERROR(OB_NOT_SUPPORTED, "read_csv named parameter");
          }
        } else {
          ret = OB_NOT_SUPPORTED;
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "read_csv argument");
        }
      }
    }
  }
  if (OB_FAIL(ret)) {
  } else {
    FILE *fp = fopen(file_path.ptr(), "rb");
    if (NULL == fp) {
      ret = OB_FILE_NOT_EXIST;
    } else {
      if (0 != fseek(fp, 0, SEEK_END) || (file_size = ftell(fp)) < 0 || file_size > MAX_FILE_SIZE) {
        ret = OB_SIZE_OVERFLOW;
      } else if (OB_ISNULL(file_data = static_cast<char *>(allocator_->alloc(file_size + 1)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        rewind(fp);
        if (file_size != static_cast<int64_t>(fread(file_data, 1, file_size, fp))) {
          ret = OB_IO_ERROR;
        } else {
          file_data[file_size] = '\0';
        }
      }
      fclose(fp);
    }
  }

  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(parse_csv_text(file_data, file_size, *allocator_, col_names, rows, max_columns))) {
  } else if (col_names.empty()) {
    ret = OB_INVALID_ARGUMENT;
  }

  // 重复列名处理：第二个同名列改名 name__2、name__3 ...
  if (OB_SUCC(ret)) {
    for (int64_t c = 0; OB_SUCC(ret) && c < col_names.count(); ++c) {
      int64_t dup = 1;
      for (int64_t k = 0; k < c; ++k) {
        if (0 == col_names.at(k).case_compare(col_names.at(c))) {
          ++dup;
        }
      }
      if (dup > 1) {
        ObSqlString new_name;
        if (OB_FAIL(new_name.append_fmt("%.*s__%ld", col_names.at(c).length(),
                                        col_names.at(c).ptr(), dup))) {
        } else {
          char *p2 = static_cast<char *>(allocator_->alloc(new_name.length() + 1));
          if (OB_ISNULL(p2)) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else {
            MEMCPY(p2, new_name.ptr(), new_name.length());
            p2[new_name.length()] = '\0';
            col_names.at(c).assign_ptr(p2, static_cast<int32_t>(new_name.length()));
          }
        }
      }
    }
  }

  // 类型推断：整列全是整数 -> INT；全是浮点 -> DOUBLE；否则 VARCHAR
  if (OB_FAIL(ret)) {
  } else {
    for (int64_t c = 0; OB_SUCC(ret) && c < col_names.count(); ++c) {
      bool all_int = true, all_double = true, all_bool = true, has_value = false;
      for (int64_t r = 0; OB_SUCC(ret) && r < rows.count(); ++r) {
        if (c >= rows.at(r).count()) continue;
        ObString cell = rows.at(r).at(c);
        if (cell.empty()) continue;
        has_value = true;
        char tmp[64];
        int64_t n = cell.length() < 63 ? cell.length() : 63;
        MEMCPY(tmp, cell.ptr(), n);
        tmp[n] = '\0';
        char *end = NULL;
        errno = 0;
        (void)strtoll(tmp, &end, 10);
        if (NULL == end || *end != '\0' || 0 != errno) all_int = false;
        errno = 0;
        (void)strtod(tmp, &end);
        if (NULL == end || *end != '\0' || 0 != errno) all_double = false;
        if (0 != cell.case_compare("true") && 0 != cell.case_compare("false")) all_bool = false;
      }
      ObObjType t = ObVarcharType;
      if (has_value && all_bool) {
        t = ObTinyIntType;
      } else if (has_value && all_int) {
        t = ObIntType;
      } else if (has_value && all_double) {
        t = ObDoubleType;
      }
      if (OB_FAIL(col_types.push_back(t))) {}
    }
  }

  if (OB_FAIL(ret)) {
  } else if (OB_ISNULL(item = stmt->create_table_item(*allocator_))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_ISNULL(table_def = static_cast<ObValuesTableDef *>(allocator_->alloc(sizeof(ObValuesTableDef))))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    table_def = new (table_def) ObValuesTableDef();
    item->values_table_def_ = table_def;
    table_def->column_cnt_ = col_names.count();
  }

  // 列类型：用“探针常量表达式”推导，保证与真实表达式完全一致
  if (OB_FAIL(ret)) {
  } else {
    for (int64_t c = 0; OB_SUCC(ret) && c < col_types.count(); ++c) {
      const ObObjType t = col_types.at(c);
      ObString sample;
      for (int64_t r = 0; OB_SUCC(ret) && r < rows.count(); ++r) {
        if (c < rows.at(r).count() && !rows.at(r).at(c).empty()) {
          sample = rows.at(r).at(c);
          break;
        }
      }
      ObConstRawExpr *probe = NULL;
      ObObj v;
      ObItemType probe_type = T_VARCHAR;
      // 日期/时间统一成 'YYYY-MM-DD HH:MM:SS' 形式（把 'T' 换成空格）
      ObSqlString norm_sample;
      if (ObVarcharType == t && sample.length() > 10 && 'T' == sample.ptr()[10]) {
        if (OB_FAIL(norm_sample.append(sample.ptr(), 10))) {
        } else if (OB_FAIL(norm_sample.append(" "))) {
        } else if (OB_FAIL(norm_sample.append(sample.ptr() + 11, sample.length() - 11))) {
        } else {
          sample.assign_ptr(norm_sample.ptr(), static_cast<int32_t>(norm_sample.length()));
        }
      }
      if (ObTinyIntType == t) {
        probe_type = T_INT;
        v.set_tinyint(0 == sample.case_compare("true") ? 1 : 0);
      } else if (ObIntType == t) {
        probe_type = T_INT;
        char tmp[64];
        int64_t n = sample.length() < 63 ? sample.length() : 63;
        MEMCPY(tmp, sample.ptr(), n);
        tmp[n] = '\0';
        v.set_int(strtoll(tmp, NULL, 10));
      } else if (ObDoubleType == t) {
        probe_type = T_NUMBER;
        char tmp[64];
        int64_t n = sample.length() < 63 ? sample.length() : 63;
        MEMCPY(tmp, sample.ptr(), n);
        tmp[n] = '\0';
        v.set_double(strtod(tmp, NULL));
      } else {
        probe_type = T_VARCHAR;
        char *p = static_cast<char *>(allocator_->alloc(sample.length() + 1));
        if (OB_ISNULL(p)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          MEMCPY(p, sample.ptr(), sample.length());
          p[sample.length()] = '\0';
          v.set_string(ObVarcharType, ObString(static_cast<int32_t>(sample.length()), p));
          v.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
        }
      }
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(params_.expr_factory_->create_raw_expr(probe_type, probe))) {
      } else if (OB_ISNULL(probe)) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        probe->set_value(v);
        if (OB_FAIL(probe->formalize(params_.session_info_))) {
        } else if (OB_FAIL(table_def->column_types_.push_back(probe->get_result_type()))) {
        }
      }
    }
  }

  // 行数据
  if (OB_FAIL(ret)) {
  } else {
    for (int64_t r = 0; OB_SUCC(ret) && r < rows.count(); ++r) {
      for (int64_t c = 0; OB_SUCC(ret) && c < col_names.count(); ++c) {
        ObConstRawExpr *const_expr = NULL;
        ObObj value;
        ObString cell;
        if (c < rows.at(r).count()) cell = rows.at(r).at(c);
        const ObObjType t = col_types.at(c);
        ObItemType cell_type = T_VARCHAR;
        // 日期/时间归一化（'T' -> 空格）
        ObSqlString norm_cell;
        if (!cell.empty() && cell.length() > 10 && 'T' == cell.ptr()[10]) {
          if (OB_SUCC(norm_cell.append(cell.ptr(), 10))
              && OB_SUCC(norm_cell.append(" "))
              && OB_SUCC(norm_cell.append(cell.ptr() + 11, cell.length() - 11))) {
            char *pc = static_cast<char *>(allocator_->alloc(norm_cell.length() + 1));
            if (OB_NOT_NULL(pc)) {
              MEMCPY(pc, norm_cell.ptr(), norm_cell.length());
              pc[norm_cell.length()] = '\0';
              cell.assign_ptr(pc, static_cast<int32_t>(norm_cell.length()));
            }
          }
        }
        if (cell.empty()) {
          cell_type = T_NULL;
          value.set_null();
        } else if (ObTinyIntType == t) {
          cell_type = T_INT;
          value.set_tinyint(0 == cell.case_compare("true") ? 1 : 0);
        } else if (ObIntType == t) {
          cell_type = T_INT;
          char tmp[64];
          int64_t n = cell.length() < 63 ? cell.length() : 63;
          MEMCPY(tmp, cell.ptr(), n);
          tmp[n] = '\0';
          value.set_int(strtoll(tmp, NULL, 10));
        } else if (ObDoubleType == t) {
          cell_type = T_NUMBER;
          char tmp[64];
          int64_t n = cell.length() < 63 ? cell.length() : 63;
          MEMCPY(tmp, cell.ptr(), n);
          tmp[n] = '\0';
          value.set_double(strtod(tmp, NULL));
        } else {
          cell_type = T_VARCHAR;
          char *p = static_cast<char *>(allocator_->alloc(cell.length() + 1));
          if (OB_ISNULL(p)) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else {
            MEMCPY(p, cell.ptr(), cell.length());
            p[cell.length()] = '\0';
            value.set_string(ObVarcharType, ObString(static_cast<int32_t>(cell.length()), p));
            value.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
          }
        }
        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(params_.expr_factory_->create_raw_expr(cell_type, const_expr))) {
        } else if (OB_ISNULL(const_expr)) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          const_expr->set_value(value);
          if (OB_FAIL(const_expr->formalize(params_.session_info_))) {
          } else if (OB_FAIL(table_def->access_exprs_.push_back(const_expr))) {
          }
        }
      }
    }
  }

  // 三步收尾：逐个记录返回值（INFO 追踪）
  if (OB_FAIL(ret)) {
  } else {
    // 跳过 try_add_cast_to_values（该构造下恒返回 -4016），手工设置行数与列统计
    table_def->row_cnt_ = rows.count();
    table_def->access_type_ = ObValuesTableDef::ACCESS_EXPR;
    table_def->is_const_ = false;
    for (int64_t c = 0; OB_SUCC(ret) && c < table_def->column_cnt_; ++c) {
      int64_t null_cnt = 0;
      for (int64_t r = 0; OB_SUCC(ret) && r < rows.count(); ++r) {
        if (c >= rows.at(r).count() || rows.at(r).at(c).empty()) {
          ++null_cnt;
        }
      }
      if (OB_FAIL(table_def->column_ndvs_.push_back(rows.count() > 0 ? rows.count() : 1))) {
      } else if (OB_FAIL(table_def->column_nnvs_.push_back(null_cnt))) {
      }
    }
    LOG_INFO("STEP_TRACE_manual_stats", K(ret), K(table_def->row_cnt_),
             "cols", table_def->column_cnt_,
             "ndvs", table_def->column_ndvs_.count(),
             "nnvs", table_def->column_nnvs_.count());
    LOG_INFO("file sql build", K(ret), "cols", table_def->column_cnt_, "rows", rows.count(),
             "col_types", table_def->column_types_.count(), "access_exprs", table_def->access_exprs_.count());
  }

  if (OB_FAIL(ret)) {
  } else {
    ObString table_name("read_csv");
    ObString alias_name = table_name;
    if (parse_tree.num_child_ >= 3 && OB_NOT_NULL(parse_tree.children_[2])
        && OB_NOT_NULL(parse_tree.children_[2]->str_value_)
        && parse_tree.children_[2]->str_len_ > 0) {
      alias_name.assign_ptr(parse_tree.children_[2]->str_value_,
                            static_cast<int32_t>(parse_tree.children_[2]->str_len_));
    }
    item->table_id_ = generate_table_id();
    item->table_name_ = table_name;
    item->alias_name_ = alias_name;
    item->type_ = TableItem::VALUES_TABLE;
    item->is_view_table_ = false;
    if (OB_FAIL(stmt->add_table_item(session_info_, item))) {
    } else if (OB_FAIL(gen_file_table_column_items(table_def->column_cnt_,
                                                   table_def->column_types_,
                                                   col_names,
                                                   *item))) {
    } else {
      table_item = item;
    }
  }
  return ret;
}

}  // namespace sql
}  // namespace oceanbase
