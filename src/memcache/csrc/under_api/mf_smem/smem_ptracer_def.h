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
#ifndef MEMCACHE_SMEM_PTRACER_DEF_H
#define MEMCACHE_SMEM_PTRACER_DEF_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool enabled;
    void (*trace_begin)(uint32_t traceId, const char *traceName);
    void (*trace_end)(uint32_t traceId, const uint64_t diff, int32_t goodOrBad);
    uint64_t (*current_time_ns)();
} smem_ptracer_t;

typedef struct {
    int32_t tracerType;
    const char *dumpFilePath;
} smem_ptracer_config_t;

#ifdef __cplusplus
}
#endif

#endif // MEMCACHE_SMEM_PTRACER_DEF_H
