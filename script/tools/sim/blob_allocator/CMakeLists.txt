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
    "${SRC}/meta_service/mmc_blob_allocator.cpp" "${SRC}/common/mmc_last_error.cpp")
target_include_directories(_allocator_sim_native PRIVATE "${REPO}/src/memcache/include"
    "${SRC}/common" "${SRC}/entities" "${SRC}/meta_service" "${SRC}/under_api/mf_smem" "${SELECTED_JSON_HEADERS}")
target_link_libraries(_allocator_sim_native PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
set_target_properties(_allocator_sim_native PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/native")
