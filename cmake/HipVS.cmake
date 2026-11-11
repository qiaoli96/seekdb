# Detect the optional AMD hipVS GPU stack.
#
# hipVS is the ROCm port of the RAPIDS cuVS vector search library:
#   https://github.com/AMD-Ecosystem/hipVS
#
# The vector index build path in src/oblib/lib/vector compiles the GPU
# accelerated HNSW builder behind the SEEKDB_WITH_HIPVS macro.  This module
# decides at configure time whether that macro is defined.  The GPU path is
# only enabled when BOTH halves of the stack are present:
#
#   * the hipVS SDK: the cuVS headers (cuvs/neighbors/hnsw.h) and libhipvs,
#   * the ROCm runtime: the HIP headers (hip/hip_runtime.h) and libamdhip64.
#
# When the stack is absent nothing at all is added to the build: no compile
# definition, no include directory and no link library.  The default build
# therefore keeps its host only behaviour.
#
# The module is included once, from src/oblib/CMakeLists.txt, which is the
# place where the oblib include directories and link libraries are assembled.

# User facing switch.  AUTO (the default) enables the GPU path only when the
# whole stack was found, ON requires it and OFF disables it.
set(OB_WITH_HIPVS "AUTO" CACHE STRING
  "Enable the AMD hipVS GPU accelerated HNSW build path (AUTO, ON or OFF)")
set_property(CACHE OB_WITH_HIPVS PROPERTY STRINGS AUTO ON OFF)

# Normalize the request so that -DOB_WITH_HIPVS=ON, =TRUE and =1 behave the
# same way.
string(TOUPPER "${OB_WITH_HIPVS}" ob_hipvs_request)
if(ob_hipvs_request STREQUAL "TRUE" OR ob_hipvs_request STREQUAL "YES"
   OR ob_hipvs_request STREQUAL "1")
  set(ob_hipvs_request "ON")
elseif(ob_hipvs_request STREQUAL "FALSE" OR ob_hipvs_request STREQUAL "NO"
   OR ob_hipvs_request STREQUAL "0")
  set(ob_hipvs_request "OFF")
elseif(NOT ob_hipvs_request STREQUAL "ON" AND NOT ob_hipvs_request STREQUAL "OFF")
  set(ob_hipvs_request "AUTO")
endif()

# Resolved state, exported for the rest of the build.
set(OB_HIPVS_ENABLED OFF)
set(OB_HIPVS_FOUND OFF)
set(OB_HIPVS_INCLUDE_DIRS "")
set(OB_HIPVS_LIBRARIES "")
set(OB_HIPVS_RPATH_DIRS "")

if(ob_hipvs_request STREQUAL "OFF")
  message(STATUS "hipVS GPU support: disabled by OB_WITH_HIPVS=OFF")
  return()
endif()

# The AMD GPU stack only exists on Linux.  Everywhere else the search below
# would only produce false positives.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(STATUS "hipVS GPU support: disabled (${CMAKE_SYSTEM_NAME} is not a supported platform)")
  return()
endif()

# Roots that may hold the hipVS SDK, in priority order.  OB_HIPVS_DIR and the
# environment variables come first, then the usual system prefixes, then the
# competition notebook layout relative to the build workspace.
set(ob_hipvs_roots "")
foreach(ob_hipvs_root
    "${OB_HIPVS_DIR}"
    "$ENV{SEEKDB_HIPVS_DIR}"
    "$ENV{OB_HIPVS_DIR}"
    "/opt/hipvs"
    "/usr/local/hipvs"
    "${ROCM_PATH}"
    "${CMAKE_SOURCE_DIR}/seekdb_hipvs_notebook"
    "${CMAKE_SOURCE_DIR}/../seekdb_hipvs_notebook"
    "${CMAKE_SOURCE_DIR}/../../seekdb_hipvs_notebook")
  if(ob_hipvs_root)
    list(APPEND ob_hipvs_roots "${ob_hipvs_root}")
  endif()
endforeach()

# The include directory of the SDK holds cuvs/ and dlpack/, the library
# directory holds libhipvs.
set(ob_hipvs_include_dirs "")
set(ob_hipvs_lib_dirs "")
foreach(ob_hipvs_root IN LISTS ob_hipvs_roots)
  list(APPEND ob_hipvs_include_dirs "${ob_hipvs_root}/include")
  list(APPEND ob_hipvs_lib_dirs "${ob_hipvs_root}/lib")
  list(APPEND ob_hipvs_lib_dirs "${ob_hipvs_root}/lib64")
endforeach()

# The ROCm runtime is searched separately: it is installed independently from
# the hipVS SDK.
set(ob_rocm_include_dirs "")
foreach(ob_rocm_root "${ROCM_PATH}" "/opt/rocm" "$ENV{ROCM_PATH}" "/opt/hipvs")
  if(ob_rocm_root)
    list(APPEND ob_rocm_include_dirs "${ob_rocm_root}/include")
  endif()
endforeach()

set(ob_rocm_lib_dirs "")
foreach(ob_rocm_root "${ROCM_PATH}" "/opt/rocm" "$ENV{ROCM_PATH}")
  if(ob_rocm_root)
    list(APPEND ob_rocm_lib_dirs "${ob_rocm_root}/lib")
  endif()
endforeach()

# A cached NOTFOUND from an earlier configure must not silently disable a
# stack that has been installed in the meantime, so only the NOTFOUND
# sentinels are dropped here.  An explicit -DOB_HIPVS_INCLUDE_DIR=... (or any
# other explicitly provided result) survives.
foreach(ob_hipvs_cache_var
    OB_HIPVS_INCLUDE_DIR OB_HIPVS_LIBRARY
    OB_ROCM_INCLUDE_DIR OB_ROCM_LIBRARY)
  if(DEFINED ${ob_hipvs_cache_var} AND "${${ob_hipvs_cache_var}}" MATCHES "-NOTFOUND$")
    unset(${ob_hipvs_cache_var} CACHE)
  endif()
endforeach()
unset(ob_hipvs_cache_var)

# HINTS keeps the priority order above: find_path searches the hints before
# the standard system directories, and the same list is repeated in PATHS so
# the intent stays visible.
find_path(OB_HIPVS_INCLUDE_DIR
  NAMES cuvs/neighbors/hnsw.h
  HINTS ${ob_hipvs_include_dirs}
  PATHS ${ob_hipvs_include_dirs})
find_library(OB_HIPVS_LIBRARY
  NAMES cuvs_c cuvs hipvs
  HINTS ${ob_hipvs_lib_dirs}
  PATHS ${ob_hipvs_lib_dirs})
# The C API of hipVS lives in its own shared object, the C++ API in another
# one. The GPU build path only calls the C API, but the C API object depends
# on the C++ one, so both are linked when they are present.
find_library(OB_HIPVS_CXX_LIBRARY
  NAMES cuvs hipvs
  HINTS ${ob_hipvs_lib_dirs}
  PATHS ${ob_hipvs_lib_dirs})
find_path(OB_ROCM_INCLUDE_DIR
  NAMES hip/hip_runtime.h
  HINTS ${ob_rocm_include_dirs}
  PATHS ${ob_rocm_include_dirs})
find_library(OB_ROCM_LIBRARY
  NAMES amdhip64
  HINTS ${ob_rocm_lib_dirs}
  PATHS ${ob_rocm_lib_dirs})

# A half installed SDK must not enable the GPU path: every header the
# SEEKDB_WITH_HIPVS branch includes has to be present.
set(ob_hipvs_missing "")
if(OB_HIPVS_INCLUDE_DIR)
  foreach(ob_hipvs_header
      "cuvs/core/c_api.h"
      "cuvs/neighbors/cagra.h"
      "cuvs/neighbors/hnsw.h"
      "dlpack/dlpack.h")
    if(NOT EXISTS "${OB_HIPVS_INCLUDE_DIR}/${ob_hipvs_header}")
      list(APPEND ob_hipvs_missing "${ob_hipvs_header}")
    endif()
  endforeach()
  if(ob_hipvs_missing)
    message(STATUS "hipVS GPU support: ${OB_HIPVS_INCLUDE_DIR} is incomplete"
      " (missing ${ob_hipvs_missing})")
  endif()
endif()

if(NOT OB_HIPVS_INCLUDE_DIR OR NOT OB_HIPVS_LIBRARY OR NOT OB_ROCM_INCLUDE_DIR
   OR NOT OB_ROCM_LIBRARY OR ob_hipvs_missing)
  if(ob_hipvs_request STREQUAL "ON")
    message(FATAL_ERROR
      "OB_WITH_HIPVS=ON was requested but the AMD GPU stack is incomplete. "
      "hipVS include dir: ${OB_HIPVS_INCLUDE_DIR}, hipVS library: ${OB_HIPVS_LIBRARY}, "
      "ROCm include dir: ${OB_ROCM_INCLUDE_DIR}, ROCm library: ${OB_ROCM_LIBRARY}. "
      "Point the build at the stack with -DOB_HIPVS_DIR=<hipvs prefix> and "
      "-DROCM_PATH=<rocm prefix>, or with the SEEKDB_HIPVS_DIR and ROCM_PATH "
      "environment variables.")
  endif()
  message(STATUS "hipVS GPU support: disabled (hipVS SDK and/or ROCm runtime not found)")
  return()
endif()

set(OB_HIPVS_FOUND ON)
set(OB_HIPVS_INCLUDE_DIRS "${OB_HIPVS_INCLUDE_DIR}" "${OB_ROCM_INCLUDE_DIR}")
set(OB_HIPVS_LIBRARIES "${OB_HIPVS_LIBRARY}" "${OB_ROCM_LIBRARY}")
if(OB_HIPVS_CXX_LIBRARY AND NOT OB_HIPVS_CXX_LIBRARY STREQUAL OB_HIPVS_LIBRARY)
  list(APPEND OB_HIPVS_LIBRARIES "${OB_HIPVS_CXX_LIBRARY}")
endif()
get_filename_component(ob_hipvs_lib_dir "${OB_HIPVS_LIBRARY}" DIRECTORY)
get_filename_component(ob_rocm_lib_dir "${OB_ROCM_LIBRARY}" DIRECTORY)
set(OB_HIPVS_RPATH_DIRS "${ob_hipvs_lib_dir}" "${ob_rocm_lib_dir}")

# The GPU translation unit is compiled by the project compiler, not by hipcc.
# Compile the very same includes once here so that a stack that the project
# compiler cannot consume never breaks the build: the GPU path simply stays
# off and the host build is used.
include(CheckCXXSourceCompiles)
if(DEFINED OB_HIPVS_HEADERS_COMPILE AND NOT OB_HIPVS_HEADERS_COMPILE STREQUAL "1")
  # A cached failure belongs to an older toolchain, run the check again.
  unset(OB_HIPVS_HEADERS_COMPILE CACHE)
endif()
set(CMAKE_REQUIRED_INCLUDES ${OB_HIPVS_INCLUDE_DIRS})
# __HIP_PLATFORM_AMD__ selects the HIP flavour of the cuVS/hipVS headers. The
# project compiler is not hipcc, so the macro has to be set explicitly,
# otherwise the headers would try to include cuda_runtime.h and the GPU path
# would silently stay off.
set(CMAKE_REQUIRED_DEFINITIONS "-DSEEKDB_WITH_HIPVS=1" "-D__HIP_PLATFORM_AMD__")
set(CMAKE_REQUIRED_LIBRARIES ${OB_HIPVS_LIBRARIES})
set(CMAKE_REQUIRED_LINK_OPTIONS "")
foreach(ob_hipvs_rpath_dir IN LISTS OB_HIPVS_RPATH_DIRS)
  list(APPEND CMAKE_REQUIRED_LINK_OPTIONS
    "-Wl,-rpath,${ob_hipvs_rpath_dir}"
    "-Wl,-rpath-link,${ob_hipvs_rpath_dir}")
endforeach()
check_cxx_source_compiles("
#include <hip/hip_runtime.h>
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
#include <cuvs/neighbors/hnsw.h>
#include <dlpack/dlpack.h>
int main() { return 0; }
" OB_HIPVS_HEADERS_COMPILE)
unset(CMAKE_REQUIRED_INCLUDES)
unset(CMAKE_REQUIRED_DEFINITIONS)
unset(CMAKE_REQUIRED_LIBRARIES)
unset(CMAKE_REQUIRED_LINK_OPTIONS)
unset(ob_hipvs_rpath_dir)

if(NOT OB_HIPVS_HEADERS_COMPILE)
  if(ob_hipvs_request STREQUAL "ON")
    message(FATAL_ERROR
      "OB_WITH_HIPVS=ON was requested but the hipVS and the ROCm headers do not "
      "compile with ${CMAKE_CXX_COMPILER}. See the CMakeError.log in the build "
      "directory for the compiler output.")
  endif()
  message(WARNING
    "hipVS GPU support: disabled (the hipVS and the ROCm headers do not compile "
    "with ${CMAKE_CXX_COMPILER}, see CMakeError.log in the build directory)")
  return()
endif()

set(OB_HIPVS_ENABLED ON)
message(STATUS "hipVS GPU support: enabled")
message(STATUS "hipVS include dir: ${OB_HIPVS_INCLUDE_DIR}")
message(STATUS "hipVS library: ${OB_HIPVS_LIBRARY}")
message(STATUS "ROCm include dir: ${OB_ROCM_INCLUDE_DIR}")
message(STATUS "ROCm library: ${OB_ROCM_LIBRARY}")
