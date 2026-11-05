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
#include <dirent.h>

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

// ---------- JSONL（每行一个扁平对象）----------
static int parse_jsonl_text(const char *buf, const int64_t len, ObIAllocator &alloc,
                            ObIArray<ObString> &col_names,
                            ObIArray<ObSEArray<ObString, 8>> &rows)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObString, 8> header_keys;
  int64_t pos = 0;
  while (OB_SUCC(ret) && pos < len) {
    // 取一行
    int64_t line_end = pos;
    while (line_end < len && '\n' != buf[line_end]) { ++line_end; }
    const char *line = buf + pos;
    const int64_t line_len = line_end - pos;
    pos = line_end + 1;
    if (line_len <= 1) { continue; }  // 空行

    // 临时存本行 key/value
    ObSEArray<ObString, 8> keys;
    ObSEArray<ObString, 8> vals;
    int64_t i = 0;
    while (OB_SUCC(ret) && i < line_len) {
      if ('"' != line[i]) { ++i; continue; }
      // key
      int64_t k0 = ++i;
      while (i < line_len && '"' != line[i]) { ++i; }
      if (i >= line_len) { break; }
      ObString key(static_cast<int32_t>(i - k0), const_cast<char *>(line + k0));
      ++i;
      while (i < line_len && ':' != line[i]) { ++i; }
      if (i >= line_len) { break; }
      ++i;
      while (i < line_len && ' ' == line[i]) { ++i; }
      // value
      ObSqlString vtext;
      if (i < line_len && '"' == line[i]) {
        ++i;
        while (i < line_len && '"' != line[i]) {
          if ('\\' == line[i] && i + 1 < line_len) { ++i; }
          if (OB_FAIL(append_one_char(vtext, line[i]))) { break; }
          ++i;
        }
        ++i;
      } else {
        int64_t v0 = i;
        while (i < line_len && ',' != line[i] && '}' != line[i]) { ++i; }
        int64_t v1 = i;
        while (v1 > v0 && ' ' == line[v1 - 1]) { --v1; }
        ObString raw(static_cast<int32_t>(v1 - v0), const_cast<char *>(line + v0));
        if (0 == raw.case_compare("true")) {
          if (OB_FAIL(vtext.append("1"))) {}
        } else if (0 == raw.case_compare("false")) {
          if (OB_FAIL(vtext.append("0"))) {}
        } else if (0 == raw.case_compare("null")) {
          // 空
        } else if (OB_FAIL(vtext.append(raw.ptr(), raw.length()))) {
        }
      }
      // 拷贝 key/value 到 allocator
      char *kp = static_cast<char *>(alloc.alloc(key.length() + 1));
      char *vp = static_cast<char *>(alloc.alloc(vtext.length() + 1));
      if (OB_ISNULL(kp) || OB_ISNULL(vp)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        MEMCPY(kp, key.ptr(), key.length());
        kp[key.length()] = '\0';
        MEMCPY(vp, vtext.ptr(), vtext.length());
        vp[vtext.length()] = '\0';
        if (OB_FAIL(keys.push_back(ObString(static_cast<int32_t>(key.length()), kp)))) {
        } else if (OB_FAIL(vals.push_back(ObString(static_cast<int32_t>(vtext.length()), vp)))) {
        }
      }
    }

    // 把新出现的 key 追加到 union 列表
    if (OB_SUCC(ret)) {
      for (int64_t k = 0; OB_SUCC(ret) && k < keys.count(); ++k) {
        bool found = false;
        for (int64_t h = 0; !found && h < header_keys.count(); ++h) {
          if (0 == header_keys.at(h).case_compare(keys.at(k))) { found = true; }
        }
        if (!found && OB_FAIL(header_keys.push_back(keys.at(k)))) {
        }
      }
    }
    // 本行按 union 顺序填充（当前 union 大小），后续补空
    if (OB_SUCC(ret)) {
      ObSEArray<ObString, 8> r;
      for (int64_t h = 0; OB_SUCC(ret) && h < header_keys.count(); ++h) {
        ObString v;
        for (int64_t k = 0; k < keys.count(); ++k) {
          if (0 == header_keys.at(h).case_compare(keys.at(k))) { v = vals.at(k); break; }
        }
        if (OB_FAIL(r.push_back(v))) {}
      }
      if (OB_FAIL(rows.push_back(r))) {}
    }
  }

  // 行数对齐到最终 union 大小（补齐 NULL）
  if (OB_SUCC(ret)) {
    for (int64_t h = 0; OB_SUCC(ret) && h < header_keys.count(); ++h) {
      if (OB_FAIL(col_names.push_back(header_keys.at(h)))) {}
    }
    for (int64_t r = 0; OB_SUCC(ret) && r < rows.count(); ++r) {
      while (rows.at(r).count() < col_names.count()) {
        ObString empty;
        if (OB_FAIL(rows.at(r).push_back(empty))) {}
      }
    }
  }
  return ret;
}

// ---------- 极简 Parquet 读取（PLAIN INT32/INT64，UNCOMPRESSED/SNAPPY，DATA_PAGE / DATA_PAGE_V2）----------
struct PfBuf { const uint8_t *b; int64_t len; int64_t p; };

static int pf_varint(PfBuf &r, uint64_t &out)
{
  uint64_t res = 0; int shift = 0; int ret = OB_SUCCESS;
  while (r.p < r.len) {
    uint8_t x = r.b[r.p++];
    res |= (uint64_t)(x & 0x7f) << shift;
    if (!(x & 0x80)) { out = res; return ret; }
    shift += 7;
    if (shift > 63) break;
  }
  return OB_ERR_UNEXPECTED;
}

static int pf_zigzag(PfBuf &r, int64_t &out)
{
  uint64_t v = 0;
  int ret = pf_varint(r, v);
  if (OB_SUCC(ret)) {
    out = (v & 1) ? -(int64_t)((v >> 1) + 1) : (int64_t)(v >> 1);
  }
  return ret;
}

static int pf_skip(PfBuf &r, int type);

static int pf_skip_struct(PfBuf &r)
{
  int ret = OB_SUCCESS;
  int64_t last = 0;
  while (OB_SUCC(ret) && r.p < r.len) {
    uint8_t h = r.b[r.p++];
    if (0 == h) { break; }
    int type = h & 0x0f;
    int delta = (h >> 4) & 0x0f;
    int64_t fid = delta ? last + delta : 0;
    if (!delta) { if (OB_FAIL(pf_zigzag(r, fid))) { break; } }
    last = fid;
    if (OB_FAIL(pf_skip(r, type))) { break; }
  }
  return ret;
}

static int pf_skip(PfBuf &r, int type)
{
  int ret = OB_SUCCESS;
  int64_t tmp = 0;
  switch (type) {
    case 1: case 2: break;  // compact bool：类型本身即值，无 payload
    case 3: r.p += 1; break;
    case 4: case 5: case 6: if (OB_FAIL(pf_zigzag(r, tmp))) {} break;
    case 7: r.p += 8; break;
    case 8: { uint64_t n = 0; if (OB_FAIL(pf_varint(r, n))) {} else { r.p += (int64_t)n; } break; }
    case 9: case 10: {
      uint8_t h = r.b[r.p++]; int64_t n = h >> 4; int et = h & 0x0f;
      if (15 == n) { uint64_t nn = 0; if (OB_FAIL(pf_varint(r, nn))) {} else { n = (int64_t)nn; } }
      for (int64_t i = 0; OB_SUCC(ret) && i < n; ++i) { if (OB_FAIL(pf_skip(r, et))) {} }
      break;
    }
    case 11: { r.p += 1; uint8_t h = r.b[r.p++]; int64_t n = h >> 4; int kt = h & 0x0f; int vt = (h >> 4) & 0x0f;
      for (int64_t i = 0; OB_SUCC(ret) && i < n; ++i) { if (OB_FAIL(pf_skip(r, kt))) {} else if (OB_FAIL(pf_skip(r, vt))) {} }
      break; }
    case 12: if (OB_FAIL(pf_skip_struct(r))) {} break;
    default: ret = OB_ERR_UNEXPECTED; break;
  }
  return ret;
}

// snappy 解压
static int snappy_decompress(const uint8_t *in, int64_t in_len, uint8_t *out, int64_t out_cap, int64_t &out_len)
{
  int ret = OB_SUCCESS;
  PfBuf r{in, in_len, 0};
  uint64_t want = 0;
  if (OB_FAIL(pf_varint(r, want))) {
  } else if (want > (uint64_t)out_cap) {
    ret = OB_SIZE_OVERFLOW;
  } else {
    int64_t op = 0;
    while (OB_SUCC(ret) && r.p < r.len && op < (int64_t)want) {
      uint8_t tag = r.b[r.p++];
      int t = tag & 0x03;
      if (0 == t) {
        int64_t len = (tag >> 2);
        if (len < 60) { len += 1; }
        else { int nb = (int)(len - 59); len = 0;
          for (int i = 0; i < nb && r.p < r.len; ++i) { len |= ((int64_t)r.b[r.p++]) << (8 * i); }
          len += 1;
        }
        if (r.p + len > r.len || op + len > (int64_t)want) { ret = OB_ERR_UNEXPECTED; break; }
        MEMCPY(out + op, r.b + r.p, len); r.p += len; op += len;
      } else {
        int64_t len = 0, off = 0;
        if (1 == t) {
          len = ((tag >> 2) & 0x07) + 4;
          off = ((int64_t)(tag >> 5) << 8) | r.b[r.p++];
        } else if (2 == t) {
          len = (tag >> 2) + 1;
          off = (int64_t)r.b[r.p] | ((int64_t)r.b[r.p + 1] << 8); r.p += 2;
        } else {
          len = (tag >> 2) + 1;
          off = (int64_t)r.b[r.p] | ((int64_t)r.b[r.p + 1] << 8) | ((int64_t)r.b[r.p + 2] << 16) | ((int64_t)r.b[r.p + 3] << 24);
          r.p += 4;
        }
        if (off <= 0 || off > op || op + len > (int64_t)want) { ret = OB_ERR_UNEXPECTED; break; }
        for (int64_t i = 0; i < len; ++i) { out[op + i] = out[op - off + i]; }
        op += len;
      }
    }
    if (OB_SUCC(ret)) { out_len = op; }
  }
  return ret;
}

// 解析 parquet 文件 -> 单列（或多列）INT 值；此处只支持单列 PLAIN INT32/INT64
static int read_parquet_single_column(const char *path, ObIAllocator &alloc,
                                      ObString &col_name, ObIArray<int64_t> &values)
{
  int ret = OB_SUCCESS;
  FILE *fp = fopen(path, "rb");
  uint8_t *data = NULL;
  int64_t size = 0;
  if (NULL == fp) {
    ret = OB_FILE_NOT_EXIST;
  } else {
    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    rewind(fp);
    if (size < 12) {
      ret = OB_INVALID_ARGUMENT;
    } else if (OB_ISNULL(data = static_cast<uint8_t *>(alloc.alloc(size)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (size != (int64_t)fread(data, 1, size, fp)) {
      ret = OB_IO_ERROR;
    }
    fclose(fp);
  }
  if (OB_FAIL(ret)) {
    return ret;
  }
  // footer
  const uint32_t meta_len = (uint32_t)data[size - 8] | ((uint32_t)data[size - 7] << 8)
                            | ((uint32_t)data[size - 6] << 16) | ((uint32_t)data[size - 5] << 24);
  const int64_t meta_start = size - 8 - meta_len;
  if (meta_start <= 0) {
    return OB_INVALID_ARGUMENT;
  }
  PfBuf r{data, size, meta_start};
  int64_t num_rows = 0;
  int32_t col_type = -1;
  int32_t codec = -1;
  int64_t num_values = 0;
  int64_t data_off = -1;
  ObString name;
  int64_t last = 0;
  // 顶层 struct
  while (OB_SUCC(ret) && r.p < r.len) {
    uint8_t h = data[r.p++];
    if (0 == h) { break; }
    int type = h & 0x0f;
    int delta = (h >> 4) & 0x0f;
    int64_t fid = delta ? last + delta : 0;
    if (!delta) { if (OB_FAIL(pf_zigzag(r, fid))) { break; } }
    last = fid;
    if (2 == fid && 9 == type) {           // schema list
      uint8_t hh = data[r.p++]; int64_t n = hh >> 4; int et = hh & 0x0f;
      if (15 == n) { uint64_t nn = 0; pf_varint(r, nn); n = (int64_t)nn; }
      for (int64_t i = 0; i < n; ++i) {
        int64_t flast = 0;
        int64_t slevel = r.p;
        while (r.p < r.len) {
          uint8_t fh = data[r.p++];
          if (0 == fh) { break; }
          int ft = fh & 0x0f; int fd = (fh >> 4) & 0x0f;
          int64_t ffid = fd ? flast + fd : 0;
          if (!fd) { pf_zigzag(r, ffid); }
          flast = ffid;
          if (4 == ffid && 8 == ft) {
            uint64_t sl = 0; pf_varint(r, sl);
            ObString s(static_cast<int32_t>(sl), reinterpret_cast<char *>(data + r.p));
            r.p += sl;
            if (i > 0 && name.empty()) { name = s; }
          } else if (1 == ffid && 5 == ft) {
            int64_t v = 0; pf_zigzag(r, v); if (i > 0) { col_type = (int32_t)v; }
          } else if (OB_FAIL(pf_skip(r, ft))) { break; }
        }
        if (OB_FAIL(ret)) { break; }
        (void)slevel;
      }
    } else if (3 == fid && 6 == type) {
      if (OB_FAIL(pf_zigzag(r, num_rows))) {}
    } else if (4 == fid && 9 == type) {     // row_groups
      uint8_t hh = data[r.p++]; int64_t n = hh >> 4; int et = hh & 0x0f;
      if (15 == n) { uint64_t nn = 0; pf_varint(r, nn); n = (int64_t)nn; }
      for (int64_t i = 0; OB_SUCC(ret) && i < n; ++i) {
        int64_t glast = 0;
        while (OB_SUCC(ret) && r.p < r.len) {
          uint8_t gh = data[r.p++];
          if (0 == gh) { break; }
          int gt = gh & 0x0f; int gd = (gh >> 4) & 0x0f;
          int64_t gfid = gd ? glast + gd : 0;
          if (!gd) { pf_zigzag(r, gfid); }
          glast = gfid;
          if (1 == gfid && 9 == gt) {       // columns
            uint8_t ch = data[r.p++]; int64_t cn = ch >> 4; int cet = ch & 0x0f;
            if (15 == cn) { uint64_t nn = 0; pf_varint(r, nn); cn = (int64_t)nn; }
            for (int64_t ci = 0; OB_SUCC(ret) && ci < cn; ++ci) {
              int64_t clast = 0;
              while (OB_SUCC(ret) && r.p < r.len) {
                uint8_t cch = data[r.p++];
                if (0 == cch) { break; }
                int ct = cch & 0x0f; int cd = (cch >> 4) & 0x0f;
                int64_t cfid = cd ? clast + cd : 0;
                if (!cd) { pf_zigzag(r, cfid); }
                clast = cfid;
                if (3 == cfid && 12 == ct) {   // ColumnMetaData
                  int64_t mlast = 0;
                  while (OB_SUCC(ret) && r.p < r.len) {
                    uint8_t mh = data[r.p++];
                    if (0 == mh) { break; }
                    int mt = mh & 0x0f; int md = (mh >> 4) & 0x0f;
                    int64_t mfid = md ? mlast + md : 0;
                    if (!md) { pf_zigzag(r, mfid); }
                    mlast = mfid;
                    if (1 == mfid && 5 == mt) { int64_t v = 0; pf_zigzag(r, v); col_type = (int32_t)v; }
                    else if (4 == mfid && 5 == mt) { int64_t v = 0; pf_zigzag(r, v); codec = (int32_t)v; }
                    else if (5 == mfid && 6 == mt) { pf_zigzag(r, num_values); }
                    else if (9 == mfid && 6 == mt) { pf_zigzag(r, data_off); }
                    else if (OB_FAIL(pf_skip(r, mt))) { break; }
                  }
                } else if (OB_FAIL(pf_skip(r, ct))) { break; }
              }
            }
          } else if (OB_FAIL(pf_skip(r, gt))) { break; }
        }
      }
    } else if (OB_FAIL(pf_skip(r, type))) {
      break;
    }
  }
  if (OB_FAIL(ret)) {
    return ret;
  }
  if (OB_ISNULL(name.ptr()) || name.empty()) {
    ObString def("c1");
    name = def;
  }
  col_name = name;

  // 解析 page header
  PfBuf pr{data, size, data_off};
  int32_t page_type = -1;
  int64_t uncomp_size = 0, comp_size = 0, page_vals = 0, def_len = 0, rep_len = 0;
  int32_t encoding = 0;
  int64_t pl = 0;
  while (OB_SUCC(ret) && pr.p < pr.len) {
    uint8_t h = data[pr.p++];
    if (0 == h) { break; }
    int type = h & 0x0f; int delta = (h >> 4) & 0x0f;
    int64_t fid = delta ? pl + delta : 0;
    if (!delta) { pf_zigzag(pr, fid); }
    pl = fid;
    if (1 == fid && 5 == type) { int64_t v = 0; pf_zigzag(pr, v); page_type = (int32_t)v; }
    else if (2 == fid && 5 == type) { pf_zigzag(pr, uncomp_size); }
    else if (3 == fid && 5 == type) { pf_zigzag(pr, comp_size); }
    else if (5 == fid && 12 == type) {          // DataPageHeader v1
      int64_t dlast = 0;
      while (OB_SUCC(ret) && pr.p < pr.len) {
        uint8_t dh = data[pr.p++];
        if (0 == dh) { break; }
        int dt = dh & 0x0f; int dd = (dh >> 4) & 0x0f;
        int64_t dfid = dd ? dlast + dd : 0;
        if (!dd) { pf_zigzag(pr, dfid); }
        dlast = dfid;
        if (1 == dfid && 5 == dt) { pf_zigzag(pr, page_vals); }
        else if (2 == dfid && 5 == dt) { int64_t v = 0; pf_zigzag(pr, v); encoding = (int32_t)v; }
        else if (OB_FAIL(pf_skip(pr, dt))) { break; }
      }
    } else if (8 == fid && 12 == type) {        // DataPageHeader v2
      int64_t dlast = 0;
      while (OB_SUCC(ret) && pr.p < pr.len) {
        uint8_t dh = data[pr.p++];
        if (0 == dh) { break; }
        int dt = dh & 0x0f; int dd = (dh >> 4) & 0x0f;
        int64_t dfid = dd ? dlast + dd : 0;
        if (!dd) { pf_zigzag(pr, dfid); }
        dlast = dfid;
        if (1 == dfid && 5 == dt) { pf_zigzag(pr, page_vals); }
        else if (3 == dfid && 5 == dt) { /* num_rows */ int64_t v = 0; pf_zigzag(pr, v); }
        else if (4 == dfid && 5 == dt) { int64_t v = 0; pf_zigzag(pr, v); encoding = (int32_t)v; }
        else if (5 == dfid && 5 == dt) { pf_zigzag(pr, def_len); }
        else if (6 == dfid && 5 == dt) { pf_zigzag(pr, rep_len); }
        else if (OB_FAIL(pf_skip(pr, dt))) { break; }
      }
    } else if (OB_FAIL(pf_skip(pr, type))) {
      break;
    }
  }
  if (OB_FAIL(ret)) {
    return ret;
  }
  // 数据区
  const int64_t body_off = pr.p + rep_len + def_len;
  const int64_t body_len = comp_size - rep_len - def_len;
  if (body_off + body_len > size) {
    return OB_INVALID_ARGUMENT;
  }
  uint8_t *plain = NULL;
  int64_t plain_len = 0;
  if (0 == codec) {
    plain = data + body_off;
    plain_len = body_len;
  } else if (1 == codec) {                       // SNAPPY
    if (OB_ISNULL(plain = static_cast<uint8_t *>(alloc.alloc(uncomp_size + 1)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (OB_FAIL(snappy_decompress(data + body_off, body_len, plain, uncomp_size, plain_len))) {
    }
  } else {
    ret = OB_NOT_SUPPORTED;
  }
  if (OB_FAIL(ret)) {
    return ret;
  }
  // PLAIN 解码
  const int32_t width = (2 == col_type) ? 8 : 4;
  const int64_t cnt = (page_vals > 0 ? page_vals : num_values);
  for (int64_t i = 0; OB_SUCC(ret) && i < cnt; ++i) {
    const int64_t off = i * width;
    if (off + width > plain_len) {
      break;
    }
    int64_t v = 0;
    if (4 == width) {
      uint32_t u = (uint32_t)plain[off] | ((uint32_t)plain[off + 1] << 8)
                   | ((uint32_t)plain[off + 2] << 16) | ((uint32_t)plain[off + 3] << 24);
      v = (int64_t)(int32_t)u;
    } else {
      uint64_t u = 0;
      for (int b = 7; b >= 0; --b) { u = (u << 8) | plain[off + b]; }
      v = (int64_t)u;
    }
    if (OB_FAIL(values.push_back(v))) {
    }
  }
  return ret;
}

// ---------- glob：列出目录下所有条目 ----------
static int glob_paths(const char *pattern, ObIAllocator &alloc, ObIArray<ObString> &paths)
{
  int ret = OB_SUCCESS;
  ObString pat(pattern);
  ObString dir_part = pat;
  int64_t slash = -1;
  for (int64_t i = pat.length() - 1; i >= 0; --i) {
    if ('/' == pat.ptr()[i]) { slash = i; break; }
  }
  ObSqlString dir;
  if (slash > 0) {
    if (OB_FAIL(dir.append(pat.ptr(), slash))) {}
  } else {
    if (OB_FAIL(dir.append("."))) {}
  }
  if (OB_SUCC(ret)) {
    char dpath[512];
    int64_t n = dir.length() < 510 ? dir.length() : 510;
    MEMCPY(dpath, dir.ptr(), n);
    dpath[n] = '\0';
    DIR *dp = opendir(dpath);
    if (NULL == dp) {
      ret = OB_FILE_NOT_EXIST;
    } else {
      struct dirent *ent = NULL;
      while (OB_SUCC(ret) && NULL != (ent = readdir(dp))) {
        if (0 == strcmp(ent->d_name, ".") || 0 == strcmp(ent->d_name, "..")) { continue; }
        ObSqlString full;
        if (OB_FAIL(full.append(dir.ptr(), dir.length()))) {
        } else if (OB_FAIL(full.append("/"))) {
        } else if (OB_FAIL(full.append(ent->d_name))) {
        } else {
          char *cp = static_cast<char *>(alloc.alloc(full.length() + 1));
          if (OB_ISNULL(cp)) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else {
            MEMCPY(cp, full.ptr(), full.length());
            cp[full.length()] = '\0';
            if (OB_FAIL(paths.push_back(ObString(static_cast<int32_t>(full.length()), cp)))) {}
          }
        }
      }
      closedir(dp);
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

  ObString func_name;
  bool is_glob = false;
  bool is_json = false;
  bool is_parquet = false;
  if (OB_NOT_NULL(parse_tree.children_[0]) && OB_NOT_NULL(parse_tree.children_[0]->str_value_)) {
    func_name.assign_ptr(parse_tree.children_[0]->str_value_,
                         static_cast<int32_t>(parse_tree.children_[0]->str_len_));
    is_glob = (0 == func_name.case_compare("glob"));
    is_json = (0 == func_name.case_compare("read_json"));
    is_parquet = (0 == func_name.case_compare("read_parquet"));
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
          if (0 == pname.case_compare("format")) {
            // read_json 的 format 参数（newline_delimited / json）接受
          } else if (0 == pname.case_compare("header")) {
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
  } else if (is_glob) {
    ObSEArray<ObString, 8> gpaths;
    if (OB_FAIL(glob_paths(file_path.ptr(), *allocator_, gpaths))) {
      LOG_WARN("glob failed", K(file_path), K(ret));
    } else {
      ObString file_col("file");
      if (OB_FAIL(col_names.push_back(file_col))) {
      }
      for (int64_t gi = 0; OB_SUCC(ret) && gi < gpaths.count(); ++gi) {
        ObSEArray<ObString, 8> r;
        if (OB_FAIL(r.push_back(gpaths.at(gi)))) {
        } else if (OB_FAIL(rows.push_back(r))) {
        }
      }
    }
  } else if (is_parquet) {
    ObString pq_col;
    ObSEArray<int64_t, 16> pq_vals;
    if (OB_FAIL(read_parquet_single_column(file_path.ptr(), *allocator_, pq_col, pq_vals))) {
      LOG_WARN("read parquet failed", K(file_path), K(ret));
    } else {
      if (OB_FAIL(col_names.push_back(pq_col))) {
      }
      for (int64_t pi = 0; OB_SUCC(ret) && pi < pq_vals.count(); ++pi) {
        char num[32];
        int len = snprintf(num, sizeof(num), "%ld", (long)pq_vals.at(pi));
        char *cp = static_cast<char *>(allocator_->alloc(len + 1));
        if (OB_ISNULL(cp)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          MEMCPY(cp, num, len);
          cp[len] = '\0';
          ObSEArray<ObString, 8> r;
          if (OB_FAIL(r.push_back(ObString(static_cast<int32_t>(len), cp)))) {
          } else if (OB_FAIL(rows.push_back(r))) {
          }
        }
      }
      LOG_INFO("file sql parquet", K(ret), K(file_path), "col", pq_col, "rows", pq_vals.count());
    }
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
  } else if (is_json && OB_FAIL(parse_jsonl_text(file_data, file_size, *allocator_, col_names, rows))) {
  } else if (!is_json && OB_FAIL(parse_csv_text(file_data, file_size, *allocator_, col_names, rows, max_columns))) {
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
      if (is_glob || is_json) {
        t = ObVarcharType;
      } else if (has_value && all_bool) {
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
