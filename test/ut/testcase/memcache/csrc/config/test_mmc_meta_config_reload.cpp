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
#include "mmc_meta_config_reload.h"
#include "mmc_logger.h"
#include "mmc_meta_service.h"
#include "mmc_def.h"
#include "smem_logger_api.h"

using namespace testing;
using namespace std;
using namespace ock::mmc;

namespace {
constexpr time_t CONFIG_MTIME_BASE = 1700000000;
constexpr time_t CONFIG_MTIME_STEP = 100;
constexpr uint64_t TEST_LEASE_TTL_MS = 5000U;
constexpr int32_t TEST_EVICT_THRESHOLD_HIGH = 85;
constexpr int32_t TEST_EVICT_THRESHOLD_LOW = 75;

struct MetaConfigFields {
    string logLevel;
    uint64_t leaseTtlMs;
    int32_t evictHigh;
    int32_t evictLow;
};

bool WriteMetaConfig(const string &filePath, const MetaConfigFields &fields)
{
    ofstream ofs(filePath, ios::trunc);
    if (!ofs.is_open()) {
        return false;
    }
    ofs << "ock.mmc.meta_service_url=tcp://127.0.0.1:5000\n";
    ofs << "ock.mmc.log_level=" << fields.logLevel << "\n";
    ofs << "ock.mmc.meta.lease_ttl_ms=" << fields.leaseTtlMs << "\n";
    ofs << "ock.mmc.evict_threshold_high=" << fields.evictHigh << "\n";
    ofs << "ock.mmc.evict_threshold_low=" << fields.evictLow << "\n";
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

class TestMmcMetaConfigReload : public testing::Test {
public:
    void SetUp() override
    {
        ASSERT_EQ(MFLoggerApi::LoadLibrary(), MMC_OK);
        originalLevel_ = MmcOutLogger::Instance().GetLogLevel();
        svc_ = MmcMakeRef<MmcMetaService>("test-meta-config-reload");
        ASSERT_NE(svc_, nullptr);
        filePath_ = CreateTempConfigFile("/tmp/mmc_meta_config_reload_XXXXXX");
        ASSERT_FALSE(filePath_.empty());
    }

    void TearDown() override
    {
        MmcOutLogger::Instance().SetLogLevel(originalLevel_);
        std::remove(filePath_.c_str());
    }

protected:
    string filePath_;
    MmcRef<MmcMetaService> svc_;
    LogLevel originalLevel_ = INFO_LEVEL;
};

TEST_F(TestMmcMetaConfigReload, LogLevelRollsBackWhenSpdlogUnavailable)
{
    ASSERT_EQ(MmcOutLogger::Instance().SetLogLevel(DEBUG_LEVEL), 0);
    time_t lastMtime = 0;
    ASSERT_TRUE(
        WriteMetaConfig(filePath_, {"warn", TEST_LEASE_TTL_MS, TEST_EVICT_THRESHOLD_HIGH, TEST_EVICT_THRESHOLD_LOW}));
    SetFileMtime(filePath_, CONFIG_MTIME_BASE);

    // The meta daemon initializes spdlog before polling; in UT the singleton is not
    // initialized, so SPDLOG_ResetLogLevel fails and ALL meta loggers must roll back
    // to keep the service log level consistent.
    EXPECT_FALSE(ReloadMetaConfigFromFile(filePath_, lastMtime, svc_.Get()));
    EXPECT_EQ(MmcOutLogger::Instance().GetLogLevel(), DEBUG_LEVEL);
}

TEST_F(TestMmcMetaConfigReload, SkipsWhenMtimeUnchanged)
{
    ASSERT_EQ(MmcOutLogger::Instance().SetLogLevel(DEBUG_LEVEL), 0);
    time_t lastMtime = 0;
    ASSERT_TRUE(
        WriteMetaConfig(filePath_, {"warn", TEST_LEASE_TTL_MS, TEST_EVICT_THRESHOLD_HIGH, TEST_EVICT_THRESHOLD_LOW}));
    SetFileMtime(filePath_, CONFIG_MTIME_BASE);
    // First call: log level rolls back (spdlog unavailable in UT), returns false.
    EXPECT_FALSE(ReloadMetaConfigFromFile(filePath_, lastMtime, svc_.Get()));

    EXPECT_FALSE(ReloadMetaConfigFromFile(filePath_, lastMtime, svc_.Get()));
    EXPECT_EQ(MmcOutLogger::Instance().GetLogLevel(), DEBUG_LEVEL);
}

TEST_F(TestMmcMetaConfigReload, KeepsStateWhenFileMissing)
{
    time_t lastMtime = 0;
    EXPECT_FALSE(ReloadMetaConfigFromFile("/tmp/mmc_meta_config_reload_not_exist.conf", lastMtime, svc_.Get()));
}

TEST_F(TestMmcMetaConfigReload, KeepsStateOnLoadFailure)
{
    ASSERT_EQ(MmcOutLogger::Instance().SetLogLevel(INFO_LEVEL), 0);
    time_t lastMtime = 0;
    ASSERT_TRUE(WriteGarbageConfig(filePath_));
    SetFileMtime(filePath_, CONFIG_MTIME_BASE);

    EXPECT_FALSE(ReloadMetaConfigFromFile(filePath_, lastMtime, svc_.Get()));
    EXPECT_EQ(MmcOutLogger::Instance().GetLogLevel(), INFO_LEVEL);
}

TEST_F(TestMmcMetaConfigReload, IgnoresNonHotReloadableKey)
{
    ASSERT_EQ(MmcOutLogger::Instance().SetLogLevel(DEBUG_LEVEL), 0);
    time_t lastMtime = 0;
    ASSERT_TRUE(
        WriteMetaConfig(filePath_, {"warn", TEST_LEASE_TTL_MS, TEST_EVICT_THRESHOLD_HIGH, TEST_EVICT_THRESHOLD_LOW}));
    // dynamic_config.interval is not a hot-reloadable field and is ignored; an
    // invalid value there must not break the reload flow.
    {
        std::ofstream ofs(filePath_, std::ios::app);
        ofs << "ock.mmc.dynamic_config.interval=0\n";
    }
    SetFileMtime(filePath_, CONFIG_MTIME_BASE);

    EXPECT_FALSE(ReloadMetaConfigFromFile(filePath_, lastMtime, svc_.Get()));
    EXPECT_EQ(MmcOutLogger::Instance().GetLogLevel(), DEBUG_LEVEL);
}

TEST_F(TestMmcMetaConfigReload, SkipsWhenMetaServiceNull)
{
    time_t lastMtime = 0;
    ASSERT_TRUE(
        WriteMetaConfig(filePath_, {"warn", TEST_LEASE_TTL_MS, TEST_EVICT_THRESHOLD_HIGH, TEST_EVICT_THRESHOLD_LOW}));
    SetFileMtime(filePath_, CONFIG_MTIME_BASE);

    EXPECT_FALSE(ReloadMetaConfigFromFile(filePath_, lastMtime, nullptr));
}
