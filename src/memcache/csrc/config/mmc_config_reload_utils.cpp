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
#include "mmc_config_reload_utils.h"

#include <map>
#include <set>
#include <utility>

#include "mmc_config_const.h"
#include "mmc_functions.h"
#include "mmc_logger.h"

namespace ock {
namespace mmc {

FieldStatus GetValidatedInt(const std::string &key, KVParser &kvParser, const ValidatorPtr &validator, int32_t &out)
{
    std::string value;
    if (kvParser.GetItem(key, value) != MMC_OK) {
        return FieldStatus::NOT_FOUND;
    }
    long parsedValue = 0;
    if (!OckStol(value, parsedValue) || parsedValue > INT32_MAX || parsedValue < INT32_MIN) {
        MMC_LOG_ERROR("Invalid value for <" << key << ">: " << value);
        return FieldStatus::INVALID;
    }
    out = static_cast<int32_t>(parsedValue);
    if (!validator->Validate(out)) {
        MMC_LOG_ERROR("Invalid value for <" << key << ">: " << value << ", " << validator->ErrorMessage());
        return FieldStatus::INVALID;
    }
    return FieldStatus::OK;
}

namespace {

// Fixed baseline for hot-reload change checks. Written once by
// SetNonHotReloadableBaseline during initialization and only read afterwards,
// so the report behavior does not depend on call history.
std::map<std::string, std::string> gBaselineItems;

struct DynamicConfigKeyEntry {
    const char *key;
};

// Whitelist of config items that can be modified dynamically without a restart.
// The array order MUST match the enumerator order of DynamicConfigItem.
constexpr DynamicConfigKeyEntry K_DYNAMIC_CONFIG_KEYS[] = {
    {ConfConstant::OCK_MMC_LOG_LEVEL.first},
    {ConfConstant::OKC_MMC_CLIENT_RETRY_MILLISECONDS.first},
    {ConfConstant::OCK_MMC_CLIENT_AGGREGATE_IO.first},
    {ConfConstant::OCK_MMC_CLIENT_AGGREGATE_NUM.first},
    {ConfConstant::OCK_MMC_CLIENT_BATCH_CHUNK_SIZE.first},
    {ConfConstant::OCK_MMC_CLIENT_BATCH_CHUNK_COUNT.first},
    {ConfConstant::OCK_MMC_META_SERVICE_URL.first},
    {ConfConstant::OKC_MMC_LOCAL_SERVICE_BM_IP_PORT.first},
    {ConfConstant::OCK_MMC_META_LEASE_TTL_MS.first},
    {ConfConstant::OKC_MMC_EVICT_THRESHOLD_HIGH.first},
    {ConfConstant::OKC_MMC_EVICT_THRESHOLD_LOW.first},
};

} // namespace

const char *GetDynamicConfigKey(DynamicConfigItem item)
{
    const uint32_t index = static_cast<uint32_t>(item);
    if (index >= sizeof(K_DYNAMIC_CONFIG_KEYS) / sizeof(K_DYNAMIC_CONFIG_KEYS[0])) {
        return "";
    }
    return K_DYNAMIC_CONFIG_KEYS[index].key;
}

bool IsDynamicConfigKey(const std::string &key)
{
    for (const auto &entry : K_DYNAMIC_CONFIG_KEYS) {
        if (key == entry.key) {
            return true;
        }
    }
    return false;
}

void SetNonHotReloadableBaseline(const std::map<std::string, std::string> &initialItems)
{
    gBaselineItems.clear();
    for (const auto &item : initialItems) {
        if (IsDynamicConfigKey(item.first)) {
            continue;
        }
        gBaselineItems[item.first] = item.second;
    }
}

void ReportNonHotReloadableKeys(KVParser &kvParser)
{
    std::set<std::string> currentKeys;
    const uint32_t size = kvParser.Size();
    for (uint32_t i = 0; i < size; ++i) {
        std::string key;
        std::string value;
        kvParser.GetI(i, key, value);
        currentKeys.insert(key);
        if (IsDynamicConfigKey(key)) {
            continue;
        }
        const auto iter = gBaselineItems.find(key);
        if (iter != gBaselineItems.end() && iter->second == value) {
            continue;
        }
        MMC_LOG_ERROR("Config item <" << key << "> cannot be hot-reloaded, keeping the current value");
    }
    // A non-whitelisted key removed from the file is a modification: report it.
    for (const auto &item : gBaselineItems) {
        if (currentKeys.count(item.first) != 0) {
            continue;
        }
        MMC_LOG_ERROR("Config item <" << item.first << "> cannot be hot-reloaded, keeping the current value");
    }
}

} // namespace mmc
} // namespace ock
