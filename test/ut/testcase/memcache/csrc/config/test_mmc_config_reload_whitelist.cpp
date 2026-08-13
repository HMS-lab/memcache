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
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "mmc_config_reload_utils.h"
#include "mmc_config_const.h"
#include "mmc_functions.h"

using namespace testing;
using namespace std;
using namespace ock::mmc;

namespace {
constexpr time_t CONFIG_MTIME_BASE = 1700000000;
constexpr time_t CONFIG_MTIME_STEP = 100;

bool WriteWhitelistConfig(const string &filePath)
{
    ofstream ofs(filePath, ios::trunc);
    if (!ofs.is_open()) {
        return false;
    }
    ofs << "ock.mmc.log_level=info\n";
    ofs << "ock.mmc.client.retry_milliseconds=100\n";
    ofs << "ock.mmc.meta_service_url=tcp://127.0.0.1:5000\n";
    ofs << "ock.mmc.meta.lease_ttl_ms=5000\n";
    ofs.close();
    return true;
}

bool WriteNonWhitelistConfig(const string &filePath)
{
    ofstream ofs(filePath, ios::trunc);
    if (!ofs.is_open()) {
        return false;
    }
    ofs << "ock.mmc.log_path=/var/log/test\n";
    ofs << "ock.mmc.local_service.dram.size=1GB\n";
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

class TestMmcConfigReloadWhitelist : public testing::Test {
public:
    void SetUp() override
    {
        filePath_ = CreateTempConfigFile("/tmp/mmc_config_whitelist_XXXXXX");
        ASSERT_FALSE(filePath_.empty());
    }

    void TearDown() override
    {
        std::remove(filePath_.c_str());
    }

protected:
    string filePath_;
};

TEST_F(TestMmcConfigReloadWhitelist, WhitelistCoversSharedAndServiceItems)
{
    EXPECT_TRUE(IsDynamicConfigKey(ConfConstant::OCK_MMC_LOG_LEVEL.first));
    EXPECT_TRUE(IsDynamicConfigKey(ConfConstant::OKC_MMC_CLIENT_RETRY_MILLISECONDS.first));
    EXPECT_TRUE(IsDynamicConfigKey(ConfConstant::OCK_MMC_CLIENT_AGGREGATE_IO.first));
    EXPECT_TRUE(IsDynamicConfigKey(ConfConstant::OCK_MMC_CLIENT_AGGREGATE_NUM.first));
    EXPECT_TRUE(IsDynamicConfigKey(ConfConstant::OCK_MMC_CLIENT_BATCH_CHUNK_SIZE.first));
    EXPECT_TRUE(IsDynamicConfigKey(ConfConstant::OCK_MMC_CLIENT_BATCH_CHUNK_COUNT.first));
    EXPECT_TRUE(IsDynamicConfigKey(ConfConstant::OCK_MMC_META_SERVICE_URL.first));
    EXPECT_TRUE(IsDynamicConfigKey(ConfConstant::OKC_MMC_LOCAL_SERVICE_BM_IP_PORT.first));
    EXPECT_TRUE(IsDynamicConfigKey(ConfConstant::OCK_MMC_META_LEASE_TTL_MS.first));
    EXPECT_TRUE(IsDynamicConfigKey(ConfConstant::OKC_MMC_EVICT_THRESHOLD_HIGH.first));
    EXPECT_TRUE(IsDynamicConfigKey(ConfConstant::OKC_MMC_EVICT_THRESHOLD_LOW.first));
}

TEST_F(TestMmcConfigReloadWhitelist, NonWhitelistKeysAreRejected)
{
    EXPECT_FALSE(IsDynamicConfigKey("ock.mmc.log_path"));
    EXPECT_FALSE(IsDynamicConfigKey("ock.mmc.local_service.dram.size"));
    EXPECT_FALSE(IsDynamicConfigKey("ock.mmc.no_such_key"));
    EXPECT_FALSE(IsDynamicConfigKey(""));
}

TEST_F(TestMmcConfigReloadWhitelist, EnumMapsToConfigKeys)
{
    EXPECT_STREQ(GetDynamicConfigKey(DynamicConfigItem::LOG_LEVEL), ConfConstant::OCK_MMC_LOG_LEVEL.first);
    EXPECT_STREQ(GetDynamicConfigKey(DynamicConfigItem::CLIENT_RETRY_MILLISECONDS),
                 ConfConstant::OKC_MMC_CLIENT_RETRY_MILLISECONDS.first);
    EXPECT_STREQ(GetDynamicConfigKey(DynamicConfigItem::META_SERVICE_URL),
                 ConfConstant::OCK_MMC_META_SERVICE_URL.first);
    EXPECT_STREQ(GetDynamicConfigKey(DynamicConfigItem::CONFIG_STORE_URL),
                 ConfConstant::OKC_MMC_LOCAL_SERVICE_BM_IP_PORT.first);
    EXPECT_STREQ(GetDynamicConfigKey(DynamicConfigItem::META_LEASE_TTL_MS),
                 ConfConstant::OCK_MMC_META_LEASE_TTL_MS.first);
    EXPECT_STREQ(GetDynamicConfigKey(DynamicConfigItem::EVICT_THRESHOLD_HIGH),
                 ConfConstant::OKC_MMC_EVICT_THRESHOLD_HIGH.first);
}

TEST_F(TestMmcConfigReloadWhitelist, ReportSkipsWhitelistKeys)
{
    ASSERT_TRUE(WriteWhitelistConfig(filePath_));
    SetFileMtime(filePath_, CONFIG_MTIME_BASE);
    KVParser kvParser;
    ASSERT_EQ(kvParser.FromFile(filePath_), MMC_OK);
    ReportNonHotReloadableKeys(kvParser);
}

TEST_F(TestMmcConfigReloadWhitelist, ReportOnlyChangedKeysAgainstBaseline)
{
    ASSERT_TRUE(WriteNonWhitelistConfig(filePath_));
    SetFileMtime(filePath_, CONFIG_MTIME_BASE);
    KVParser firstParse;
    ASSERT_EQ(firstParse.FromFile(filePath_), MMC_OK);

    // 初始化时解析校验出的配置项作为固定基线：当前文件与基线一致，不上报。
    std::map<std::string, std::string> initialItems;
    initialItems["ock.mmc.log_path"] = "/var/log/test";
    initialItems["ock.mmc.local_service.dram.size"] = "1GB";
    SetNonHotReloadableBaseline(initialItems);

    // 当前文件与基线一致：不打印任何告警。
    std::ostringstream captured;
    std::streambuf *oldBuf = std::cout.rdbuf(captured.rdbuf());
    ReportNonHotReloadableKeys(firstParse);
    std::cout.rdbuf(oldBuf);
    EXPECT_EQ(captured.str().find("cannot be hot-reloaded"), std::string::npos);

    // 只修改其中一个非白名单项，其余保持不变，仅该 key 上报一次。
    {
        std::ofstream ofs(filePath_, std::ios::trunc);
        ofs << "ock.mmc.log_path=/var/log/other\n";
        ofs << "ock.mmc.local_service.dram.size=1GB\n";
    }
    SetFileMtime(filePath_, CONFIG_MTIME_BASE + CONFIG_MTIME_STEP);
    KVParser secondParse;
    ASSERT_EQ(secondParse.FromFile(filePath_), MMC_OK);
    captured.str("");
    std::cout.rdbuf(captured.rdbuf());
    ReportNonHotReloadableKeys(secondParse);
    std::cout.rdbuf(oldBuf);
    EXPECT_NE(captured.str().find("Config item <ock.mmc.log_path> cannot be hot-reloaded"), std::string::npos);
    EXPECT_EQ(captured.str().find("Config item <ock.mmc.local_service.dram.size> cannot be hot-reloaded"),
              std::string::npos);

    // 值恢复到与基线一致：不打印告警。
    {
        std::ofstream ofs(filePath_, std::ios::trunc);
        ofs << "ock.mmc.log_path=/var/log/test\n";
        ofs << "ock.mmc.local_service.dram.size=1GB\n";
    }
    KVParser thirdParse;
    ASSERT_EQ(thirdParse.FromFile(filePath_), MMC_OK);
    captured.str("");
    std::cout.rdbuf(captured.rdbuf());
    ReportNonHotReloadableKeys(thirdParse);
    std::cout.rdbuf(oldBuf);
    EXPECT_EQ(captured.str().find("cannot be hot-reloaded"), std::string::npos);
}

TEST_F(TestMmcConfigReloadWhitelist, RemovedKeyIsReported)
{
    // 非白名单 key 被从文件中删除即视为修改，直接报 error；其余项保持不变不报。
    std::map<std::string, std::string> initialItems;
    initialItems["ock.mmc.log_path"] = "/var/log/test";
    initialItems["ock.mmc.local_service.dram.size"] = "1GB";
    SetNonHotReloadableBaseline(initialItems);

    // 只删除 log_path，dram.size 保持不变。
    {
        std::ofstream ofs(filePath_, std::ios::trunc);
        ofs << "ock.mmc.local_service.dram.size=1GB\n";
    }
    KVParser kvParser;
    ASSERT_EQ(kvParser.FromFile(filePath_), MMC_OK);
    std::ostringstream captured;
    std::streambuf *oldBuf = std::cout.rdbuf(captured.rdbuf());
    ReportNonHotReloadableKeys(kvParser);
    std::cout.rdbuf(oldBuf);
    EXPECT_NE(captured.str().find("Config item <ock.mmc.log_path> cannot be hot-reloaded"), std::string::npos);
    EXPECT_EQ(captured.str().find("Config item <ock.mmc.local_service.dram.size> cannot be hot-reloaded"),
              std::string::npos);
}
