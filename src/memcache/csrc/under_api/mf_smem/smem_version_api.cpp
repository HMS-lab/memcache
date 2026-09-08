/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemCache_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
*/
#include <dlfcn.h>

#include "mmc_logger.h"
#include "smem_version_api.h"

namespace ock {
namespace mmc {
namespace {
constexpr uint32_t VERSION_PART_MASK = 0xFFFFU;
constexpr uint32_t VERSION_MAJOR_SHIFT = 16U;
constexpr uint32_t EXPECTED_ABI_VERSION =
    (static_cast<uint32_t>(VERSION_MAJOR) << VERSION_MAJOR_SHIFT) | static_cast<uint32_t>(VERSION_MINOR);
} // namespace

Result CheckMemfabricAbiVersion(void *handle)
{
    dlerror();
    auto getAbiVersion = reinterpret_cast<smemGetAbiVersionFunc>(dlsym(handle, "smem_get_abi_version"));
    const char *error = dlerror();
    if (error != nullptr || getAbiVersion == nullptr) {
        MMC_LOG_ERROR("The loaded MemFabric does not provide smem_get_abi_version. Install MemFabric "
                      << VERSION_MAJOR << "." << VERSION_MINOR
                      << ".x matching the MemCache version before starting the service. dlsym error: "
                      << (error == nullptr ? "unknown" : error));
        return MMC_NOT_INITIALIZED;
    }

    const uint32_t actualVersion = getAbiVersion();
    if (actualVersion == EXPECTED_ABI_VERSION) {
        return MMC_OK;
    }

    const uint32_t actualMajor = actualVersion >> VERSION_MAJOR_SHIFT;
    const uint32_t actualMinor = actualVersion & VERSION_PART_MASK;
    MMC_LOG_ERROR("Incompatible MemFabric ABI: MemCache requires "
                  << VERSION_MAJOR << "." << VERSION_MINOR << ".x, but the loaded MemFabric is " << actualMajor << "."
                  << actualMinor << ".x. Install matching MemCache and MemFabric major.minor versions.");
    return MMC_NOT_INITIALIZED;
}

} // namespace mmc
} // namespace ock
