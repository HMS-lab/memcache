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
#ifndef MEM_FABRIC_MMC_LOCAL_CONFIG_RELOAD_H
#define MEM_FABRIC_MMC_LOCAL_CONFIG_RELOAD_H

#include <ctime>
#include <string>

namespace ock {
namespace mmc {

// Reload the hot-reloadable settings (log_level and client tunables: retry_milliseconds,
// aggregate.io/num, batch_option.chunk.size/count) from the local config file and apply them
// to the shared MmcOutLogger and MmcClientDefault singletons. The file is parsed at most once
// per poll. Returns true when at least one setting was actually applied.
bool ReloadLocalConfigFromFile(const std::string &configFilePath, time_t &lastConfigMtime);

} // namespace mmc
} // namespace ock

#endif // MEM_FABRIC_MMC_LOCAL_CONFIG_RELOAD_H
