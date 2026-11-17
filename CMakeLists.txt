cmake_minimum_required(VERSION 3.16)
project(minivectordb CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(MVDB_SOURCES
  src/main.cpp
  src/json.cpp
  src/parser.cpp
  src/engine.cpp
  src/rowstore.cpp
  src/storage.cpp
  src/vector.cpp)

add_executable(minivectordb ${MVDB_SOURCES})
target_compile_options(minivectordb PRIVATE -Wall -Wextra -Wno-unused-parameter)

# 单测：每个模块一个可执行，ctest 直接跑
enable_testing()
foreach(mod storage vector)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/${mod}_test.cpp)
    add_executable(${mod}_test tests/${mod}_test.cpp src/${mod}.cpp)
    target_include_directories(${mod}_test PRIVATE src)
    target_compile_options(${mod}_test PRIVATE -Wall -Wextra -Wno-unused-parameter)
    add_test(NAME ${mod}_test COMMAND ${mod}_test)
  endif()
endforeach()
