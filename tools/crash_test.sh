#!/usr/bin/env bash
# 崩溃恢复实测：已 COMMIT 的必须全在，未 COMMIT 的必须全不在。
set -uo pipefail
BIN=${BIN:-/root/build/minivectordb}
TMP=$(mktemp -d)

wait_lines() {  # $1=期望行数 $2=文件
  local want=$1 file=$2 i=0
  while [ "$(wc -l < "$file")" -lt "$want" ]; do
    i=$((i+1))
    if [ $i -gt 200 ]; then echo "TIMEOUT waiting $want lines in $file"; return 1; fi
    sleep 0.05
  done
}

echo "================= 场景A：事务 COMMIT 之后 kill -9 ================="
mkdir -p "$TMP/a/data"
cat > "$TMP/a/req.jsonl" <<'JSONL'
{"id":1,"op":"execute","sql":"CREATE TABLE docs (id BIGINT PRIMARY KEY, content VARCHAR(16));","search_mode":"exact"}
{"id":2,"op":"execute","sql":"INSERT INTO docs VALUES (1,'旧值');","search_mode":"exact"}
{"id":3,"op":"execute","sql":"BEGIN;","search_mode":"exact"}
{"id":4,"op":"execute","sql":"INSERT INTO docs VALUES (2,'事务内新增');","search_mode":"exact"}
{"id":5,"op":"execute","sql":"UPDATE docs SET content = '事务内改过' WHERE id = 1;","search_mode":"exact"}
{"id":6,"op":"execute","sql":"COMMIT;","search_mode":"exact"}
JSONL
mkfifo "$TMP/a/in"
"$BIN" --data-dir "$TMP/a/data" < "$TMP/a/in" > "$TMP/a/out" 2> "$TMP/a/err" &
A_PID=$!
exec 3> "$TMP/a/in"
wait_lines 1 "$TMP/a/out"
cat "$TMP/a/req.jsonl" >&3
wait_lines 7 "$TMP/a/out"
echo "--- kill -9 之前的进程响应（ready + 6 条）---"
cat "$TMP/a/out"
kill -9 "$A_PID"
wait "$A_PID" 2>/dev/null
exec 3>&-
echo "--- kill -9 之后 wal.log 字节数 ---"
wc -c < "$TMP/a/data/wal.log"
echo "--- 重启后查询（期望 id=1 事务内改过, id=2 事务内新增）---"
printf '%s\n' '{"id":1,"op":"execute","sql":"SELECT id, content FROM docs ORDER BY id;","search_mode":"exact"}' | "$BIN" --data-dir "$TMP/a/data"

echo
echo "================= 场景B：事务未 COMMIT 就 kill -9 ================="
mkdir -p "$TMP/b/data"
cat > "$TMP/b/req.jsonl" <<'JSONL'
{"id":1,"op":"execute","sql":"CREATE TABLE docs (id BIGINT PRIMARY KEY, content VARCHAR(16));","search_mode":"exact"}
{"id":2,"op":"execute","sql":"INSERT INTO docs VALUES (1,'旧值');","search_mode":"exact"}
{"id":3,"op":"execute","sql":"BEGIN;","search_mode":"exact"}
{"id":4,"op":"execute","sql":"INSERT INTO docs VALUES (2,'未提交新增');","search_mode":"exact"}
{"id":5,"op":"execute","sql":"UPDATE docs SET content = '未提交改过' WHERE id = 1;","search_mode":"exact"}
JSONL
mkfifo "$TMP/b/in"
"$BIN" --data-dir "$TMP/b/data" < "$TMP/b/in" > "$TMP/b/out" 2> "$TMP/b/err" &
B_PID=$!
exec 4> "$TMP/b/in"
wait_lines 1 "$TMP/b/out"
cat "$TMP/b/req.jsonl" >&4
wait_lines 6 "$TMP/b/out"
echo "--- kill -9 之前的进程响应（ready + 5 条，事务未提交）---"
cat "$TMP/b/out"
kill -9 "$B_PID"
wait "$B_PID" 2>/dev/null
exec 4>&-
echo "--- kill -9 之后 wal.log 字节数 ---"
wc -c < "$TMP/b/data/wal.log"
echo "--- 重启后查询（期望只有 id=1 旧值；未提交的新增与更新都不在）---"
printf '%s\n' '{"id":1,"op":"execute","sql":"SELECT id, content FROM docs ORDER BY id;","search_mode":"exact"}' | "$BIN" --data-dir "$TMP/b/data"
rm -rf "$TMP"
