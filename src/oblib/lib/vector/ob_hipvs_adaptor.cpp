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

#include "ob_hipvs_adaptor.h"

#include "lib/ob_errno.h"
#include "lib/utility/ob_macro_utils.h"

#include <cstring>
#include <strings.h>
#include <fstream>
#include <vector>

#if defined(SEEKDB_WITH_HIPVS)
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <unistd.h>
#include <hip/hip_runtime.h>
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
#include <cuvs/neighbors/hnsw.h>
#include <dlpack/dlpack.h>
#endif

namespace oceanbase {
namespace common {
namespace obhipvs {

bool is_compiled_with_hipvs()
{
#if defined(SEEKDB_WITH_HIPVS)
  return true;
#else
  return false;
#endif
}

#if defined(SEEKDB_WITH_HIPVS)

bool is_gpu_build_enabled()
{
  // The GPU build path is opt in. The hipVS build path is compiled in when the
  // AMD stack is present, but it is only taken when the operator explicitly
  // asks for it: an unusable stack then can never change the behaviour of a
  // deployment that did not ask for it.
  const char *env = getenv("SEEKDB_HNSW_GPU_BUILD");
  if (nullptr == env) {
    return false;
  }
  return (0 == strcasecmp(env, "1") || 0 == strcasecmp(env, "true")
          || 0 == strcasecmp(env, "on") || 0 == strcasecmp(env, "yes"));
}

namespace {

/**
 * @brief Write a trace line to the error stream.
 *
 * The trace is written with the write system call and not with fprintf,
 * because a redirected stream is buffered and the buffer is lost when the
 * process dies. The trace is not lost, so the last line that was written
 * before a crash is the place where the crash happened.
 */
void trace(const char *msg)
{
  const ssize_t ignored = ::write(2, msg, strlen(msg));
  (void)ignored;
}

void trace_num(const char *prefix, int64_t value, const char *suffix)
{
  char buf[160];
  const int len = snprintf(buf, sizeof(buf), "%s%lld%s", prefix, (long long)value, suffix);
  if (len > 0) {
    const ssize_t ignored = ::write(2, buf, (size_t)len);
    (void)ignored;
  }
}

int64_t monotonic_ms()
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}

int map_metric(const char *metric, cuvsDistanceType &mapped)
{
  int ret = OB_SUCCESS;
  if (nullptr == metric) {
    ret = OB_INVALID_ARGUMENT;
  } else if (0 == strcasecmp(metric, "l2")) {
    mapped = L2Expanded;
  } else if (0 == strcasecmp(metric, "ip")) {
    mapped = InnerProduct;
  } else {
    // The cosine distance would need the vectors to be normalized before the
    // upload, which changes the distance values the index reports.
    ret = OB_NOT_SUPPORTED;
  }
  return ret;
}

/**
 * @brief Create the temporary file the exported index is written to.
 */
int make_temp_index_file(std::string &index_file)
{
  int ret = OB_SUCCESS;
  char tmpl[] = "/tmp/hipvs_hnsw_XXXXXX";
  int fd = mkstemp(tmpl);
  if (fd < 0) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    close(fd);
    index_file.assign(tmpl);
  }
  return ret;
}

} // namespace

int get_device_info(HipvsDeviceInfo &info)
{
  int ret = OB_SUCCESS;
  int device_count = 0;
  if (hipSuccess != hipGetDeviceCount(&device_count) || device_count <= 0) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    info.device_count_ = device_count;
    info.device_name_[0] = '\0';
    hipDeviceProp_t prop;
    if (hipSuccess == hipGetDeviceProperties(&prop, 0)) {
      strncpy(info.device_name_, prop.name, sizeof(info.device_name_) - 1);
      info.device_name_[sizeof(info.device_name_) - 1] = '\0';
    }
  }
  return ret;
}

int build_hnsw_index(const HipvsBuildParam &param, std::string &index_file)
{
  int ret = OB_SUCCESS;
  cuvsDistanceType metric = L2Expanded;
  cuvsResources_t resources = 0;
  cuvsCagraIndexParams_t cagra_params = nullptr;
  cuvsCagraIndex_t cagra_index = nullptr;
  cuvsHnswIndexParams_t hnsw_params = nullptr;
  cuvsHnswIndex_t hnsw_index = nullptr;
  DLManagedTensor dataset;
  std::string tmp_file;

  trace("[HIPVS] M1 enter build_hnsw_index\n");

  if (!is_compiled_with_hipvs()) {
    ret = OB_NOT_SUPPORTED;
  } else if (nullptr == param.vectors_ || param.count_ <= 0 || param.dim_ <= 0
             || nullptr == param.vids_ || param.vid_count_ != param.count_) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(map_metric(param.metric_, metric))) {
    // ret is set by map_metric.
  } else if (param.max_degree_ < 2 || param.ef_construction_ <= 0) {
    // hipVS derives the degree of the index from the degree of the graph it
    // builds, a degree below two would leave the index without any neighbour.
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(make_temp_index_file(tmp_file))) {
    // ret is set by make_temp_index_file.
  } else {
    // The device has to be selected before the first GPU call. The resources
    // of cuVS are empty when they are created, the device is resolved on the
    // first call that needs it, and the resolution fails when no device was
    // ever selected.
    if (hipSuccess != hipSetDevice(param.device_id_)) {
      ret = OB_ERR_UNEXPECTED;
      trace("[HIPVS] M2 hipSetDevice failed\n");
    } else if (CUVS_SUCCESS != cuvsResourcesCreate(&resources)) {
      ret = OB_ERR_UNEXPECTED;
      trace("[HIPVS] M2 cuvsResourcesCreate failed\n");
    } else {
      trace("[HIPVS] M2 resources created\n");
    }
  }

  if (OB_SUCC(ret)) {
    if (CUVS_SUCCESS != cuvsCagraIndexParamsCreate(&cagra_params)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (CUVS_SUCCESS != cuvsCagraIndexCreate(&cagra_index)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (CUVS_SUCCESS != cuvsHnswIndexParamsCreate(&hnsw_params)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (CUVS_SUCCESS != cuvsHnswIndexCreate(&hnsw_index)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      cagra_params->metric = metric;
      // The degree of the graph is twice the degree of the index: hipVS
      // derives the degree of the HNSW index from the degree of the graph
      // (M = graph_degree / 2) and it writes graph_degree neighbours into the
      // maxM0_ slots of the level 0 graph. The graph degree therefore has to
      // be even, otherwise the graph does not fit into the slots and the
      // write runs over the end of every element.
      cagra_params->graph_degree = 2 * static_cast<size_t>(param.max_degree_);
      // The intermediate graph keeps more neighbours than the final graph, so
      // that the pruning step still leaves the requested number of them.
      cagra_params->intermediate_graph_degree = 3 * static_cast<size_t>(param.max_degree_);
      cagra_params->nn_descent_niter = 20;
      // The upper HNSW layers stay on the host: the base layer graph produced
      // by CAGRA is the expensive part, and the host hierarchy keeps the
      // exported index in the original hnswlib layout.
      hnsw_params->hierarchy = param.gpu_hierarchy_ ? cuvsHnswHierarchy::GPU : cuvsHnswHierarchy::CPU;
      hnsw_params->ef_construction = param.ef_construction_;
      hnsw_params->num_threads = param.num_threads_;

      memset(&dataset, 0, sizeof(dataset));
      // DLPack keeps the shape and the strides in arrays the caller owns.
      int64_t shape[2] = {param.count_, static_cast<int64_t>(param.dim_)};
      int64_t strides[2] = {static_cast<int64_t>(param.dim_), 1};
      dataset.dl_tensor.data = const_cast<float *>(param.vectors_);
      dataset.dl_tensor.device = {kDLCPU, 0};
      dataset.dl_tensor.ndim = 2;
      dataset.dl_tensor.shape = shape;
      dataset.dl_tensor.strides = strides;
      dataset.dl_tensor.dtype = {kDLFloat, 32, 1};
      dataset.dl_tensor.byte_offset = 0;

      // The NN-Descent build is the fast one but it is still experimental in
      // hipVS, and the inner product metric is only supported by the IVF-PQ
      // build. The fast build is tried first and the build that is covered by
      // the tests of hipVS is the fallback.
      cuvsCagraGraphBuildAlgo build_algos[2];
      int build_algo_count = 0;
      if (InnerProduct != metric) {
        build_algos[build_algo_count++] = cuvsCagraGraphBuildAlgo::NN_DESCENT;
      }
      build_algos[build_algo_count++] = cuvsCagraGraphBuildAlgo::IVF_PQ;

      const int64_t build_begin = monotonic_ms();
      int64_t build_end = build_begin;
      for (int attempt = 0; attempt < build_algo_count && OB_SUCC(ret); ++attempt) {
        cagra_params->build_algo = build_algos[attempt];
        trace("[HIPVS] M3 cuvsCagraBuild begin\n");
        if (CUVS_SUCCESS != cuvsCagraBuild(resources, cagra_params, &dataset, cagra_index)) {
          ret = OB_ERR_UNEXPECTED;
          trace("[HIPVS] M3 cuvsCagraBuild failed\n");
          // The index is in an undefined state after a failed build, it is
          // built again for the next algorithm.
          cuvsCagraIndexDestroy(cagra_index);
          cagra_index = nullptr;
          if (CUVS_SUCCESS != cuvsCagraIndexCreate(&cagra_index)) {
            ret = OB_ERR_UNEXPECTED;
          }
        } else {
          build_end = monotonic_ms();
          trace("[HIPVS] M4 cuvsCagraBuild done\n");
        }
      }
      trace_num("[HIPVS] cagra_build_ms=", build_end - build_begin, "\n");

      if (OB_SUCC(ret)) {
        trace("[HIPVS] M5 cuvsHnswFromCagra begin\n");
        if (CUVS_SUCCESS != cuvsHnswFromCagra(resources, hnsw_params, cagra_index, hnsw_index)) {
          ret = OB_ERR_UNEXPECTED;
          trace("[HIPVS] M5 cuvsHnswFromCagra failed\n");
        } else {
          trace("[HIPVS] M6 cuvsHnswSerialize begin\n");
          if (CUVS_SUCCESS != cuvsHnswSerialize(resources, tmp_file.c_str(), hnsw_index)) {
            ret = OB_ERR_UNEXPECTED;
            trace("[HIPVS] M6 cuvsHnswSerialize failed\n");
          } else {
            trace("[HIPVS] M7 index exported\n");
            index_file = tmp_file;
            tmp_file.clear();
          }
        }
      }
    }
  }

  if (nullptr != hnsw_index) {
    cuvsHnswIndexDestroy(hnsw_index);
  }
  if (nullptr != hnsw_params) {
    cuvsHnswIndexParamsDestroy(hnsw_params);
  }
  if (nullptr != cagra_index) {
    cuvsCagraIndexDestroy(cagra_index);
  }
  if (nullptr != cagra_params) {
    cuvsCagraIndexParamsDestroy(cagra_params);
  }
  if (0 != resources) {
    cuvsResourcesDestroy(resources);
  }
  if (!tmp_file.empty()) {
    unlink(tmp_file.c_str());
  }
  return ret;
}

#else

int get_device_info(HipvsDeviceInfo &info)
{
  info.device_count_ = 0;
  info.device_name_[0] = '\0';
  return OB_NOT_SUPPORTED;
}

int build_hnsw_index(const HipvsBuildParam &param, std::string &index_file)
{
  UNUSED(param);
  index_file.clear();
  return OB_NOT_SUPPORTED;
}

#endif

namespace {

// The header of an index file in the hnswlib layout. Every field is written
// with its own size, there is no padding between the fields:
//   offsetLevel0_, max_elements_, cur_element_count, size_data_per_element_,
//   label_offset_, offsetData_ (8 bytes each), maxlevel_, enterpoint_node_
//   (4 bytes each), maxM_, maxM0_, M_ (8 bytes each), mult_ (4 bytes),
//   ef_construction_ (8 bytes).
const size_t HNSWLIB_HEADER_SIZE = 92;
// vsag writes three counters of the product quantizer behind the header.
const size_t VSAG_PQ_COUNTER_SIZE = 24;
// vsag writes the distance of every node to the closest product quantizer
// centroid behind the data. The index does not use the product quantizer, so
// all of them are zero.
const size_t VSAG_NODE_CLUSTER_DIST_SIZE = sizeof(float);

template <typename T>
T read_pod(const char *data)
{
  T value;
  memcpy(&value, data, sizeof(T));
  return value;
}

int convert_hnswlib_buffer(const std::vector<char> &file,
                           const int64_t *vids,
                           int64_t vid_count,
                           std::vector<char> &vsag_data)
{
  int ret = OB_SUCCESS;
  vsag_data.clear();
  const char *data = file.data();
  const uint64_t max_elements = read_pod<uint64_t>(data + 8);
  const uint64_t cur_element_count = read_pod<uint64_t>(data + 16);
  const uint64_t size_data_per_element = read_pod<uint64_t>(data + 24);
  const uint64_t label_offset = read_pod<uint64_t>(data + 32);

  if (cur_element_count != static_cast<uint64_t>(vid_count)
      || size_data_per_element < label_offset + sizeof(int64_t)
      || file.size() < HNSWLIB_HEADER_SIZE + cur_element_count * size_data_per_element) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    const size_t level0_size = static_cast<size_t>(cur_element_count) * static_cast<size_t>(size_data_per_element);
    // Behind the level 0 data the file holds the link lists of the upper
    // levels, they are copied unchanged.
    const size_t tail_size = file.size() - HNSWLIB_HEADER_SIZE - level0_size;
    const size_t dist_size = static_cast<size_t>(max_elements) * VSAG_NODE_CLUSTER_DIST_SIZE;
    vsag_data.resize(HNSWLIB_HEADER_SIZE + VSAG_PQ_COUNTER_SIZE + level0_size + tail_size + dist_size);
    char *out = vsag_data.data();
    memcpy(out, data, HNSWLIB_HEADER_SIZE);
    // The product quantizer is not used, its counters are zero.
    memset(out + HNSWLIB_HEADER_SIZE, 0, VSAG_PQ_COUNTER_SIZE);
    memcpy(out + HNSWLIB_HEADER_SIZE + VSAG_PQ_COUNTER_SIZE, data + HNSWLIB_HEADER_SIZE, level0_size + tail_size);
    // The labels of the rows are the positions of the rows in the input
    // array, they are replaced by the external ids of the rows.
    char *level0 = out + HNSWLIB_HEADER_SIZE + VSAG_PQ_COUNTER_SIZE;
    for (uint64_t i = 0; i < cur_element_count; ++i) {
      const int64_t vid = (nullptr != vids) ? vids[i] : static_cast<int64_t>(i);
      memcpy(level0 + i * size_data_per_element + label_offset, &vid, sizeof(vid));
    }
    // The distances to the product quantizer centroids are zero.
    memset(out + vsag_data.size() - dist_size, 0, dist_size);
  }
  return ret;
}

} // namespace

int convert_hnswlib_to_vsag_data(const std::string &hnswlib_file,
                                 const int64_t *vids,
                                 int64_t vid_count,
                                 std::vector<char> &vsag_data)
{
  int ret = OB_SUCCESS;
  vsag_data.clear();
  std::ifstream input(hnswlib_file.c_str(), std::ios::binary | std::ios::ate);
  if (!input.good()) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    const std::streamsize file_size = input.tellg();
    input.seekg(0, std::ios::beg);
    if (file_size < static_cast<std::streamsize>(HNSWLIB_HEADER_SIZE)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      std::vector<char> file(static_cast<size_t>(file_size));
      if (!input.read(file.data(), file_size)) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        ret = convert_hnswlib_buffer(file, vids, vid_count, vsag_data);
      }
    }
  }
  return ret;
}

} // namespace obhipvs
} // namespace common
} // namespace oceanbase
