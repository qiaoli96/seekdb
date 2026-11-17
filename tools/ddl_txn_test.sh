#!/usr/bin/env bash
# 补充验证：事务内 DDL 的持久化/回滚，与数据目录不可打开时的启动行为。
set -uo pipefail
BIN=${BIN:-/root/build/minivectordb}
D=$(mktemp -d)

cat > "$D/req.jsonl" <<'JSONL'
{"id":1,"op":"execute","sql":"BEGIN;","search_mode":"exact"}
{"id":2,"op":"execute","sql":"CREATE TABLE keep (id BIGINT PRIMARY KEY, c VARCHAR(8));","search_mode":"exact"}
{"id":3,"op":"execute","sql":"INSERT INTO keep VALUES (1,'kept');","search_mode":"exact"}
{"id":4,"op":"execute","sql":"COMMIT;","search_mode":"exact"}
{"id":5,"op":"execute","sql":"BEGIN;","search_mode":"exact"}
{"id":6,"op":"execute","sql":"CREATE TABLE gone (id BIGINT PRIMARY KEY, c VARCHAR(8));","search_mode":"exact"}
{"id":7,"op":"execute","sql":"ROLLBACK;","search_mode":"exact"}
{"id":8,"op":"execute","sql":"SELECT id FROM gone;","search_mode":"exact"}
{"id":9,"op":"shutdown"}
JSONL
echo "--- 第一次会话（事务内建表：keep 提交，gone 回滚）---"
"$BIN" --data-dir "$D/data" < "$D/req.jsonl"
echo "--- 重启：keep 应在，gone 应不在 ---"
printf '%s\n' '{"id":1,"op":"execute","sql":"SELECT * FROM keep;","search_mode":"exact"}' \
              '{"id":2,"op":"execute","sql":"SELECT * FROM gone;","search_mode":"exact"}' \
              '{"id":3,"op":"shutdown"}' | "$BIN" --data-dir "$D/data"
echo "--- 不可写数据目录：不输出 ready，退出码非 0 ---"
printf '%s\n' '{"id":1,"op":"shutdown"}' | "$BIN" --data-dir /proc/nowhere/data
echo "exit=$?"
rm -rf "$D"
