# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Mulan PSL v2. See the repository LICENSE file.

cmake_minimum_required(VERSION 3.18)
project(meta_manager_sim LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(Python3 COMPONENTS Interpreter)
find_package(Threads)

execute_process(
    COMMAND "${Python3_EXECUTABLE}" -c
        "import nlohmann_json; print(nlohmann_json.get_include())"
    OUTPUT_VARIABLE JSON_HEADERS
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

get_filename_component(
    REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../.." ABSOLUTE
)
set(MEMCACHE_SRC "${REPO_ROOT}/src/memcache/csrc")

# Note: sharded_lru.cpp includes lru.cpp and instantiates the container factory
add_executable(meta_manager_sim_native
    src/main.cpp
    src/manager_adapter.cpp
    "${MEMCACHE_SRC}/meta_service/mmc_meta_manager.cpp"
    "${MEMCACHE_SRC}/meta_service/mmc_meta_container_sharded_lru.cpp"
    "${MEMCACHE_SRC}/meta_service/mmc_blob_allocator.cpp"
    "${MEMCACHE_SRC}/meta_service/mmc_meta_backup_mgr_default.cpp"
    "${MEMCACHE_SRC}/meta_service/mmc_meta_metric_manager.cpp"
    "${MEMCACHE_SRC}/entities/mmc_mem_blob.cpp"
    "${MEMCACHE_SRC}/entities/mmc_mem_obj_meta.cpp"
    "${MEMCACHE_SRC}/entities/mmc_blob_state.cpp"
    "${MEMCACHE_SRC}/entities/mmc_meta_lease_manager.cpp"
    "${MEMCACHE_SRC}/common/mmc_last_error.cpp"
)

target_include_directories(meta_manager_sim_native PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/standalone"
    "${JSON_HEADERS}"
    "${REPO_ROOT}/src/memcache/include"
    "${MEMCACHE_SRC}/common"
    "${MEMCACHE_SRC}/config"
    "${MEMCACHE_SRC}/entities"
    "${MEMCACHE_SRC}/meta_service"
    "${MEMCACHE_SRC}/net"
    "${MEMCACHE_SRC}/proto"
    "${MEMCACHE_SRC}/client/metric"
    "${MEMCACHE_SRC}/under_api/mf_smem"
    "${MEMCACHE_SRC}/under_api/ubs_io"
)

target_link_libraries(meta_manager_sim_native PRIVATE
    OpenSSL::Crypto
    Threads::Threads
    ${CMAKE_DL_LIBS}
)
target_compile_options(meta_manager_sim_native PRIVATE -g)

set_target_properties(meta_manager_sim_native PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY
    "${CMAKE_BINARY_DIR}/bin"
)
