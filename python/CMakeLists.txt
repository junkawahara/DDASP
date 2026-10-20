cmake_minimum_required(VERSION 3.15)
project(ddasp_python CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(pybind11 REQUIRED)

set(DDASP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." CACHE PATH "Path to the DDASP root directory")

# Build the DDASP core (and KyotoDD through it) with the project's own CMake
# files, as position-independent static libraries linked into the extension.
# The extension therefore carries its own KyotoDD node table, separate from the
# kyotodd Python module's; results cross over as export_binary bytes.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(DDASP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${DDASP_ROOT}" ddasp_core EXCLUDE_FROM_ALL)

pybind11_add_module(_core src/ddasp/_binding.cpp)
target_link_libraries(_core PRIVATE ddasp_lib)

install(TARGETS _core DESTINATION ddasp)
