cmake_minimum_required(VERSION 3.15)
project(ddasp_python CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(pybind11 REQUIRED)

set(DDASP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." CACHE PATH "Path to the DDASP root directory")
set(KYOTODD_ROOT "${DDASP_ROOT}/third_party/KyotoDD" CACHE PATH "Path to the KyotoDD root directory")

# The extension carries its own copy of KyotoDD (a separate node table from the
# kyotodd Python module); results cross over as export_binary bytes.
file(GLOB KYOTODD_SOURCES "${KYOTODD_ROOT}/src/*.cpp")
list(FILTER KYOTODD_SOURCES EXCLUDE REGEX "/main\\.cpp$")
file(GLOB DDASP_SOURCES "${DDASP_ROOT}/src/*.cpp")

pybind11_add_module(_core
    src/ddasp/_binding.cpp
    ${DDASP_SOURCES}
    ${KYOTODD_SOURCES}
)
target_include_directories(_core PRIVATE "${DDASP_ROOT}/include" "${KYOTODD_ROOT}/include")
if(EXISTS "${KYOTODD_ROOT}/third_party/TdZdd/include/tdzdd/DdSpec.hpp")
  target_include_directories(_core SYSTEM PRIVATE "${KYOTODD_ROOT}/third_party/TdZdd/include")
endif()

install(TARGETS _core DESTINATION ddasp)
