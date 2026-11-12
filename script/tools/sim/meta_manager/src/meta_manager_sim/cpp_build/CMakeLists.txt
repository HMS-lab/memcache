# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.
cmake_minimum_required(VERSION 3.18)
project(meta_manager_sim LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Python3 REQUIRED COMPONENTS Interpreter Development.Module)
execute_process(COMMAND "${Python3_EXECUTABLE}" -m pybind11 --cmakedir
    OUTPUT_VARIABLE PYBIND_DIR OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE PYBIND_RESULT)
if(NOT PYBIND_RESULT EQUAL 0)
    message(FATAL_ERROR "Install pybind11 for ${Python3_EXECUTABLE} before running the simulator.")
endif()
list(APPEND CMAKE_PREFIX_PATH "${PYBIND_DIR}")
set(PYBIND11_FINDPYTHON ON)
find_package(pybind11 CONFIG REQUIRED)
find_package(Threads REQUIRED)
execute_process(COMMAND "${Python3_EXECUTABLE}" -c "import nlohmann_json; print(nlohmann_json.get_include())"
    OUTPUT_VARIABLE JSON_HEADERS OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE JSON_RESULT)
if(NOT JSON_RESULT EQUAL 0)
    message(FATAL_ERROR "Install nlohmann_json for ${Python3_EXECUTABLE} before running the simulator.")
endif()

get_filename_component(REPO "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../.." ABSOLUTE)
set(SRC "${REPO}/src/memcache/csrc")
if(NOT EXISTS "${SRC}/meta_service/mmc_meta_manager.cpp")
    message(FATAL_ERROR "This standalone tool must be built inside the MemCache checkout.")
endif()

# sharded_lru.cpp includes lru.cpp and instantiates the shared container factory.
pybind11_add_module(_meta_manager_native NO_EXTRAS
    bindings.cpp manager_adapter.cpp
    "${SRC}/meta_service/mmc_meta_manager.cpp"
    "${SRC}/meta_service/mmc_meta_container_sharded_lru.cpp"
    "${SRC}/meta_service/mmc_blob_allocator.cpp"
    "${SRC}/meta_service/mmc_meta_backup_mgr_default.cpp"
    "${SRC}/meta_service/mmc_meta_metric_manager.cpp"
    "${SRC}/entities/mmc_mem_blob.cpp"
    "${SRC}/entities/mmc_mem_obj_meta.cpp"
    "${SRC}/entities/mmc_blob_state.cpp"
    "${SRC}/entities/mmc_meta_lease_manager.cpp"
    "${SRC}/common/mmc_last_error.cpp")
target_include_directories(_meta_manager_native PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/standalone" "${JSON_HEADERS}"
    "${REPO}/src/memcache/include" "${SRC}/common" "${SRC}/config"
    "${SRC}/entities" "${SRC}/meta_service" "${SRC}/net" "${SRC}/proto"
    "${SRC}/client/metric" "${SRC}/under_api/mf_smem" "${SRC}/under_api/ubs_io")
target_link_libraries(_meta_manager_native PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
set_target_properties(_meta_manager_native PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/native")
