#!/usr/bin/env bash
# docker_check.sh — 镜像构建 + 容器内协议自检（握手 / 公开用例 / 加固运行）。
# 用法: tools/docker_check.sh [--no-build]   环境变量: VECTORDB_IMAGE（默认 minivectordb:latest）
# 幂等：每次运行都新建临时目录，不触碰已有数据库；任一步失败即以非零码退出。
set -euo pipefail

IMAGE=${VECTORDB_IMAGE:-minivectordb:latest}
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD=1

for arg in "$@"; do
  case "$arg" in
    --no-build) BUILD=0 ;;
    -h|--help) sed -n '2,4p' "$0"; exit 0 ;;
    *) echo "unknown argument: $arg" >&2; exit 2 ;;
  esac
done

step() { echo; echo "== $*"; }
fail() { echo "FAIL: $*" >&2; exit 1; }

command -v docker >/dev/null || fail "docker 不在 PATH"
command -v python3 >/dev/null || fail "python3 不在 PATH"

WORK=$(mktemp -d "${TMPDIR:-/tmp}/docker-check.XXXXXX")
CONTAINER_NAME=""
cleanup() {
  if [ -n "$CONTAINER_NAME" ]; then docker rm -f "$CONTAINER_NAME" >/dev/null 2>&1 || true; fi
  echo "artifacts: $WORK"
}
trap cleanup EXIT

# ---------- 1. 构建 ----------
if [ "$BUILD" = 1 ]; then
  step "[1/4] docker build -t $IMAGE $ROOT"
  build_log=$WORK/build.log
  start=$(date +%s)
  if ! docker build -t "$IMAGE" "$ROOT" >"$build_log" 2>&1; then
    tail -40 "$build_log" >&2
    fail "docker build 失败（完整日志 $build_log）"
  fi
  build_secs=$(( $(date +%s) - start ))
  echo "构建成功：${build_secs}s"
else
  docker image inspect "$IMAGE" >/dev/null 2>&1 || fail "镜像 $IMAGE 不存在，去掉 --no-build 重新构建"
  step "[1/4] 构建已跳过（--no-build）"
fi
img_size=$(docker image inspect "$IMAGE" --format '{{.Size}}')
echo "镜像大小：${img_size} 字节（$(( img_size / 1024 / 1024 )) MiB）"

# ---------- 2. 握手 ----------
step "[2/4] 握手检查（--handshake-only）"
python3 "$ROOT/ref/protocol_check/run.py" --image "$IMAGE" --handshake-only --output "$WORK/handshake" \
  || fail "握手未通过（$WORK/handshake）"
python3 - "$WORK/handshake/result.json" <<'PY' || fail "握手结论非 PASS"
import json, sys
report = json.load(open(sys.argv[1]))
assert report['passed'] is True, report
assert [c['status'] for c in report['cases']] == ['PASS'], report
print('握手：ready + shutdown PASS')
PY

# ---------- 3. 公开用例全量 ----------
step "[3/4] 公开用例全量（镜像模式）"
python3 "$ROOT/ref/protocol_check/run.py" --image "$IMAGE" --output "$WORK/public" \
  || fail "公开用例未全 PASS（$WORK/public）"
python3 - "$WORK/public/result.json" "$ROOT/ref/mysqltest/manifest.json" <<'PY' || fail "公开用例数量/结论不符"
import json, sys
report = json.load(open(sys.argv[1]))
manifest = json.load(open(sys.argv[2]))
expected = [c['id'] for c in manifest['cases']]
got = [c['id'] for c in report['cases']]
assert report['passed'] is True, report
assert sorted(got) == sorted(expected), (got, expected)
passed = [c for c in report['cases'] if c['status'] == 'PASS']
assert len(passed) == len(expected), report['cases']
print('公开用例：%d/%d PASS' % (len(passed), len(expected)))
PY

# ---------- 4. 加固运行（只读根 + 无网络 + 2G + 128 进程 + bind /data） ----------
step "[4/4] 加固运行：握手 + INSERT/SELECT"
test_root=$WORK/hardened
mkdir -p "$test_root/data"
chmod 777 "$test_root/data"
CONTAINER_NAME="vectordb-protocol-check-$$"
cat > "$test_root/requests.jsonl" <<'JSONL'
{"id":1,"op":"execute","sql":"CREATE TABLE docs (id BIGINT PRIMARY KEY, content VARCHAR(16));","search_mode":"exact"}
{"id":2,"op":"execute","sql":"INSERT INTO docs VALUES (1, '你好');","search_mode":"exact"}
{"id":3,"op":"execute","sql":"SELECT id, content FROM docs ORDER BY id;","search_mode":"exact"}
{"id":4,"op":"shutdown"}
JSONL
timeout 180s docker run --rm -i --name "$CONTAINER_NAME" \
  --network=none --cpus=2 --memory=2g --memory-swap=2g --pids-limit=128 \
  --read-only --cap-drop=ALL --security-opt=no-new-privileges \
  --mount "type=bind,src=$test_root/data,dst=/data" \
  "$IMAGE" \
  < "$test_root/requests.jsonl" \
  > "$test_root/stdout.jsonl" 2> "$test_root/stderr.log" \
  || fail "加固运行退出码非零（见 $test_root/stderr.log）"
python3 - "$test_root/stdout.jsonl" <<'PY' || fail "加固运行协议断言失败"
import json, sys
from pathlib import Path

raw = Path(sys.argv[1]).read_bytes()
assert raw.endswith(b'\n'), '最后一条响应必须以换行结束'
lines = raw.decode('utf-8').splitlines()
assert len(lines) == 5, '预期 5 行输出，实际 %d 行：%r' % (len(lines), lines[:6])
msgs = [json.loads(line) for line in lines]
assert msgs[0] == {'type': 'ready'}, msgs[0]
for ident, resp in enumerate(msgs[1:], 1):
    assert type(resp.get('id')) is int and resp['id'] == ident, resp
    assert resp.get('ok') is True, resp
assert msgs[2].get('affected_rows') == 1, msgs[2]
assert msgs[3].get('columns') == ['id', 'content'], msgs[3]
assert msgs[3].get('rows') == [[1, '你好']], msgs[3]
assert msgs[4] == {'id': 4, 'ok': True}, msgs[4]
print('加固运行：ready + CREATE/INSERT/SELECT + shutdown PASS')
PY

echo
echo "ALL PASS（镜像 $IMAGE，产物 $WORK）"
