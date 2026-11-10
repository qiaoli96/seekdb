#!/usr/bin/env bash
#
# Build the AMD hipVS vector search library and install it into a prefix that
# the seekdb build can pick up.
#
# hipVS is the ROCm port of the RAPIDS cuVS library:
#   https://github.com/AMD-Ecosystem/hipVS
#
# The script needs a ROCm toolchain (hipcc). The toolchain alone is enough to
# compile; an AMD GPU is only needed to run the GPU build.
#
# Usage:
#   HIPVS_REF=release/rocmds-26.03 \
#   HIPVS_ARCH=gfx1100 \
#   scripts/build_hipvs.sh [install-prefix]
#
# Environment:
#   HIPVS_REF   branch or tag of AMD-Ecosystem/hipVS   (default release/rocmds-26.03)
#   HIPVS_ARCH  HIP architecture of the target GPU     (default gfx1100, RX 7900)
#   ROCM_PATH   ROCm installation prefix              (default /opt/rocm)
#   JOBS        parallel build jobs                   (default nproc)
#
set -euo pipefail

HIPVS_REF="${HIPVS_REF:-release/rocmds-26.03}"
HIPVS_ARCH="${HIPVS_ARCH:-gfx1100}"
ROCM_PATH="${ROCM_PATH:-/opt/rocm}"
PREFIX="${1:-${PWD}/../hipvs-install}"
JOBS="${JOBS:-$(nproc)}"
WORKDIR="$(mktemp -d)"
SRC_DIR="${WORKDIR}/hipVS"

echo "==> hipVS ref   : ${HIPVS_REF}"
echo "==> HIP arch    : ${HIPVS_ARCH}"
echo "==> install to  : ${PREFIX}"

if ! command -v hipcc >/dev/null 2>&1; then
  if [ -x "${ROCM_PATH}/bin/hipcc" ]; then
    export PATH="${PATH}:${ROCM_PATH}/bin"
  else
    echo "hipcc was not found. Install ROCm first:" >&2
    echo "  https://rocm.docs.amd.com/projects/install-on-linux/en/latest/" >&2
    exit 1
  fi
fi
echo "==> hipcc       : $(command -v hipcc)"
hipcc --version | head -2

echo "==> fetching hipVS"
mkdir -p "${SRC_DIR}"
curl -fsSL "https://github.com/AMD-Ecosystem/hipVS/archive/refs/heads/${HIPVS_REF}.tar.gz" \
  | tar -xz --strip-components=1 -C "${SRC_DIR}"

echo "==> configuring"
cmake -S "${SRC_DIR}/cpp" -B "${WORKDIR}/build" \
  -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
  -DCMAKE_HIP_ARCHITECTURES="${HIPVS_ARCH}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=hipcc \
  -DBUILD_TESTS=OFF \
  -DBUILD_C_TESTS=OFF \
  -DBUILD_CUVS_BENCH=OFF \
  -DBUILD_MG_ALGOS=OFF \
  -DBUILD_SHARED_LIBS=ON \
  -DDISABLE_DEPRECATION_WARNINGS=ON

echo "==> building (${JOBS} jobs, this takes a while)"
cmake --build "${WORKDIR}/build" -j "${JOBS}"

echo "==> installing"
cmake --install "${WORKDIR}/build"

cat <<EOF

hipVS was installed into ${PREFIX}.

Now build seekdb with the GPU HNSW build path enabled:

  export HIPVS_PREFIX=${PREFIX}
  export ROCM=${ROCM_PATH}
  bazel build -c opt //src/observer:observer \\
    --copt=-DSEEKDB_WITH_HIPVS \\
    --copt=-I\${HIPVS_PREFIX}/include \\
    --copt=-I\${ROCM}/include \\
    --linkopt=-L\${HIPVS_PREFIX}/lib -Wl,-rpath,\${HIPVS_PREFIX}/lib \\
    --linkopt=-L\${ROCM}/lib -Wl,-rpath,\${ROCM}/lib \\
    --linkopt=-lhipvs

Without SEEKDB_WITH_HIPVS the build never touches ROCm and the HNSW index is
built on the host exactly as before, so the two builds can be compared to
measure the speedup.
EOF
