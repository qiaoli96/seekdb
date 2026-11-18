#!/usr/bin/env bash
#
# Compile the hipVS bridge.
#
# The evaluation system runs exactly:
#     HIPVS_ROOT=/opt/hipvs ROCM_ROOT=/opt/rocm \
#       tools/hipvs_bridge/build.sh <out>/libseekdb_cuvs_bridge.so
# so this script is ours to change (the rules allow it), but the output has
# to stay at the path the caller asks for.
#
# Route B compiles the bridge as C++ (it contains HIP kernels and uses
# hipBLAS), so the source is seekdb_cuvs_bridge.cpp instead of the .c file.
# hipcc compiles a .cpp as HIP C++, a .c as plain C, which is exactly the
# trap that broke an earlier submission (the file was a .c but used C++
# only syntax).
#
# If the ROCm installation has no hipBLAS, the bridge is still built, but
# without the GEMM engine (it falls back to the cuVS brute force engine),
# so a missing library can never turn into a dead submission.

set -Eeuo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
HIPVS_ROOT=${HIPVS_ROOT:-/opt/hipvs}
ROCM_ROOT=${ROCM_ROOT:-/opt/rocm}
HIPCC=${HIPCC:-$ROCM_ROOT/bin/hipcc}
OUTPUT=${1:-$SCRIPT_DIR/libseekdb_cuvs_bridge.so}

[[ -x "$HIPCC" ]] || { echo "hipcc is not executable: $HIPCC" >&2; exit 2; }
[[ -f "$HIPVS_ROOT/include/cuvs/core/c_api.h" ]] || {
  echo "hipVS headers are missing under $HIPVS_ROOT/include" >&2
  exit 2
}
[[ -f "$HIPVS_ROOT/lib/libcuvs_c.so" ]] || {
  echo "libcuvs_c.so is missing under $HIPVS_ROOT/lib" >&2
  exit 2
}

mkdir -p "$(dirname "$OUTPUT")"

SOURCE="$SCRIPT_DIR/seekdb_cuvs_bridge.cpp"
[[ -f "$SOURCE" ]] || SOURCE="$SCRIPT_DIR/seekdb_cuvs_bridge.c"

EXTRA_LIBS=()
EXTRA_DEFS=()
if [[ -f "$ROCM_ROOT/lib/libhipblas.so" || -f "$ROCM_ROOT/lib/libhipblas.so.1" ]]; then
  EXTRA_LIBS=(-lhipblas)
else
  echo "warning: no libhipblas under $ROCM_ROOT/lib, building without the GEMM engine" >&2
  EXTRA_DEFS=(-DSK_NO_HIPBLAS=1)
fi

# The compile time defaults are what the evaluation sees, because the
# evaluation machine does not run our scripts and therefore has none of our
# environment variables.  The knobs below select the variant that is built:
#
#   SK_ENGINE=0   cuVS NN-Descent (the frozen baseline behaviour)
#   SK_ENGINE=1   cuVS brute force kNN
#   SK_ENGINE=2   our GEMM + top-k engine (route B, the default)
#   SK_OWN=0      never build the graph ourselves
#   SK_OWN=1      build the graph ourselves (the default)
#   SK_COMPACT=1  the compact snapshot (the index stream drops from 612 MB to
#                 368 MB for 250K x 256)
#   SK_VERIFY=0   do not run the self check of the graph
EXTRA_DEFS+=(-DSK_DEFAULT_OWN_ENGINE="${SK_ENGINE:-2}")
EXTRA_DEFS+=(-DSK_DEFAULT_OWN_GRAPH="${SK_OWN:-1}")
EXTRA_DEFS+=(-DSK_DEFAULT_COMPACT_SNAPSHOT="${SK_COMPACT:-0}")
EXTRA_DEFS+=(-DSK_DEFAULT_VERIFY="${SK_VERIFY:-1}")

"$HIPCC" \
  -shared \
  -fPIC \
  -O2 \
  -D__HIP_PLATFORM_AMD__ \
  "${EXTRA_DEFS[@]}" \
  -I"$HIPVS_ROOT/include" \
  -I"$ROCM_ROOT/include" \
  "$SOURCE" \
  -L"$HIPVS_ROOT/lib" \
  -L"$ROCM_ROOT/lib" \
  -Wl,-soname,libseekdb_cuvs_bridge.so \
  -Wl,-rpath,"$HIPVS_ROOT/lib:$ROCM_ROOT/lib" \
  -lcuvs_c \
  "${EXTRA_LIBS[@]}" \
  -o "$OUTPUT"

echo "built $OUTPUT"
