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
#ifndef MEM_FABRIC_MMC_META_CONFIG_RELOAD_H
#define MEM_FABRIC_MMC_META_CONFIG_RELOAD_H

#include <ctime>
#include <string>

#include "mmc_meta_service.h"

namespace ock {
namespace mmc {

// Reload the hot-reloadable meta settings (log_level, lease_ttl_ms, evict_threshold_high/low)
// from the meta config file and apply them to the running MmcMetaService. The file is parsed at
// most once per poll. Returns true when at least one setting was actually applied.
bool ReloadMetaConfigFromFile(const std::string &configFilePath, time_t &lastConfigMtime, MmcMetaService *metaService);

} // namespace mmc
} // namespace ock

#endif // MEM_FABRIC_MMC_META_CONFIG_RELOAD_H
