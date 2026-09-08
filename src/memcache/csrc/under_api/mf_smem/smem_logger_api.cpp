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

#include "mmc_functions.h"
#include "smem_version_api.h"
#include "smem_logger_api.h"

namespace ock {
namespace mmc {

std::mutex MFLoggerApi::gMutex;
bool MFLoggerApi::gLoaded = false;
void *MFLoggerApi::gSmemHandle = nullptr;
const char *MFLoggerApi::gSmemLibName = "libmf_smem.so";
smemSetExternLoggerFunc MFLoggerApi::gSmemSetExternLogger = nullptr;
smemSetLogLevelFunc MFLoggerApi::gSmemSetLogLevel = nullptr;

Result MFLoggerApi::LoadAllSymbols()
{
    DL_LOAD_SYM(gSmemSetExternLogger, smemSetExternLoggerFunc, gSmemHandle, "smem_set_extern_logger");
    DL_LOAD_SYM(gSmemSetLogLevel, smemSetLogLevelFunc, gSmemHandle, "smem_set_log_level");
    return MMC_OK;
}

Result MFLoggerApi::LoadLibrary()
{
    std::lock_guard<std::mutex> guard(gMutex);
    if (gLoaded) {
        return MMC_OK;
    }

    gSmemHandle = dlopen(gSmemLibName, RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND);
    if (gSmemHandle == nullptr) {
        MMC_LOG_ERROR("Failed to load MemFabric logger library [" << gSmemLibName << "], error: " << dlerror());
        return MMC_NOT_INITIALIZED;
    }

    if (CheckMemfabricAbiVersion(gSmemHandle) != MMC_OK) {
        dlclose(gSmemHandle);
        gSmemHandle = nullptr;
        return MMC_NOT_INITIALIZED;
    }

    if (LoadAllSymbols() != MMC_OK) {
        // DL_LOAD_SYM closes the handle when a required symbol is missing.
        gSmemHandle = nullptr;
        gSmemSetExternLogger = nullptr;
        gSmemSetLogLevel = nullptr;
        return MMC_NOT_INITIALIZED;
    }

    gLoaded = true;
    return MMC_OK;
}

} // namespace mmc
} // namespace ock
