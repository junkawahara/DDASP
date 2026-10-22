cmake_minimum_required(VERSION 3.14)
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()
project(DDASP CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# KyotoDD (git submodule in third_party/). Only kyotodd_lib is needed; its apps and
# tests are excluded from the default build.
set(KYOTODD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/KyotoDD EXCLUDE_FROM_ALL)

add_library(ddasp_lib STATIC
  src/program.cpp
  src/aspif_reader.cpp
  src/dd_util.cpp
  src/compiler.cpp
  src/stored_dd.cpp
  src/consequences.cpp
  src/cost_order.cpp
)
target_include_directories(ddasp_lib PUBLIC include)
target_link_libraries(ddasp_lib PUBLIC kyotodd_lib)

# Warnings for every DDASP target (not for KyotoDD or GoogleTest).
function(ddasp_warnings target)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${target} PRIVATE -Wall -Wextra)
  endif()
endfunction()
ddasp_warnings(ddasp_lib)

add_executable(ddasp app/ddasp/main.cpp)
target_link_libraries(ddasp ddasp_lib)
ddasp_warnings(ddasp)

option(DDASP_BUILD_TESTS "Build tests (GoogleTest via FetchContent)" ON)

if(DDASP_BUILD_TESTS)
  include(FetchContent)
  # Reuse an existing GoogleTest checkout if one is given (avoids a download), e.g.
  #   cmake -S . -B build -DDDASP_GTEST_SOURCE_DIR=path/to/googletest-src
  set(DDASP_GTEST_SOURCE_DIR "" CACHE PATH "Existing GoogleTest source checkout to reuse")
  if(DDASP_GTEST_SOURCE_DIR AND EXISTS "${DDASP_GTEST_SOURCE_DIR}/CMakeLists.txt"
     AND NOT FETCHCONTENT_SOURCE_DIR_GOOGLETEST)
    set(FETCHCONTENT_SOURCE_DIR_GOOGLETEST "${DDASP_GTEST_SOURCE_DIR}" CACHE PATH "" FORCE)
  endif()
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/release-1.12.1.tar.gz
    URL_HASH SHA256=81964fe578e9bd7c94dfdb09c8e4d6e6759e19967e397dbea48d1c10e45d0df2
  )
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()

  foreach(name program aspif_reader compiler stored_dd consequences cost_order)
    add_executable(${name}_test test/test_${name}.cpp)
    target_link_libraries(${name}_test ddasp_lib GTest::gtest_main)
    ddasp_warnings(${name}_test)
    add_test(NAME ${name}_test COMMAND ${name}_test)
  endforeach()
endif()
