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
#ifndef MEMCACHE_PTRACER_H
#define MEMCACHE_PTRACER_H

#include "smem_ptracer_def.h"

#ifdef __cplusplus
extern "C" {
#endif
/**
 * @brief ptracer (i.e. performance tracer) interface for extension
 */
typedef smem_ptracer_t ptracer;

extern ptracer g_mmc_tracer;

typedef smem_ptracer_config_t ptracer_config_t;

int32_t mmc_ptracer_init(ptracer_config_t *config);
void mmc_ptracer_uninit(void);
const char *mmc_ptracer_get_last_err_msg(void);
const char *mmc_ptracer_get_all_tp_string(void);

#define g_tracer                  g_mmc_tracer
#define ptracer_init              mmc_ptracer_init
#define ptracer_uninit            mmc_ptracer_uninit
#define ptracer_get_last_err_msg  mmc_ptracer_get_last_err_msg
#define ptracer_get_all_tp_string mmc_ptracer_get_all_tp_string

/**
 * @brief Start to trace
 * @param TP_ID            [in] trace id defined with macro PTRACER_ID
 */
#define TP_TRACE_BEGIN(TP_ID)                        \
    uint64_t tpBegin##TP_ID = 0;                     \
    if (g_tracer.enabled) {                          \
        g_tracer.trace_begin(TP_ID, #TP_ID);         \
        tpBegin##TP_ID = g_tracer.current_time_ns(); \
    }

/**
 * @brief End to trace, this should be in the same thread with PTRACER_ID
 * @param TP_ID            [in] trace id defined with macro PTRACER_ID
 * @param GOOD_BAD         [in] good or bad result to be record, used to counting function
 *                              execution result beside the time cost, 0 means good, other
 *                              value means bad, bad execution will be not record into
 *                              performance counting
 */
#define TP_TRACE_END(TP_ID, GOOD_BAD)                                                     \
    if (g_tracer.enabled) {                                                               \
        g_tracer.trace_end(TP_ID, g_tracer.current_time_ns() - tpBegin##TP_ID, GOOD_BAD); \
    }

#define TP_TRACE_TRACE_BEGIN(TP_ID, P_U64_TIME_NS)       \
    if (g_tracer.enabled) {                              \
        g_tracer.trace_begin(TP_ID, #TP_ID);             \
        (*(P_U64_TIME_NS)) = g_tracer.current_time_ns(); \
    }

#define TP_TRACE_TRACE_END(TP_ID, U64_TIME_NS, GOOD_BAD)                                   \
    if (g_tracer.enabled) {                                                                \
        g_tracer.trace_end(TP_ID, (g_tracer.current_time_ns() - (U64_TIME_NS)), GOOD_BAD); \
    }

#define TP_TRACE_RECORD(TP_ID, U64_DIFF_TIME_NS, GOOD_BAD)       \
    if (g_tracer.enabled) {                                      \
        g_tracer.trace_begin(TP_ID, #TP_ID);                     \
        g_tracer.trace_end(TP_ID, (U64_DIFF_TIME_NS), GOOD_BAD); \
    }

#define TP_CURRENT_TIME_NS (g_tracer.current_time_ns ? g_tracer.current_time_ns() : 0)

#define PTRACER_ID(MODULE_ID_, traceId_) ((MODULE_ID_) << 16 | ((traceId_) & 0xFFFF))

#ifdef __cplusplus
}
#endif
#endif // MEMCACHE_PTRACER_H
