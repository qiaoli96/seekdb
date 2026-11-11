/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <stdint.h>
#include <string>
#include <vector>

namespace oceanbase {
namespace common {
namespace obhipvs {

/**
 * @brief Parameters of one GPU HNSW build executed by hipVS.
 *
 * hipVS is the AMD (ROCm) port of the RAPIDS cuVS vector search library. It
 * builds the kNN graph of an HNSW index on an AMD Radeon GPU and exports the
 * result in the hnswlib binary layout. The vectors stay on the host, hipVS
 * copies them to the device internally.
 */
struct HipvsBuildParam
{
  /// Dense float32 vectors, row major, count_ * dim_ elements.
  const float *vectors_;
  /// Number of vectors.
  int64_t count_;
  /// Dimension of every vector.
  int dim_;
  /// External ids of the vectors, one per vector.
  const int64_t *vids_;
  /// Number of external ids.
  int64_t vid_count_;
  /// Distance metric, "l2" or "ip". Other metrics are not supported.
  const char *metric_;
  /// HNSW graph degree, the "m" index parameter.
  int max_degree_;
  /// HNSW construction candidate list size, the "ef_construction" parameter.
  int ef_construction_;
  /// Build the upper HNSW layers on the GPU instead of the host.
  bool gpu_hierarchy_;
  /// Number of host threads used for the parts that stay on the host.
  int num_threads_;
  /// AMD GPU ordinal to build on.
  int device_id_;
};

/**
 * @brief Description of the AMD GPUs visible to the current process.
 */
struct HipvsDeviceInfo
{
  /// Number of visible AMD GPUs.
  int device_count_;
  /// Marketing name of the first visible AMD GPU.
  char device_name_[256];
};

/**
 * @brief Whether this build was compiled with hipVS support.
 *
 * The GPU build path is compiled in only when SEEKDB_WITH_HIPVS is defined.
 * Without it every entry point below degrades to a no-op, so a normal build
 * never depends on ROCm.
 */
bool is_compiled_with_hipvs();

/**
 * @brief Whether the GPU build path is enabled at run time.
 *
 * The GPU build path is compiled in when the AMD stack was found at build
 * time. It can be turned off again at run time with the environment variable
 * SEEKDB_HNSW_GPU_BUILD=0, so that one and the same binary can be used to
 * measure the host build for comparison.
 */
bool is_gpu_build_enabled();

/**
 * @brief Query the AMD GPUs visible to the current process.
 *
 * @param[out] info  Filled on success.
 * @retval OB_SUCCESS        At least one AMD GPU was found.
 * @retval OB_NOT_SUPPORTED  hipVS support was not compiled in.
 * @retval OB_ERR_UNEXPECTED hipVS is present but no usable GPU was found.
 */
int get_device_info(HipvsDeviceInfo &info);

/**
 * @brief Build an HNSW index on an AMD GPU and export it in the hnswlib layout.
 *
 * The caller owns the returned file and must remove it after loading it into
 * the vector index backend.
 *
 * @param[in]  param        Build parameters.
 * @param[out] index_file   Path of the exported hnswlib index file.
 * @retval OB_SUCCESS        The index was built and exported.
 * @retval OB_NOT_SUPPORTED hipVS support was not compiled in.
 * @retval OB_ERR_UNEXPECTED The GPU build failed, the caller has to fall back
 *                           to the host build path.
 */
int build_hnsw_index(const HipvsBuildParam &param, std::string &index_file);

/**
 * @brief Convert an index file exported by hipVS into the binary vsag loads.
 *
 * hipVS exports the index in the hnswlib layout. vsag reads the same layout,
 * with two additions: three counters of the product quantizer are written
 * between the header and the data, and the distance of every node to the
 * closest product quantizer centroid is written behind the data. An index
 * that does not use the product quantizer has zeroes in both places, so the
 * conversion copies the header, writes the three zeroes, copies the rest of
 * the file and writes the zero distances behind it.
 *
 * The external ids are written into the labels of the level 0 data, because
 * hipVS labels the rows with their position in the input array while vsag
 * reports the label of a hit to the caller.
 *
 * @param[in]  hnswlib_file  Path of the index file exported by hipVS.
 * @param[in]  vids          External ids of the rows, one per row.
 * @param[in]  vid_count     Number of external ids.
 * @param[out] vsag_data     The binary that vsag loads.
 * @retval OB_SUCCESS        The file was converted.
 * @retval OB_ERR_UNEXPECTED The file is not an index of the expected shape.
 */
int convert_hnswlib_to_vsag_data(const std::string &hnswlib_file,
                                 const int64_t *vids,
                                 int64_t vid_count,
                                 std::vector<char> &vsag_data);

} // namespace obhipvs
} // namespace common
} // namespace oceanbase
