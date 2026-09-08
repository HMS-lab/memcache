# Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
# MemCache_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.

aux_source_directory(. DAEMON_SRCS)

set(MMC_META_SERVICE_BUILDABLE ON)
if (BUILD_PYTHON STREQUAL "ON")
    # pybind11 may return a bare library name even when the embedding library is absent.
    if (NOT EXISTS "${PYTHON_LIBRARY}")
        set(MMC_META_SERVICE_BUILDABLE OFF)
        message(WARNING "skip building mmc_meta_service: Python embedding library '${PYTHON_LIBRARY}' does not exist; "
                        "install the matching Python development library to build the executable")
    endif ()
endif ()

if (MMC_META_SERVICE_BUILDABLE)
    add_executable(mmc_meta_service ${DAEMON_SRCS})

    target_compile_options(mmc_meta_service PRIVATE -fPIC -Wno-float-equal)

    target_link_options(mmc_meta_service PRIVATE -pie)

    target_link_libraries(mmc_meta_service PUBLIC mmc_shared pthread dl)

    if (BUILD_PYTHON STREQUAL "ON")
        target_link_libraries(mmc_meta_service PRIVATE pybind11::embed)
    endif ()

    install(TARGETS mmc_meta_service
            RUNTIME DESTINATION ${TARGET_INSTALL_DIR}/memcache/bin
            PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
endif ()
