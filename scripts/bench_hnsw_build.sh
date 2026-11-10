#!/usr/bin/env bash
#
# Measure how long seekdb needs to build an HNSW vector index, so that the
# host build and the hipVS GPU build can be compared.
#
# The script drives an already running seekdb instance through the MySQL
# protocol. Build one observer binary with -DSEEKDB_WITH_HIPVS and one
# without, start them in turn, and run this script against both.
#
# Usage:
#   ROWS=200000 DIM=256 scripts/bench_hnsw_build.sh "host build"
#   ROWS=200000 DIM=256 scripts/bench_hnsw_build.sh "gpu build (hipVS)"
#
# Environment:
#   ROWS      number of vectors            (default 200000)
#   DIM       vector dimension             (default 256)
#   M         HNSW graph degree            (default 16)
#   EFC       ef_construction              (default 200)
#   DIST      distance metric              (default l2)
#   HOST      mysql host                   (default 127.0.0.1)
#   PORT      mysql port                   (default 2881)
#   USER      user                         (default root)
#   PASS      password                     (default empty)
#   DB        database                     (default test)
#   DATA      prepared data file           (default /tmp/vectors.sql)
#
set -euo pipefail

LABEL="${1:-run}"
ROWS="${ROWS:-200000}"
DIM="${DIM:-256}"
M="${M:-16}"
EFC="${EFC:-200}"
DIST="${DIST:-l2}"
HOST="${HOST:-127.0.0.1}"
PORT="${PORT:-2881}"
USER="${USER:-root}"
PASS="${PASS:-}"
DB="${DB:-test}"
DATA="${DATA:-/tmp/vectors.sql}"

TABLE="bench_vec"
INDEX="idx_bench_vec"

MYSQL=(mysql -h "$HOST" -P "$PORT" -u "$USER" ${PASS:+-p"$PASS"} --default-character-set=utf8mb4)

run_sql() { "${MYSQL[@]}" -D "$DB" -N -B -e "$1"; }

echo "=============================================================="
echo " $LABEL"
echo " rows=$ROWS dim=$DIM m=$M ef_construction=$EFC distance=$DIST"
echo "=============================================================="

if [ ! -f "$DATA" ]; then
  echo "generating the data file $DATA ..."
  python3 "$(dirname "${BASH_SOURCE[0]}")/gen_vec_data.py" --rows "$ROWS" --dim "$DIM" --table "$TABLE" > "$DATA"
fi

run_sql "DROP TABLE IF EXISTS $TABLE;"
run_sql "CREATE TABLE $TABLE (id BIGINT PRIMARY KEY, v VECTOR($DIM));"

echo "loading the vectors ..."
LOAD_START=$(date +%s.%N)
"${MYSQL[@]}" -D "$DB" < "$DATA"
LOAD_END=$(date +%s.%N)
LOAD_SEC=$(echo "$LOAD_END - $LOAD_START" | bc)
echo "load: ${LOAD_SEC}s"

run_sql "DROP INDEX IF EXISTS $INDEX;"

echo "building the HNSW index ..."
BUILD_START=$(date +%s.%N)
run_sql "CREATE VECTOR INDEX $INDEX ON $TABLE(v) WITH (distance=$DIST, type=hnsw, m=$M, ef_construction=$EFC);"
BUILD_END=$(date +%s.%N)
BUILD_SEC=$(echo "$BUILD_END - $BUILD_START" | bc)

echo ""
echo "--------------------------------------------------------------"
printf "label                : %s\n" "$LABEL"
printf "rows / dim           : %s / %s\n" "$ROWS" "$DIM"
printf "data load (s)        : %s\n" "$LOAD_SEC"
printf "HNSW build time (s)  : %s\n" "$BUILD_SEC"
printf "vectors per second   : %s\n" "$(echo "scale=0; $ROWS / $BUILD_SEC" | bc)"
echo "--------------------------------------------------------------"

echo ""
echo "sanity check (the index must answer queries):"
run_sql "SELECT COUNT(*) FROM $TABLE WHERE ORDER BY distance(v, (SELECT v FROM $TABLE LIMIT 1)) LIMIT 1;" 2>/dev/null || true
run_sql "EXPLAIN SELECT id FROM $TABLE ORDER BY distance(v, (SELECT v FROM $TABLE LIMIT 1)) LIMIT 10;" 2>/dev/null || true

echo ""
echo "save the numbers above; compare the build time of the two builds."
