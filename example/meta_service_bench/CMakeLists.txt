if (("${BUILD_CPP_EXAMPLE}" STREQUAL "") OR ("${BUILD_CPP_EXAMPLE}" STREQUAL "OFF"))
    message(STATUS "BUILD_CPP_EXAMPLE is OFF (bench_meta_service standalone mode).")
    add_compile_definitions(_GLIBCXX_USE_CXX11_ABI=0)
    cmake_minimum_required(VERSION 3.12.0)
    project(bench_meta_service VERSION 1.0 LANGUAGES CXX C)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_SKIP_RPATH TRUE)

    set(MF_PATH $ENV{MEMFABRIC_HYBRID_HOME_PATH})
    if ("${MF_PATH}" STREQUAL "")
        message(FATAL_ERROR "MEMFABRIC_HYBRID_HOME_PATH not set. Run: source /usr/local/memfabric_hybrid/set_env.sh")
    endif ()
    set(MCC_PATH $ENV{MEMCACHE_HYBRID_HOME_PATH})
    if ("${MCC_PATH}" STREQUAL "")
        message(FATAL_ERROR "MEMCACHE_HYBRID_HOME_PATH not set. Point to memcache source root.")
    endif ()

    file(GLOB_RECURSE ALL_HEADERS
            "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
            "${MF_PATH}/*.h"
            "${MCC_PATH}/*.h"
    )
    set(INCLUDE_DIRS "")
    foreach (header ${ALL_HEADERS})
        get_filename_component(HEADER_DIR ${header} PATH)
        list(APPEND INCLUDE_DIRS ${HEADER_DIR})
    endforeach ()
    list(REMOVE_DUPLICATES INCLUDE_DIRS)
    list(FILTER INCLUDE_DIRS EXCLUDE REGEX ".*/mockcpp/.*")
    list(FILTER INCLUDE_DIRS EXCLUDE REGEX ".*/build/3rdparty/.*")
    include_directories(${INCLUDE_DIRS})

    include_directories(
            ${MCC_PATH}/3rdparty/nlohmann/json/single_include
            ${MCC_PATH}/3rdparty/prometheus-cpp-lite/simpleapi/include
            ${MCC_PATH}/3rdparty/prometheus-cpp-lite/core/include
            ${MCC_PATH}/3rdparty/log/spdlog/include
            ${MCC_PATH}/build/3rdparty/spdlog/include)

    file(GLOB_RECURSE ALL_SO
            "${MF_PATH}/*.so"
            "${MCC_PATH}/*.so"
            "/usr/local/memcache_hybrid/*.so"
            "/usr/local/memcache_hybrid/*/*.so"
    )
    add_executable(bench_meta_service bench_meta_service.cpp)
    foreach (SO ${ALL_SO})
        get_filename_component(FILENAME ${SO} NAME)
        if ("${FILENAME}" MATCHES "^libmf_memcache")
            get_filename_component(FILEDIR ${SO} DIRECTORY)
            target_link_directories(bench_meta_service PUBLIC ${FILEDIR})
            target_link_libraries(bench_meta_service PUBLIC ${FILENAME})
            break()
        endif ()
    endforeach ()
    find_package(Python3 REQUIRED COMPONENTS Development)
    target_link_libraries(bench_meta_service PUBLIC Python3::Python pthread dl)
    target_compile_options(bench_meta_service PRIVATE -W -fpermissive -Wno-float-equal)
else ()
    message(STATUS "BUILD_CPP_EXAMPLE is ON (bench_meta_service in-tree mode).")

    add_executable(bench_meta_service bench_meta_service.cpp)

    target_include_directories(bench_meta_service PRIVATE
            ${PROJECT_MMC_SRC_BASE}/include/
            ${PROJECT_MMC_SRC_BASE}/include/cpp/
            ${PROJECT_MMC_SRC_BASE}/csrc/
            ${PROJECT_MMC_SRC_BASE}/csrc/common
            ${PROJECT_MMC_SRC_BASE}/csrc/client
            ${PROJECT_MMC_SRC_BASE}/csrc/client/metric
            ${PROJECT_MMC_SRC_BASE}/csrc/config
            ${PROJECT_MMC_SRC_BASE}/csrc/local_service
            ${PROJECT_MMC_SRC_BASE}/csrc/entities
            ${PROJECT_MMC_SRC_BASE}/csrc/ha
            ${PROJECT_MMC_SRC_BASE}/csrc/meta_service
            ${PROJECT_MMC_SRC_BASE}/csrc/kv_event
            ${PROJECT_MMC_SRC_BASE}/csrc/log
            ${PROJECT_MMC_SRC_BASE}/csrc/net
            ${PROJECT_MMC_SRC_BASE}/csrc/net/acc_links_impl
            ${PROJECT_MMC_SRC_BASE}/csrc/proto
            ${PROJECT_MMC_SRC_BASE}/csrc/under_api/mf_smem
            ${PROJECT_MMC_SRC_BASE}/csrc/under_api/ubs_io
            ${PROJECT_SMEM_SRC_BASE}/include/host
            ${PROJECT_SMEM_SRC_BASE}/csrc/common
            ${PROJECT_SMEM_SRC_BASE}/csrc/config_store
            ${PROJECT_SMEM_SRC_BASE}/csrc/net
            ${PROJECT_SMEM_SRC_BASE}/csrc/under_api/hybm_core
            ${PROJECT_UTIL_SRC_BASE}/ptracer/include
            ${PROJECT_ACCLINKS_SRC_BASE}/include
            ${PROJECT_3RDPARTY_BIN_DIR}/spdlog/include/
            ${PROJECT_3RDPARTY_SRC_DIR}/log/spdlog/include
            ${PROJECT_3RDPARTY_SRC_DIR}/nlohmann/json/single_include/
            ${PROJECT_3RDPARTY_SRC_DIR}/prometheus-cpp-lite/simpleapi/include
            ${PROJECT_3RDPARTY_SRC_DIR}/prometheus-cpp-lite/core/include
    )

    target_link_libraries(bench_meta_service PUBLIC mmc_static pthread dl)
    target_link_directories(bench_meta_service PUBLIC
            ${PROJECT_OUTPUT_PATH}/acc_links/lib64/
            ${PROJECT_3RDPARTY_BIN_DIR}/spdlog/lib
            ${PROJECT_3RDPARTY_BIN_DIR}/spdlog/lib64)
    target_compile_options(bench_meta_service PRIVATE -W -fpermissive -Wno-float-equal)

    install(TARGETS bench_meta_service
            RUNTIME DESTINATION ${TARGET_INSTALL_DIR}/memcache/bin
            PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
endif ()
