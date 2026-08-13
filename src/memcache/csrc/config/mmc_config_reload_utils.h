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
#ifndef MMC_CONFIG_RELOAD_UTILS_H
#define MMC_CONFIG_RELOAD_UTILS_H

#include <cstdint>
#include <map>
#include <string>

#include "mmc_config_validator.h"
#include "mmc_kv_parser.h"

namespace ock {
namespace mmc {

enum class FieldStatus {
    NOT_FOUND,
    OK,
    INVALID,
};

FieldStatus GetValidatedInt(const std::string &key, KVParser &kvParser, const ValidatorPtr &validator, int32_t &out);

// Whitelist of config items that can be modified dynamically without a restart.
// Any other item present in a reloaded config file is reported as non-hot-reloadable
// and its modification is ignored, keeping the currently applied value.
// Shared items (log_level) are reloaded by both MetaService and LocalService; the
// remaining items are service-specific (client/local-service items vs meta items).
enum class DynamicConfigItem : uint32_t {
    LOG_LEVEL,
    // LocalService/LocalService-only items
    CLIENT_RETRY_MILLISECONDS,
    CLIENT_AGGREGATE_IO,
    CLIENT_AGGREGATE_NUM,
    CLIENT_BATCH_CHUNK_SIZE,
    CLIENT_BATCH_CHUNK_COUNT,
    META_SERVICE_URL,
    CONFIG_STORE_URL,
    // MetaService-only items
    META_LEASE_TTL_MS,
    EVICT_THRESHOLD_HIGH,
    EVICT_THRESHOLD_LOW,
};

const char *GetDynamicConfigKey(DynamicConfigItem item);
bool IsDynamicConfigKey(const std::string &key);

// Record the fixed baseline for hot-reload change checks: the config items
// parsed and validated during initialization. Must be called once at startup.
void SetNonHotReloadableBaseline(const std::map<std::string, std::string> &initialItems);

// Scan the freshly parsed config file and print an error log for every item that
// is NOT in the hot-reload whitelist and whose value differs from the baseline,
// or that was present in the baseline but removed from the file. The
// modification is ignored and the current value is kept.
void ReportNonHotReloadableKeys(KVParser &kvParser);

} // namespace mmc
} // namespace ock

#endif // MMC_CONFIG_RELOAD_UTILS_H
