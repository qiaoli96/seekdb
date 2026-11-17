#!/usr/bin/env bash
# 复现子代理 C 报的三条语义缺口
set -uo pipefail
BIN=${BIN:-/root/build/minivectordb}
D=$(mktemp -d)
cat > "$D/req.jsonl" <<'JSONL'
{"id":1,"op":"execute","sql":"CREATE TABLE t (id BIGINT PRIMARY KEY, c VARCHAR(0));","search_mode":"exact"}
{"id":2,"op":"execute","sql":"INSERT INTO t VALUES (1,'abcdefghij');","search_mode":"exact"}
{"id":3,"op":"execute","sql":"SELECT id, c FROM t;","search_mode":"exact"}
{"id":4,"op":"execute","sql":"CREATE TABLE v (id BIGINT PRIMARY KEY, e VECTOR(2));","search_mode":"exact"}
{"id":5,"op":"execute","sql":"INSERT INTO v VALUES (1,'[1,0]');","search_mode":"exact"}
{"id":6,"op":"execute","sql":"SELECT id, L2_DISTANCE(e,'[nan,0]') AS d FROM v;","search_mode":"exact"}
{"id":7,"op":"execute","sql":"SELECT id, L2_DISTANCE(e,'[1e400,0]') AS d FROM v;","search_mode":"exact"}
{"id":8,"op":"execute","sql":"UPDATE v SET id = 5 WHERE id = 999;","search_mode":"exact"}
{"id":9,"op":"execute","sql":"ALTER TABLE t ADD COLUMN x BIGINT;","search_mode":"exact"}
{"id":10,"op":"shutdown"}
JSONL
"$BIN" --data-dir "$D/data" < "$D/req.jsonl"
