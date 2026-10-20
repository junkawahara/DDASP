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

# KyotoDD (symlink to ../KyotoDD). Only kyotodd_lib is needed; its apps and
# tests are excluded from the default build.
set(KYOTODD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(KyotoDD EXCLUDE_FROM_ALL)

add_library(ddasp_lib STATIC
  src/program.cpp
  src/aspif_reader.cpp
  src/dd_util.cpp
  src/compiler.cpp
)
target_include_directories(ddasp_lib PUBLIC include)
target_link_libraries(ddasp_lib PUBLIC kyotodd_lib)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(ddasp_lib PRIVATE -Wall -Wextra)
endif()

option(DDASP_BUILD_TESTS "Build tests (GoogleTest via FetchContent)" ON)

if(DDASP_BUILD_TESTS)
  include(FetchContent)
  # Reuse the checkout made by KyotoDD's build if present (avoids a download).
  set(_kyotodd_gtest_src "${CMAKE_CURRENT_SOURCE_DIR}/KyotoDD/build/_deps/googletest-src")
  if(EXISTS "${_kyotodd_gtest_src}/CMakeLists.txt" AND NOT FETCHCONTENT_SOURCE_DIR_GOOGLETEST)
    set(FETCHCONTENT_SOURCE_DIR_GOOGLETEST "${_kyotodd_gtest_src}" CACHE PATH "" FORCE)
  endif()
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/release-1.12.1.tar.gz
    URL_HASH SHA256=81964fe578e9bd7c94dfdb09c8e4d6e6759e19967e397dbea48d1c10e45d0df2
  )
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()

  add_executable(program_test test/test_program.cpp)
  target_link_libraries(program_test ddasp_lib GTest::gtest_main)
  add_test(NAME program_test COMMAND program_test)

  add_executable(aspif_reader_test test/test_aspif_reader.cpp)
  target_link_libraries(aspif_reader_test ddasp_lib GTest::gtest_main)
  add_test(NAME aspif_reader_test COMMAND aspif_reader_test)
endif()

if(DDASP_BUILD_TESTS)
  add_executable(compiler_test test/test_compiler.cpp)
  target_link_libraries(compiler_test ddasp_lib GTest::gtest_main)
  add_test(NAME compiler_test COMMAND compiler_test)
endif()

add_executable(ddasp app/ddasp/main.cpp)
target_link_libraries(ddasp ddasp_lib)
