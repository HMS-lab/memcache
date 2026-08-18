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
#include "mmc_local_config_reload.h"

#include "mmc_client_default.h"
#include "mmc_config_reload_utils.h"
#include "mmc_configuration.h"
#include "mmc_functions.h"
#include "mmc_kv_parser.h"
#include "mmc_logger.h"

namespace ock {
namespace mmc {

static bool ApplyLogLevelFromConfig(int32_t newLogLevel)
{
    if (newLogLevel == MmcOutLogger::Instance().GetLogLevel()) {
        return false;
    }
    if (MmcOutLogger::Instance().SetLogLevel(static_cast<LogLevel>(newLogLevel)) != 0) {
        MMC_LOG_ERROR("Invalid log level " << newLogLevel);
        return false;
    }
    MMC_LOG_TRACE("log_level hot-reloaded to " << newLogLevel);
    return true;
}

static uint32_t ParseClientConfigFields(KVParser &kvParser, mmc_client_config_t &newClientConfig)
{
    uint32_t clientFieldMask = 0;

    int32_t retryMs = 0;
    auto retryValidator =
        VIntRange::Create(ConfConstant::OKC_MMC_CLIENT_RETRY_MILLISECONDS.first, MIN_RETRY_MS, MAX_RETRY_MS);
    if (GetValidatedInt(ConfConstant::OKC_MMC_CLIENT_RETRY_MILLISECONDS.first, kvParser, retryValidator, retryMs) ==
        FieldStatus::OK) {
        newClientConfig.rpcRetryTimeOut = static_cast<uint32_t>(retryMs);
        clientFieldMask |= CLIENT_CONFIG_FIELD_RPC_RETRY_TIMEOUT;
    }

    std::string boolStr;
    if (kvParser.GetItem(ConfConstant::OCK_MMC_CLIENT_AGGREGATE_IO.first, boolStr) == MMC_OK) {
        auto validator = VStrEnum::Create(ConfConstant::OCK_MMC_CLIENT_AGGREGATE_IO.first, BOOL_ENUM_STR);
        bool aggregateIo = false;
        if (validator == nullptr || !validator->Validate(boolStr) || !IsBool(boolStr, aggregateIo)) {
            MMC_LOG_ERROR("Invalid value for <" << ConfConstant::OCK_MMC_CLIENT_AGGREGATE_IO.first << ">: " << boolStr);
        } else {
            newClientConfig.aggregateIO = aggregateIo;
            clientFieldMask |= CLIENT_CONFIG_FIELD_AGGREGATE_IO;
        }
    }

    int32_t aggregateNum = 0;
    auto aggregateNumValidator =
        VIntRange::Create(ConfConstant::OCK_MMC_CLIENT_AGGREGATE_NUM.first, 1, MAX_AGGREGATE_NUM);
    if (GetValidatedInt(ConfConstant::OCK_MMC_CLIENT_AGGREGATE_NUM.first, kvParser, aggregateNumValidator,
                        aggregateNum) == FieldStatus::OK) {
        newClientConfig.aggregateNum = aggregateNum;
        clientFieldMask |= CLIENT_CONFIG_FIELD_AGGREGATE_NUM;
    }

    std::string chunkSizeStr;
    if (kvParser.GetItem(ConfConstant::OCK_MMC_CLIENT_BATCH_CHUNK_SIZE.first, chunkSizeStr) == MMC_OK) {
        uint64_t chunkSize = Configuration::ParseMemSize(chunkSizeStr);
        if (chunkSize != UINT64_MAX) {
            newClientConfig.batchChunkSize = chunkSize;
            clientFieldMask |= CLIENT_CONFIG_FIELD_BATCH_CHUNK_SIZE;
        } else {
            MMC_LOG_ERROR("Invalid value for <" << ConfConstant::OCK_MMC_CLIENT_BATCH_CHUNK_SIZE.first
                                                << ">: " << chunkSizeStr);
        }
    }

    int32_t chunkCount = 0;
    auto chunkCountValidator =
        VIntRange::Create(ConfConstant::OCK_MMC_CLIENT_BATCH_CHUNK_COUNT.first, 1, MAX_BATCH_CHUNK_COUNT);
    if (GetValidatedInt(ConfConstant::OCK_MMC_CLIENT_BATCH_CHUNK_COUNT.first, kvParser, chunkCountValidator,
                        chunkCount) == FieldStatus::OK) {
        newClientConfig.batchChunkCount = static_cast<uint32_t>(chunkCount);
        clientFieldMask |= CLIENT_CONFIG_FIELD_BATCH_CHUNK_COUNT;
    }

    return clientFieldMask;
}

bool ReloadLocalConfigFromFile(const std::string &configFilePath, time_t &lastConfigMtime)
{
    if (!IsConfigFileModified(configFilePath, lastConfigMtime)) {
        return false;
    }

    KVParser kvParser;
    if (kvParser.FromFile(configFilePath) != MMC_OK) {
        MMC_LOG_WARN("Failed to parse config file: " << configFilePath);
        return false;
    }

    ReportNonHotReloadableKeys(kvParser);

    bool logLevelApplied = false;
    std::string logLevelStr;
    if (kvParser.GetItem(ConfConstant::OCK_MMC_LOG_LEVEL.first, logLevelStr) == MMC_OK) {
        auto validator = VStrEnum::Create(ConfConstant::OCK_MMC_LOG_LEVEL.first, LOG_LEVEL_ENUM_STR);
        if (validator == nullptr || !validator->Validate(logLevelStr)) {
            MMC_LOG_ERROR("Invalid log level \"" << logLevelStr << "\"");
        } else {
            StringToUpper(logLevelStr);
            logLevelApplied = ApplyLogLevelFromConfig(MmcOutLogger::Instance().GetLogLevel(logLevelStr));
        }
    }

    auto *client = MmcClientDefault::GetInstance();
    if (client == nullptr) {
        MMC_LOG_WARN("Client not initialized, skip client config hot-reload");
        return logLevelApplied;
    }

    mmc_client_config_t newClientConfig{};
    const uint32_t clientFieldMask = ParseClientConfigFields(kvParser, newClientConfig);
    const bool clientApplied = (clientFieldMask != 0) && client->UpdateClientConfig(newClientConfig, clientFieldMask);
    return logLevelApplied || clientApplied;
}

} // namespace mmc
} // namespace ock
