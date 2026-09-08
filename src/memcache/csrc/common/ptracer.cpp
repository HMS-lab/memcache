/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * MemCache_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */
#include "ptracer.h"

#include "mmc_types.h"
#include "smem_ptracer_api.h"

#define PTRACER_API __attribute__((visibility("default")))

ptracer g_tracer = {};

PTRACER_API int32_t ptracer_init(ptracer_config_t *config)
{
#ifdef ENABLE_PTRACER
    return ock::mmc::MFPtracerApi::Init(config, g_tracer);
#else
    (void)config;
    g_tracer.enabled = false;
    return ock::mmc::MMC_OK;
#endif
}

PTRACER_API void ptracer_uninit(void)
{
#ifdef ENABLE_PTRACER
    g_tracer = {};
    ock::mmc::MFPtracerApi::Uninit();
#endif
}

PTRACER_API const char *ptracer_get_last_err_msg(void)
{
#ifdef ENABLE_PTRACER
    return ock::mmc::MFPtracerApi::GetLastError();
#else
    return "";
#endif
}

PTRACER_API const char *ptracer_get_all_tp_string(void)
{
#ifdef ENABLE_PTRACER
    return ock::mmc::MFPtracerApi::GetAllTracepoints();
#else
    return "";
#endif
}
