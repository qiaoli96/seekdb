#!/usr/bin/env bash
# 用官方驱动跑公开用例（本地二进制模式，无需 Docker）
set -uo pipefail
BIN=${BIN:-/root/build/minivectordb}
OUT=${OUT:-/root/pr-public}
PKG=${PKG:-/mnt/e/studyitem/minivectordb/ref}
cd "$PKG" || exit 1
rm -rf "$OUT"
exec python3 protocol_check/run.py \
  --command "[\"$BIN\",\"--data-dir\",\"{data_dir}\"]" \
  --output "$OUT" "$@"
