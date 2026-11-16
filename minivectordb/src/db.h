#pragma once

#include <memory>
#include <string>
#include <vector>

#include "common.h"

namespace mvd {

struct ExecResult {
  enum Kind { NONE, MODIFIED, QUERY } kind = NONE;
  int64_t affected_rows = 0;
  std::vector<std::string> columns;
  std::string rows_json;
};

class Database {
 public:
  explicit Database(std::string data_dir);
  ~Database();

  void open();
  ExecResult execute(const std::string &sql);
  void shutdown();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mvd
