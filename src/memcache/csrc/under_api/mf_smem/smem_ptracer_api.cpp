/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemCache_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of the License at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */
#include "smem_ptracer_api.h"

#include <dlfcn.h>

#include "mmc_functions.h"

namespace ock {
namespace mmc {

std::mutex MFPtracerApi::gMutex;
void *MFPtracerApi::gPtracerHandle = nullptr;
const char *MFPtracerApi::gPtracerLibName = "libmf_hybm_core.so";
smem_ptracer_t *MFPtracerApi::gMfTracer = nullptr;
ptracerInitFunc MFPtracerApi::gPtracerInit = nullptr;
ptracerUninitFunc MFPtracerApi::gPtracerUninit = nullptr;
ptracerGetLastErrorFunc MFPtracerApi::gPtracerGetLastError = nullptr;
ptracerGetAllTracepointsFunc MFPtracerApi::gPtracerGetAllTracepoints = nullptr;
std::string MFPtracerApi::gLastError;

Result MFPtracerApi::LoadAllSymbols(void *handle)
{
    DL_LOAD_SYM(gMfTracer, smem_ptracer_t *, handle, "g_tracer");
    DL_LOAD_SYM(gPtracerInit, ptracerInitFunc, handle, "ptracer_init");
    DL_LOAD_SYM(gPtracerUninit, ptracerUninitFunc, handle, "ptracer_uninit");
    DL_LOAD_SYM(gPtracerGetLastError, ptracerGetLastErrorFunc, handle, "ptracer_get_last_err_msg");
    DL_LOAD_SYM(gPtracerGetAllTracepoints, ptracerGetAllTracepointsFunc, handle, "ptracer_get_all_tp_string");
    return MMC_OK;
}

Result MFPtracerApi::LoadLibrary()
{
    void *handle = dlopen(gPtracerLibName, RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        const char *error = dlerror();
        gLastError = error == nullptr ? "failed to load ptracer library" : error;
        MMC_LOG_ERROR("Failed to load " << gPtracerLibName << ", error: " << gLastError);
        return MMC_ERROR;
    }

    if (LoadAllSymbols(handle) != MMC_OK) {
        gLastError = "failed to load required ptracer symbols from libmf_hybm_core.so";
        ClearAllSymbols();
        return MMC_ERROR;
    }

    gPtracerHandle = handle;
    return MMC_OK;
}

bool MFPtracerApi::IsTracerValid()
{
    return !gMfTracer->enabled || (gMfTracer->trace_begin != nullptr && gMfTracer->trace_end != nullptr &&
                                   gMfTracer->current_time_ns != nullptr);
}

Result MFPtracerApi::Init(smem_ptracer_config_t *config, smem_ptracer_t &tracer)
{
    std::lock_guard<std::mutex> guard(gMutex);
    if (gPtracerHandle != nullptr) {
        tracer = *gMfTracer;
        return MMC_OK;
    }

    MMC_RETURN_ERROR(LoadLibrary(), "Failed to load ptracer library");
    const auto result = gPtracerInit(config);
    if (result != MMC_OK) {
        const char *error = gPtracerGetLastError();
        gLastError = error == nullptr ? "failed to initialize ptracer" : error;
        MMC_LOG_ERROR("Failed to initialize ptracer, error: " << gLastError);
        ClearAllSymbols();
        dlclose(gPtracerHandle);
        gPtracerHandle = nullptr;
        return result;
    }

    if (!IsTracerValid()) {
        gLastError = "invalid ptracer function table from libmf_hybm_core.so";
        MMC_LOG_ERROR(gLastError);
        gPtracerUninit();
        ClearAllSymbols();
        dlclose(gPtracerHandle);
        gPtracerHandle = nullptr;
        return MMC_ERROR;
    }

    tracer = *gMfTracer;
    gLastError.clear();
    return MMC_OK;
}

void MFPtracerApi::Uninit()
{
    std::lock_guard<std::mutex> guard(gMutex);
    if (gPtracerHandle == nullptr) {
        return;
    }

    gPtracerUninit();
    ClearAllSymbols();
    dlclose(gPtracerHandle);
    gPtracerHandle = nullptr;
}

const char *MFPtracerApi::GetLastError()
{
    return gPtracerGetLastError == nullptr ? gLastError.c_str() : gPtracerGetLastError();
}

const char *MFPtracerApi::GetAllTracepoints()
{
    return gPtracerGetAllTracepoints == nullptr ? "" : gPtracerGetAllTracepoints();
}

void MFPtracerApi::ClearAllSymbols()
{
    gMfTracer = nullptr;
    gPtracerInit = nullptr;
    gPtracerUninit = nullptr;
    gPtracerGetLastError = nullptr;
    gPtracerGetAllTracepoints = nullptr;
}

} // namespace mmc
} // namespace ock
