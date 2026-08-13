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
#include "mmc_meta_config_reload.h"

#include "mmc_config_reload_utils.h"
#include "mmc_configuration.h"
#include "mmc_functions.h"
#include "mmc_kv_parser.h"
#include "mmc_logger.h"
#include "mmc_meta_service.h"
#include "mf_out_logger.h"
#include "spdlogger4c.h"

namespace ock {
namespace mmc {

static bool ApplyMetaLogLevelFromConfig(int32_t newLogLevel)
{
    if (newLogLevel == MmcOutLogger::Instance().GetLogLevel()) {
        return false;
    }
    const LogLevel oldLogLevel = MmcOutLogger::Instance().GetLogLevel();
    if (MmcOutLogger::Instance().SetLogLevel(static_cast<LogLevel>(newLogLevel)) != 0) {
        MMC_LOG_ERROR("Invalid log level " << newLogLevel);
        return false;
    }
    mf::OutLogger::Instance().SetLogLevel(static_cast<mf::LogLevel>(newLogLevel));
    if (SPDLOG_ResetLogLevel(newLogLevel) != 0) {
        MMC_LOG_ERROR("Failed to reset spdlog log level to " << newLogLevel << ", roll back all meta loggers");
        MmcOutLogger::Instance().SetLogLevel(oldLogLevel);
        mf::OutLogger::Instance().SetLogLevel(static_cast<mf::LogLevel>(oldLogLevel));
        return false;
    }
    MMC_LOG_INFO("meta log_level hot-reloaded to " << newLogLevel);
    return true;
}

// evict thresholds form one group: either field is applied only when the merged
// pair stays consistent (high > low); otherwise the whole pair keeps its values.
static void ApplyEvictThresholds(KVParser &kvParser, mmc_meta_service_config_t &newMetaConfig)
{
    int32_t evictHigh = 0;
    int32_t evictLow = 0;
    auto highValidator =
        VIntRange::Create(ConfConstant::OKC_MMC_EVICT_THRESHOLD_HIGH.first, MIN_EVICT_THRESHOLD, MAX_EVICT_THRESHOLD);
    auto lowValidator = VIntRange::Create(ConfConstant::OKC_MMC_EVICT_THRESHOLD_LOW.first, MIN_EVICT_THRESHOLD,
                                          MAX_EVICT_THRESHOLD - 1);
    const auto highStatus =
        GetValidatedInt(ConfConstant::OKC_MMC_EVICT_THRESHOLD_HIGH.first, kvParser, highValidator, evictHigh);
    const auto lowStatus =
        GetValidatedInt(ConfConstant::OKC_MMC_EVICT_THRESHOLD_LOW.first, kvParser, lowValidator, evictLow);
    if (highStatus == FieldStatus::INVALID || lowStatus == FieldStatus::INVALID) {
        return;
    }
    if (highStatus == FieldStatus::NOT_FOUND && lowStatus == FieldStatus::NOT_FOUND) {
        return;
    }
    const int32_t mergedHigh =
        highStatus == FieldStatus::OK ? evictHigh : static_cast<int32_t>(newMetaConfig.evictThresholdHigh);
    const int32_t mergedLow =
        lowStatus == FieldStatus::OK ? evictLow : static_cast<int32_t>(newMetaConfig.evictThresholdLow);
    if (mergedHigh > mergedLow) {
        newMetaConfig.evictThresholdHigh = static_cast<uint16_t>(mergedHigh);
        newMetaConfig.evictThresholdLow = static_cast<uint16_t>(mergedLow);
    } else {
        MMC_LOG_ERROR("Invalid new evict thresholds, high must be greater than low, but got high="
                      << mergedHigh << " low=" << mergedLow);
    }
}

bool ReloadMetaConfigFromFile(const std::string &configFilePath, time_t &lastConfigMtime, MmcMetaService *metaService)
{
    if (metaService == nullptr) {
        MMC_LOG_WARN("Meta service not initialized, skip meta config hot-reload");
        return false;
    }
    if (!IsConfigFileModified(configFilePath, lastConfigMtime)) {
        return false;
    }

    KVParser kvParser;
    if (kvParser.FromFile(configFilePath) != MMC_OK) {
        MMC_LOG_WARN("Failed to parse meta config file: " << configFilePath);
        return false;
    }

    ReportNonHotReloadableKeys(kvParser);

    const mmc_meta_service_config_t &current = metaService->Options();
    mmc_meta_service_config_t newMetaConfig = current;

    bool logLevelSet = false;
    std::string logLevelStr;
    if (kvParser.GetItem(ConfConstant::OCK_MMC_LOG_LEVEL.first, logLevelStr) == MMC_OK) {
        auto validator = VStrEnum::Create(ConfConstant::OCK_MMC_LOG_LEVEL.first, LOG_LEVEL_ENUM_STR);
        if (validator == nullptr || !validator->Validate(logLevelStr)) {
            MMC_LOG_WARN("Invalid log level \"" << logLevelStr << "\"");
        } else {
            StringToUpper(logLevelStr);
            newMetaConfig.logLevel = MmcOutLogger::Instance().GetLogLevel(logLevelStr);
            logLevelSet = true;
        }
    }

    int32_t leaseTtlMs = 0;
    auto leaseValidator =
        VIntRange::Create(ConfConstant::OCK_MMC_META_LEASE_TTL_MS.first, MIN_LEASE_TTL_MS, MAX_LEASE_TTL_MS);
    if (GetValidatedInt(ConfConstant::OCK_MMC_META_LEASE_TTL_MS.first, kvParser, leaseValidator, leaseTtlMs) ==
        FieldStatus::OK) {
        newMetaConfig.leaseTtlMs = static_cast<uint64_t>(leaseTtlMs);
    }

    ApplyEvictThresholds(kvParser, newMetaConfig);

    const bool logLevelApplied = logLevelSet && ApplyMetaLogLevelFromConfig(newMetaConfig.logLevel);

    const bool paramsChanged = newMetaConfig.leaseTtlMs != current.leaseTtlMs ||
                               newMetaConfig.evictThresholdHigh != current.evictThresholdHigh ||
                               newMetaConfig.evictThresholdLow != current.evictThresholdLow;
    if (paramsChanged && metaService->UpdateConfig(newMetaConfig) != MMC_OK) {
        MMC_LOG_WARN("Failed to apply meta config hot-reload");
        return logLevelApplied;
    }
    return logLevelApplied || paramsChanged;
}

} // namespace mmc
} // namespace ock
