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
#ifndef MEMCACHE_SMEM_LOGGER_API_H
#define MEMCACHE_SMEM_LOGGER_API_H

#include <mutex>

#include "mmc_common_includes.h"

namespace ock {
namespace mmc {

using smemSetExternLoggerFunc = int32_t (*)(void (*)(int, const char *));
using smemSetLogLevelFunc = int32_t (*)(int);

class MFLoggerApi {
public:
    static Result LoadLibrary();

    static int32_t SetExternLogger(void (*func)(int, const char *))
    {
        return gSmemSetExternLogger(func);
    }

    static int32_t SetLogLevel(int level)
    {
        return gSmemSetLogLevel(level);
    }

private:
    static Result LoadAllSymbols();

private:
    static std::mutex gMutex;
    static bool gLoaded;
    static void *gSmemHandle;
    static const char *gSmemLibName;
    static smemSetExternLoggerFunc gSmemSetExternLogger;
    static smemSetLogLevelFunc gSmemSetLogLevel;
};

} // namespace mmc
} // namespace ock

#endif // MEMCACHE_SMEM_LOGGER_API_H
