# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.
cmake_minimum_required(VERSION 3.18)
project(blob_allocator_sim LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Python3 REQUIRED COMPONENTS Interpreter Development.Module)
execute_process(COMMAND "${Python3_EXECUTABLE}" -m pybind11 --cmakedir
    OUTPUT_VARIABLE PYBIND_DIR OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE PYBIND_RESULT)
if(NOT PYBIND_RESULT EQUAL 0)
    message(FATAL_ERROR "Cannot locate pybind11 for the selected interpreter.")
endif()
list(APPEND CMAKE_PREFIX_PATH "${PYBIND_DIR}")
set(PYBIND11_FINDPYTHON ON)
find_package(pybind11 CONFIG REQUIRED)
find_package(Threads REQUIRED)
get_filename_component(REPO "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
set(SRC "${REPO}/src/memcache/csrc")
set(JSON_HEADERS "" CACHE PATH "Optional directory containing nlohmann/json.hpp")
set(REPO_JSON_HEADERS "${REPO}/3rdparty/nlohmann/json/single_include")
# Treat the previous default cache value as automatic discovery.
if(JSON_HEADERS STREQUAL REPO_JSON_HEADERS)
    set(JSON_HEADERS "")
endif()
if(JSON_HEADERS)
    set(SELECTED_JSON_HEADERS "${JSON_HEADERS}")
elseif(EXISTS "${REPO_JSON_HEADERS}/nlohmann/json.hpp")
    set(SELECTED_JSON_HEADERS "${REPO_JSON_HEADERS}")
else()
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" -c "import nlohmann_json; print(nlohmann_json.get_include())"
        OUTPUT_VARIABLE PYTHON_JSON_HEADERS OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET RESULT_VARIABLE JSON_PACKAGE_RESULT)
    if(JSON_PACKAGE_RESULT EQUAL 0 AND EXISTS "${PYTHON_JSON_HEADERS}/nlohmann/json.hpp")
        set(SELECTED_JSON_HEADERS "${PYTHON_JSON_HEADERS}")
    else()
        find_path(SYSTEM_JSON_HEADERS NAMES nlohmann/json.hpp)
        set(SELECTED_JSON_HEADERS "${SYSTEM_JSON_HEADERS}")
    endif()
endif()
if(NOT EXISTS "${SELECTED_JSON_HEADERS}/nlohmann/json.hpp")
    message(FATAL_ERROR
        "Missing nlohmann/json.hpp.\n"
        "Install headers for the selected Python interpreter:\n"
        "  \"${Python3_EXECUTABLE}\" -m pip install nlohmann_json\n"
        "Or set BLOB_SIM_JSON_HEADERS to the directory containing nlohmann/json.hpp before running the simulator.\n"
        "For direct CMake use, supply -DJSON_HEADERS=<header root>.\n"
        "An explicit header path must contain nlohmann/json.hpp.")
endif()
message(STATUS "Blob allocator JSON headers: ${SELECTED_JSON_HEADERS}")
pybind11_add_module(_allocator_sim_native allocator_bindings.cpp
    "${SRC}/meta_service/mmc_best_fit_allocator.cpp"
    "${SRC}/meta_service/mmc_slab_allocator.cpp"
    "${SRC}/common/mmc_last_error.cpp")
target_include_directories(_allocator_sim_native PRIVATE "${REPO}/src/memcache/include"
    "${SRC}/common" "${SRC}/entities" "${SRC}/meta_service" "${SRC}/under_api/mf_smem" "${SELECTED_JSON_HEADERS}")
target_link_libraries(_allocator_sim_native PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
set_target_properties(_allocator_sim_native PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/native")
# Osobny program testowy zastępuje operator new tylko we własnym procesie.
# Moduł symulatora i alokator produkcyjny nie zawierają mechanizmu wstrzykiwania błędów.
set(SAFETY_STUB_INCLUDE "${CMAKE_CURRENT_BINARY_DIR}/stub_include")
file(MAKE_DIRECTORY "${SAFETY_STUB_INCLUDE}/prometheus")
file(WRITE "${SAFETY_STUB_INCLUDE}/prometheus/simpleapi.h"
"#pragma once\n"
"#include <atomic>\n"
"#include <cstdint>\n"
"namespace prometheus {\n"
"namespace simpleapi {\n"
"struct counter_metric_t {\n"
"    std::atomic<uint64_t> value{0};\n"
"    void operator++(int) { value.fetch_add(1, std::memory_order_relaxed); }\n"
"    template <typename T>\n"
"    void operator+=(T delta) { value.fetch_add(static_cast<uint64_t>(delta), std::memory_order_relaxed); }\n"
"};\n"
"struct gauge_metric_t {\n"
"    std::atomic<int64_t> value{0};\n"
"    template <typename T>\n"
"    void operator+=(T delta) { value.fetch_add(static_cast<int64_t>(delta), std::memory_order_relaxed); }\n"
"    template <typename T>\n"
"    void operator-=(T delta) { value.fetch_sub(static_cast<int64_t>(delta), std::memory_order_relaxed); }\n"
"    template <typename T>\n"
"    void operator=(T val) { value.store(static_cast<int64_t>(val), std::memory_order_relaxed); }\n"
"};\n"
"} // namespace simpleapi\n"
"} // namespace prometheus\n")
add_executable(slab_allocator_safety_test EXCLUDE_FROM_ALL tests/slab_allocator_safety_test.cpp
    "${SRC}/meta_service/mmc_best_fit_allocator.cpp"
    "${SRC}/meta_service/mmc_slab_allocator.cpp"
    "${SRC}/common/mmc_last_error.cpp")
get_target_property(SIM_INCLUDE_DIRS _allocator_sim_native INCLUDE_DIRECTORIES)
target_include_directories(slab_allocator_safety_test PRIVATE ${SIM_INCLUDE_DIRS})
target_link_libraries(slab_allocator_safety_test PRIVATE Threads::Threads ${CMAKE_DL_LIBS})

target_include_directories(slab_allocator_safety_test PRIVATE "${SAFETY_STUB_INCLUDE}" "${SRC}/proto" "${SRC}/net" "${SRC}/client" "${SRC}/client/metric" "${SRC}/under_api/ubs_io")
