#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>

#include "common.h"
#include "db.h"
#include "json.h"

namespace {

std::string data_dir_from_args(int argc, char **argv) {
  std::string arg;
  for (int i = 1; i < argc; ++i) {
    std::string value = argv[i];
    const std::string prefix = "--data-dir=";
    if (value.rfind(prefix, 0) == 0) {
      arg = value.substr(prefix.size());
      break;
    }
    if (value == "--data-dir" && i + 1 < argc) {
      arg = argv[i + 1];
      break;
    }
    if (value[0] != '-') {
      arg = value;
      break;
    }
  }
  const char *env = std::getenv("VECTORDB_DATA_DIR");
  const std::string candidates[3] = {arg, (env != nullptr ? env : ""), "/data"};
  for (const std::string &c : candidates) {
    if (c.empty()) {
      continue;
    }
    if (mkdir(c.c_str(), 0755) == 0 || errno == EEXIST) {
      return c;
    }
    fprintf(stderr, "mvd: data dir %s unusable (%s), trying fallback\n", c.c_str(),
            std::strerror(errno));
  }
  return "/data";
}

void emit(const std::string &line) {
  fwrite(line.data(), 1, line.size(), stdout);
  fputc('\n', stdout);
  fflush(stdout);
}

void log_line(const std::string &text) {
  fprintf(stderr, "mvd: %s\n", text.c_str());
  fflush(stderr);
}

std::string response_prefix(const mvd::JsonValue &id) {
  std::string out = "{\"id\":";
  mvd::json_append_value(out, id);
  return out;
}

}  // namespace

int main(int argc, char **argv) {
  signal(SIGPIPE, SIG_IGN);
  std::ios::sync_with_stdio(false);
  std::cin.tie(nullptr);

  std::string dir = data_dir_from_args(argc, argv);
  mvd::Database db(dir);
  try {
    db.open();
  } catch (const mvd::Error &e) {
    log_line("failed to open database: " + e.code + " " + e.message);
    return 1;
  } catch (const std::exception &e) {
    log_line(std::string("failed to open database: ") + e.what());
    return 1;
  }

  emit("{\"type\":\"ready\"}");
  log_line("started data_dir=" + dir);

  std::string line;
  while (std::getline(std::cin, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      continue;
    }
    mvd::JsonValue request;
    try {
      request = mvd::parse_json(line);
    } catch (const mvd::Error &e) {
      log_line("invalid request: " + e.message);
      continue;
    }
    const mvd::JsonValue *id = request.get("id");
    if (id == nullptr) {
      log_line("request without id");
      continue;
    }
    std::string prefix = response_prefix(*id);
    const mvd::JsonValue *op = request.get("op");
    std::string op_name = op != nullptr && op->type == mvd::JsonValue::STR ? op->s : std::string();

    if (op_name == "shutdown") {
      db.shutdown();
      emit(prefix + ",\"ok\":true}");
      return 0;
    }
    if (op_name != "execute") {
      emit(prefix + ",\"ok\":false,\"error\":{\"code\":\"unsupported_op\",\"message\":\"unsupported operation\"}}");
      continue;
    }
    const mvd::JsonValue *sql = request.get("sql");
    if (sql == nullptr || sql->type != mvd::JsonValue::STR) {
      emit(prefix + ",\"ok\":false,\"error\":{\"code\":\"invalid_request\",\"message\":\"missing sql\"}}");
      continue;
    }
    try {
      mvd::ExecResult res = db.execute(sql->s);
      std::string out = prefix + ",\"ok\":true";
      if (res.kind == mvd::ExecResult::MODIFIED) {
        out += ",\"affected_rows\":" + std::to_string(res.affected_rows);
      } else if (res.kind == mvd::ExecResult::QUERY) {
        out += ",\"columns\":[";
        for (size_t i = 0; i < res.columns.size(); ++i) {
          if (i) {
            out.push_back(',');
          }
          mvd::json_append_string(out, res.columns[i]);
        }
        out += "],\"rows\":";
        out += res.rows_json;
      }
      out.push_back('}');
      emit(out);
    } catch (const mvd::Error &e) {
      std::string out = prefix + ",\"ok\":false,\"error\":{\"code\":";
      mvd::json_append_string(out, e.code);
      out += ",\"message\":";
      mvd::json_append_string(out, e.message.empty() ? e.code : e.message);
      out += "}}";
      emit(out);
    } catch (const std::exception &e) {
      std::string out = prefix + ",\"ok\":false,\"error\":{\"code\":\"internal_error\",\"message\":";
      mvd::json_append_string(out, e.what());
      out += "}}";
      emit(out);
    }
  }

  try {
    db.shutdown();
  } catch (const mvd::Error &e) {
    log_line("shutdown failed: " + e.code);
    return 1;
  }
  return 0;
}
