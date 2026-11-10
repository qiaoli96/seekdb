#!/usr/bin/env bash
#
# Build the AMD hipVS vector search library and install it into a prefix that
# the seekdb build can pick up.
#
# hipVS is the ROCm port of the RAPIDS cuVS library:
#   https://github.com/AMD-Ecosystem/hipVS
#
# The script needs a ROCm toolchain (hipcc). The toolchain alone is enough to
# compile, an AMD GPU is only needed to run the GPU build.
#
# Usage:
#   HIPVS_REF=release/rocmds-26.03 scripts/build_hipvs.sh [install-prefix]
#
set -euo pipefail

HIPVS_REF="${HIPVS_REF:-release/rocmds-26.03}"
PREFIX="${1:-${PWD}/../hipvs-install}"
WORKDIR="$(mktemp -d)"
SRC_DIR="${WORKDIR}/hipVS"

if ! command -v hipcc >/dev/null 2>&1; then
  echo "hipcc was not found, install ROCm first:" >&2
  echo "  https://rocm.docs.amd.com/projects/install-on-linux/en/latest/" >&2
  exit 1
fi

echo "==> fetching hipVS ${HIPVS_REF}"
mkdir -p "${SRC_DIR}"
curl -fsSL "https://github.com/AMD-Ecosystem/hipVS/archive/refs/heads/${HIPVS_REF}.tar.gz" \
  | tar -xz --strip-components=1 -C "${SRC_DIR}"

echo "==> configuring hipVS in ${WORKDIR}/build"
cmake -S "${SRC_DIR}" -B "${WORKDIR}/build" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
  -DCMAKE_C_COMPILER=hipcc \
  -DCMAKE_CXX_COMPILER=hipcc \
  -DBUILD_TESTING=OFF \
  -DCUVS_BUILD_TEST=OFF

echo "==> building hipVS"
cmake --build "${WORKDIR}/build" -j "$(nproc)"

echo "==> installing hipVS into ${PREFIX}"
cmake --install "${WORKDIR}/build"

cat <<EOF

hipVS was installed into ${PREFIX}.

Build seekdb with the GPU HNSW build path enabled:

  bazel build -c opt //src/observer:observer \\
    --copt=-DSEEKDB_WITH_HIPVS \\
    --copt=-I${PREFIX}/include \\
    --copt=-I${ROCM_PATH:-/opt/rocm}/include \\
    --linkopt=-L${PREFIX}/lib -Wl,-rpath,${PREFIX}/lib \\
    --linkopt=-L${ROCM_PATH:-/opt/rocm}/lib -Wl,-rpath,${ROCM_PATH:-/opt/rocm}/lib \\
    --linkopt=-lhipvs

Without SEEKDB_WITH_HIPVS the build never touches ROCm and the HNSW index is
built on the host exactly as before.
EOF
