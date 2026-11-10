#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <string>

#include "engine.h"
#include "json.h"

namespace {

// 内部细码 → 对外语义码。只有这一处做映射，后续按题包 error_codes 调整即可。
const std::string& external_error_code(const std::string& internal) {
  static const std::string kUnsupported = "unsupported_sql";
  static const std::map<std::string, std::string> kAliases = {
      {"unsupported_statement", "unsupported_sql"},
      {"unsupported_create", "unsupported_sql"},
      {"no_such_table", "no_such_table"},
      {"duplicate_key", "duplicate_key"},
      {"dimension_mismatch", "dimension_mismatch"},
  };
  const auto it = kAliases.find(internal);
  if (it != kAliases.end()) return it->second;
  if (internal.rfind("unsupported_", 0) == 0) return kUnsupported;
  return internal;
}

void append_int(std::string& out, int64_t v) {
  char buf[32];
  int n = std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
  out.append(buf, static_cast<size_t>(n));
}

void append_value(std::string& out, const Value& v) {
  switch (v.k) {
    case Value::K::Null: out += "null"; break;
    case Value::K::Int: append_int(out, v.i); break;
    case Value::K::Float: json_write_double(out, v.d); break;
    case Value::K::Str: json_escape_into(out, v.s); break;
    case Value::K::Vec: {
      out.push_back('[');
      for (size_t k = 0; k < v.v.size(); ++k) {
        if (k) out.push_back(',');
        json_write_double(out, static_cast<double>(v.v[k]));
      }
      out.push_back(']');
      break;
    }
  }
}

void append_id(std::string& out, const JsonValue& id) {
  if (id.t == JsonValue::T::Int) { append_int(out, id.i); return; }
  if (id.t == JsonValue::T::Str) { json_escape_into(out, id.s); return; }
  if (id.t == JsonValue::T::Double) { json_write_double(out, id.d); return; }
  out += "null";
}

void build_response(std::string& out, const JsonValue& id, const Result& r) {
  out.clear();
  out.reserve(256 + r.rows.size() * 32);
  out += "{\"id\":";
  append_id(out, id);
  if (!r.ok) {
    out += ",\"ok\":false,\"error\":{\"code\":";
    static const std::string kFallback = "error";
    json_escape_into(out, r.error_code.empty() ? kFallback : external_error_code(r.error_code));
    out += ",\"message\":";
    json_escape_into(out, r.error_message);
    out += "}}";
    return;
  }
  out += ",\"ok\":true";
  if (r.is_query) {
    out += ",\"columns\":[";
    for (size_t k = 0; k < r.columns.size(); ++k) {
      if (k) out.push_back(',');
      json_escape_into(out, r.columns[k]);
    }
    out += "],\"rows\":[";
    for (size_t i = 0; i < r.rows.size(); ++i) {
      if (i) out.push_back(',');
      out.push_back('[');
      for (size_t j = 0; j < r.rows[i].size(); ++j) {
        if (j) out.push_back(',');
        append_value(out, r.rows[i][j]);
      }
      out.push_back(']');
    }
    out += "]";
  } else if (r.affected >= 0) {
    out += ",\"affected_rows\":";
    append_int(out, r.affected);
  }
  out.push_back('}');
}

void emit(const std::string& line) {
  std::fwrite(line.data(), 1, line.size(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
  std::string data_dir = "/data";
  for (int k = 1; k < argc; ++k) {
    if (std::strcmp(argv[k], "--data-dir") == 0 && k + 1 < argc) data_dir = argv[++k];
  }

  Engine engine;
  std::string open_err;
  if (!engine.open(data_dir, open_err)) {
    // 恢复失败就不能声称 ready：写 stderr、非 0 退出，让调用方看到启动失败。
    std::fprintf(stderr, "cannot open data dir '%s': %s\n", data_dir.c_str(), open_err.c_str());
    return 1;
  }
  emit("{\"type\":\"ready\"}");

  std::string line;
  std::string out;
  while (std::getline(std::cin, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;

    JsonValue req;
    std::string err;
    if (!json_parse(line, req, err)) {
      emit("{\"id\":null,\"ok\":false,\"error\":{\"code\":\"invalid_json\",\"message\":\"bad request\"}}");
      continue;
    }
    const JsonValue* id = req.find("id");
    JsonValue id_default;
    if (!id) id = &id_default;

    const JsonValue* op = req.find("op");
    const std::string opname = (op && op->t == JsonValue::T::Str) ? op->s : std::string();

    if (opname == "shutdown") {
      engine.rollback_on_shutdown();   // 未提交事务在这里丢弃，已提交的早已落盘
      engine.close();
      build_response(out, *id, Result::Ok());
      emit(out);
      return 0;
    }
    if (opname != "execute") {
      build_response(out, *id, Result::Fail("unsupported_op", "unsupported op"));
      emit(out);
      continue;
    }

    const JsonValue* sql = req.find("sql");
    const JsonValue* mode = req.find("search_mode");
    const std::string modestr = (mode && mode->t == JsonValue::T::Str) ? mode->s : std::string("exact");
    if (!sql || sql->t != JsonValue::T::Str) {
      build_response(out, *id, Result::Fail("invalid_request", "missing sql"));
      emit(out);
      continue;
    }
    Result r = engine.execute(sql->s, modestr);
    build_response(out, *id, r);
    emit(out);
  }
  engine.rollback_on_shutdown();
  engine.close();
  return 0;
}
