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
#ifndef MEMCACHE_SMEM_PTRACER_API_H
#define MEMCACHE_SMEM_PTRACER_API_H

#include <mutex>
#include <string>

#include "mmc_types.h"
#include "smem_ptracer_def.h"

namespace ock {
namespace mmc {

using ptracerInitFunc = int32_t (*)(smem_ptracer_config_t *);
using ptracerUninitFunc = void (*)();
using ptracerGetLastErrorFunc = const char *(*)();
using ptracerGetAllTracepointsFunc = const char *(*)();

class MFPtracerApi {
public:
    static Result Init(smem_ptracer_config_t *config, smem_ptracer_t &tracer);
    static void Uninit();
    static const char *GetLastError();
    static const char *GetAllTracepoints();

private:
    static Result LoadLibrary();
    static Result LoadAllSymbols(void *handle);
    static bool IsTracerValid();
    static void ClearAllSymbols();

private:
    static std::mutex gMutex;
    static void *gPtracerHandle;
    static const char *gPtracerLibName;
    static smem_ptracer_t *gMfTracer;
    static ptracerInitFunc gPtracerInit;
    static ptracerUninitFunc gPtracerUninit;
    static ptracerGetLastErrorFunc gPtracerGetLastError;
    static ptracerGetAllTracepointsFunc gPtracerGetAllTracepoints;
    static std::string gLastError;
};

} // namespace mmc
} // namespace ock

#endif // MEMCACHE_SMEM_PTRACER_API_H
