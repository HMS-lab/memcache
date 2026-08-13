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

#include "gtest/gtest.h"

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "mmc_functions.h"
#include "mmc_local_config_reload.h"
#include "mmc_client_default.h"

#include "mmc_def.h"
#include "mmc_logger.h"

using namespace testing;
using namespace std;
using namespace ock::mmc;

namespace {
constexpr time_t CONFIG_MTIME_BASE = 1700000000;
constexpr time_t CONFIG_MTIME_STEP = 100;
constexpr uint32_t DEFAULT_RPC_RETRY_MS = 0;
constexpr uint32_t FIRST_RPC_RETRY_MS = 1000;
constexpr uint32_t SECOND_RPC_RETRY_MS = 2000;
constexpr int32_t DEFAULT_AGGREGATE_NUM = 0;
constexpr int32_t FIRST_AGGREGATE_NUM = 122;
constexpr int32_t SECOND_AGGREGATE_NUM = 256;
constexpr uint64_t FIRST_BATCH_CHUNK_SIZE = 8ULL * 1024ULL * 1024ULL;
constexpr uint64_t SECOND_BATCH_CHUNK_SIZE = 16ULL * 1024ULL * 1024ULL;
constexpr uint32_t FIRST_BATCH_CHUNK_COUNT = 3;
constexpr uint32_t SECOND_BATCH_CHUNK_COUNT = 8;

struct LocalConfigFields {
    string logLevel;
    uint32_t retryMs;
    bool aggregateIo;
    int32_t aggregateNum;
    string chunkSize;
    uint32_t chunkCount;
};

bool WriteLocalConfig(const string &filePath, const LocalConfigFields &fields)
{
    ofstream ofs(filePath, ios::trunc);
    if (!ofs.is_open()) {
        return false;
    }
    ofs << "ock.mmc.meta_service_url=tcp://127.0.0.1:5000\n";
    ofs << "ock.mmc.local_service.config_store_url=tcp://127.0.0.1:6000\n";
    ofs << "ock.mmc.local_service.protocol=host_rdma\n";
    ofs << "ock.mmc.local_service.dram.size=1GB\n";
    ofs << "ock.mmc.log_level=" << fields.logLevel << "\n";
    ofs << "ock.mmc.client.retry_milliseconds=" << fields.retryMs << "\n";
    ofs << "ock.mmc.client.aggregate.io=" << (fields.aggregateIo ? "true" : "false") << "\n";
    ofs << "ock.mmc.client.aggregate.num=" << fields.aggregateNum << "\n";
    ofs << "ock.mmc.client.batch_option.chunk.size=" << fields.chunkSize << "\n";
    ofs << "ock.mmc.client.batch_option.chunk.count=" << fields.chunkCount << "\n";
    ofs.close();
    return true;
}

bool WriteGarbageConfig(const string &filePath)
{
    ofstream ofs(filePath, ios::trunc);
    if (!ofs.is_open()) {
        return false;
    }
    ofs << "ock.mmc.no_such_key=1\n";
    ofs.close();
    return true;
}

void SetFileMtime(const string &filePath, time_t seconds)
{
    struct timespec times[2] = {{seconds, 0}, {seconds, 0}};
    ASSERT_EQ(utimensat(AT_FDCWD, filePath.c_str(), times, 0), 0);
}

string CreateTempConfigFile(const char *fullTemplate)
{
    char fileNameTemplate[64] = {0};
    SafeCopy(std::string(fullTemplate), fileNameTemplate, sizeof(fileNameTemplate));
    int fd = mkstemp(fileNameTemplate);
    if (fd == -1) {
        return "";
    }
    close(fd);
    const string filePath = string(fileNameTemplate) + ".conf";
    std::remove(fileNameTemplate);
    return filePath;
}
} // namespace

class TestMmcLocalConfigReload : public testing::Test {
public:
    void SetUp() override
    {
        ASSERT_EQ(MmcClientDefault::RegisterInstance(), MMC_OK);
        originalLevel_ = MmcOutLogger::Instance().GetLogLevel();
        filePath_ = CreateTempConfigFile("/tmp/mmc_local_config_reload_XXXXXX");
        ASSERT_FALSE(filePath_.empty());
    }

    void TearDown() override
    {
        MmcOutLogger::Instance().SetLogLevel(originalLevel_);
        MmcClientDefault::UnregisterInstance();
        std::remove(filePath_.c_str());
    }

protected:
    string filePath_;
    LogLevel originalLevel_ = INFO_LEVEL;
};

TEST_F(TestMmcLocalConfigReload, AppliesLogLevelAndClientConfig)
{
    auto *client = MmcClientDefault::GetInstance();
    ASSERT_NE(client, nullptr);
    ASSERT_EQ(MmcOutLogger::Instance().SetLogLevel(DEBUG_LEVEL), 0);
    EXPECT_EQ(client->GetRpcRetryTimeOut(), DEFAULT_RPC_RETRY_MS);

    time_t lastMtime = 0;
    ASSERT_TRUE(WriteLocalConfig(
        filePath_, {"info", FIRST_RPC_RETRY_MS, true, FIRST_AGGREGATE_NUM, "8MB", FIRST_BATCH_CHUNK_COUNT}));
    SetFileMtime(filePath_, CONFIG_MTIME_BASE);

    EXPECT_TRUE(ReloadLocalConfigFromFile(filePath_, lastMtime));
    EXPECT_EQ(MmcOutLogger::Instance().GetLogLevel(), INFO_LEVEL);
    EXPECT_EQ(client->GetRpcRetryTimeOut(), FIRST_RPC_RETRY_MS);
    EXPECT_EQ(client->GetAggregateIO(), true);
    EXPECT_EQ(static_cast<int32_t>(client->GetAggregateNum()), FIRST_AGGREGATE_NUM);
    EXPECT_EQ(client->GetBatchChunkSize(), FIRST_BATCH_CHUNK_SIZE);
    EXPECT_EQ(client->GetBatchChunkCount(), FIRST_BATCH_CHUNK_COUNT);
}

TEST_F(TestMmcLocalConfigReload, SkipsWhenMtimeUnchanged)
{
    auto *client = MmcClientDefault::GetInstance();
    ASSERT_NE(client, nullptr);
    time_t lastMtime = 0;
    ASSERT_TRUE(WriteLocalConfig(
        filePath_, {"info", FIRST_RPC_RETRY_MS, true, FIRST_AGGREGATE_NUM, "8MB", FIRST_BATCH_CHUNK_COUNT}));
    SetFileMtime(filePath_, CONFIG_MTIME_BASE);
    ASSERT_TRUE(ReloadLocalConfigFromFile(filePath_, lastMtime));

    EXPECT_FALSE(ReloadLocalConfigFromFile(filePath_, lastMtime));
    EXPECT_EQ(MmcOutLogger::Instance().GetLogLevel(), INFO_LEVEL);
    EXPECT_EQ(client->GetRpcRetryTimeOut(), FIRST_RPC_RETRY_MS);
}

TEST_F(TestMmcLocalConfigReload, AppliesConfigChange)
{
    auto *client = MmcClientDefault::GetInstance();
    ASSERT_NE(client, nullptr);
    ASSERT_EQ(MmcOutLogger::Instance().SetLogLevel(DEBUG_LEVEL), 0);
    time_t lastMtime = 0;
    ASSERT_TRUE(WriteLocalConfig(
        filePath_, {"info", FIRST_RPC_RETRY_MS, true, FIRST_AGGREGATE_NUM, "8MB", FIRST_BATCH_CHUNK_COUNT}));
    SetFileMtime(filePath_, CONFIG_MTIME_BASE);
    ASSERT_TRUE(ReloadLocalConfigFromFile(filePath_, lastMtime));

    ASSERT_TRUE(WriteLocalConfig(
        filePath_, {"warn", SECOND_RPC_RETRY_MS, false, SECOND_AGGREGATE_NUM, "16MB", SECOND_BATCH_CHUNK_COUNT}));
    SetFileMtime(filePath_, CONFIG_MTIME_BASE + CONFIG_MTIME_STEP);
    EXPECT_TRUE(ReloadLocalConfigFromFile(filePath_, lastMtime));
    EXPECT_EQ(MmcOutLogger::Instance().GetLogLevel(), WARN_LEVEL);
    EXPECT_EQ(client->GetRpcRetryTimeOut(), SECOND_RPC_RETRY_MS);
    EXPECT_EQ(client->GetAggregateIO(), false);
    EXPECT_EQ(static_cast<int32_t>(client->GetAggregateNum()), SECOND_AGGREGATE_NUM);
    EXPECT_EQ(client->GetBatchChunkSize(), SECOND_BATCH_CHUNK_SIZE);
    EXPECT_EQ(client->GetBatchChunkCount(), SECOND_BATCH_CHUNK_COUNT);
}

TEST_F(TestMmcLocalConfigReload, KeepsConfigOnLoadFailure)
{
    auto *client = MmcClientDefault::GetInstance();
    ASSERT_NE(client, nullptr);
    ASSERT_EQ(MmcOutLogger::Instance().SetLogLevel(INFO_LEVEL), 0);
    time_t lastMtime = 0;
    ASSERT_TRUE(WriteLocalConfig(
        filePath_, {"info", FIRST_RPC_RETRY_MS, true, FIRST_AGGREGATE_NUM, "8MB", FIRST_BATCH_CHUNK_COUNT}));
    SetFileMtime(filePath_, CONFIG_MTIME_BASE);
    ASSERT_TRUE(ReloadLocalConfigFromFile(filePath_, lastMtime));

    ASSERT_TRUE(WriteGarbageConfig(filePath_));
    SetFileMtime(filePath_, CONFIG_MTIME_BASE + CONFIG_MTIME_STEP);
    EXPECT_FALSE(ReloadLocalConfigFromFile(filePath_, lastMtime));
    EXPECT_EQ(MmcOutLogger::Instance().GetLogLevel(), INFO_LEVEL);
    EXPECT_EQ(client->GetRpcRetryTimeOut(), FIRST_RPC_RETRY_MS);
}

TEST_F(TestMmcLocalConfigReload, AppliesLogLevelWhenFullReloadFails)
{
    ASSERT_EQ(MmcOutLogger::Instance().SetLogLevel(DEBUG_LEVEL), 0);
    time_t lastMtime = 0;
    // aggregate.num=0 is out of the valid [1, 131072] range: the invalid field is
    // skipped, log_level is still applied.
    ASSERT_TRUE(WriteLocalConfig(filePath_, {"warn", FIRST_RPC_RETRY_MS, true, 0, "8MB", FIRST_BATCH_CHUNK_COUNT}));
    SetFileMtime(filePath_, CONFIG_MTIME_BASE);

    EXPECT_TRUE(ReloadLocalConfigFromFile(filePath_, lastMtime));
    EXPECT_EQ(MmcOutLogger::Instance().GetLogLevel(), WARN_LEVEL);
}

TEST_F(TestMmcLocalConfigReload, AppliesValidFieldsWhenOneFieldInvalid)
{
    auto *client = MmcClientDefault::GetInstance();
    ASSERT_NE(client, nullptr);
    time_t lastMtime = 0;
    // aggregate.num=0 is out of range: only that field is skipped, the other valid
    // fields (rpcRetryTimeOut, aggregateIO) are still applied.
    ASSERT_TRUE(WriteLocalConfig(filePath_, {"info", FIRST_RPC_RETRY_MS, true, 0, "8MB", FIRST_BATCH_CHUNK_COUNT}));
    SetFileMtime(filePath_, CONFIG_MTIME_BASE);

    EXPECT_TRUE(ReloadLocalConfigFromFile(filePath_, lastMtime));
    EXPECT_EQ(client->GetRpcRetryTimeOut(), FIRST_RPC_RETRY_MS);
    EXPECT_EQ(client->GetAggregateIO(), true);
    EXPECT_EQ(static_cast<int32_t>(client->GetAggregateNum()), DEFAULT_AGGREGATE_NUM);
}

TEST_F(TestMmcLocalConfigReload, KeepsConfigWhenFileMissing)
{
    auto *client = MmcClientDefault::GetInstance();
    ASSERT_NE(client, nullptr);
    time_t lastMtime = 0;

    EXPECT_FALSE(ReloadLocalConfigFromFile("/tmp/mmc_local_config_reload_not_exist.conf", lastMtime));
    EXPECT_EQ(client->GetRpcRetryTimeOut(), DEFAULT_RPC_RETRY_MS);
}

TEST_F(TestMmcLocalConfigReload, LogLevelAppliedWhenClientNotRegistered)
{
    ASSERT_EQ(MmcOutLogger::Instance().SetLogLevel(DEBUG_LEVEL), 0);
    MmcClientDefault::UnregisterInstance();
    time_t lastMtime = 0;
    ASSERT_TRUE(WriteLocalConfig(
        filePath_, {"warn", FIRST_RPC_RETRY_MS, true, FIRST_AGGREGATE_NUM, "8MB", FIRST_BATCH_CHUNK_COUNT}));
    SetFileMtime(filePath_, CONFIG_MTIME_BASE);

    EXPECT_TRUE(ReloadLocalConfigFromFile(filePath_, lastMtime));
    EXPECT_EQ(MmcOutLogger::Instance().GetLogLevel(), WARN_LEVEL);
    ASSERT_EQ(MmcClientDefault::RegisterInstance(), MMC_OK);
}
