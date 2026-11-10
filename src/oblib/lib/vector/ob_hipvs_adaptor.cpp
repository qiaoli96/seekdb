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

#if defined(SEEKDB_WITH_HIPVS)
#include <cstdio>
#include <cstdlib>
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

namespace {

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
  cuvsResources_t resources = nullptr;
  cuvsCagraIndexParams_t cagra_params = nullptr;
  cuvsCagraIndex_t cagra_index = nullptr;
  cuvsHnswIndexParams_t hnsw_params = nullptr;
  cuvsHnswIndex_t hnsw_index = nullptr;
  DLManagedTensor dataset;
  std::string tmp_file;

  if (!is_compiled_with_hipvs()) {
    ret = OB_NOT_SUPPORTED;
  } else if (nullptr == param.vectors_ || param.count_ <= 0 || param.dim_ <= 0
             || nullptr == param.vids_ || param.vid_count_ != param.count_) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(map_metric(param.metric_, metric))) {
    // ret is set by map_metric.
  } else if (param.max_degree_ <= 0 || param.ef_construction_ <= 0) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(make_temp_index_file(tmp_file))) {
    // ret is set by make_temp_index_file.
  } else if (CUVS_SUCCESS != cuvsResourcesCreate(&resources)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (CUVS_SUCCESS != cuvsCagraIndexParamsCreate(&cagra_params)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (CUVS_SUCCESS != cuvsCagraIndexCreate(&cagra_index)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (CUVS_SUCCESS != cuvsHnswIndexParamsCreate(&hnsw_params)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (CUVS_SUCCESS != cuvsHnswIndexCreate(&hnsw_index)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    cagra_params->metric = metric;
    // The intermediate graph keeps twice the degree so that the pruning step
    // still leaves the requested number of neighbours.
    cagra_params->graph_degree = static_cast<size_t>(param.max_degree_);
    cagra_params->intermediate_graph_degree = static_cast<size_t>(2 * param.max_degree_);
    cagra_params->build_algo = NN_DESCENT;
    cagra_params->nn_descent_niter = 20;
    // The upper HNSW layers stay on the host by default: the base layer graph
    // produced by CAGRA is the expensive part, and the host hierarchy keeps
    // the exported index in the original hnswlib layout.
    hnsw_params->hierarchy = param.gpu_hierarchy_ ? GPU : CPU;
    hnsw_params->ef_construction = param.ef_construction_;
    hnsw_params->num_threads = param.num_threads_;

    memset(&dataset, 0, sizeof(dataset));
    dataset.dl_tensor.data = const_cast<float *>(param.vectors_);
    dataset.dl_tensor.device = {kDLCPU, 0};
    dataset.dl_tensor.ndim = 2;
    dataset.dl_tensor.shape = {param.count_, static_cast<int64_t>(param.dim_)};
    dataset.dl_tensor.strides = {param.dim_, 1};
    dataset.dl_tensor.dtype = {kDLFloat, 32, 1};
    dataset.dl_tensor.byte_offset = 0;

    if (CUVS_SUCCESS != cuvsCagraBuild(resources, cagra_params, &dataset, cagra_index)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (CUVS_SUCCESS != cuvsHnswFromCagra(resources, hnsw_params, cagra_index, hnsw_index)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (CUVS_SUCCESS != cuvsHnswSerialize(resources, tmp_file.c_str(), hnsw_index)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      index_file = tmp_file;
      tmp_file.clear();
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
  if (nullptr != resources) {
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

} // namespace obhipvs
} // namespace common
} // namespace oceanbase
