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

#include <chrono>
#include <future>
#include <string>

#include <pthread.h>

#include "gtest/gtest.h"

#include "mmc_meta_backup_mgr_default.h"

namespace ock {
namespace mmc {
struct ThreadObservation {
    int32_t threadNameResult = 0;
    std::string threadName;
};

// Access the private pool from this test translation unit without adding a production test hook.
template<typename Tag, typename Tag::Type member>
struct PrivateMemberAccessor {
    friend typename Tag::Type GetMember(Tag)
    {
        return member;
    }
};

struct BackupPoolMember {
    using Type = MmcThreadPoolPtr MMCMetaBackUpMgrDefault::*;
    friend Type GetMember(BackupPoolMember);
};

template struct PrivateMemberAccessor<BackupPoolMember, &MMCMetaBackUpMgrDefault::backupPool_>;

class MmcMetaBackupMgrTest : public testing::Test {};

TEST_F(MmcMetaBackupMgrTest, BackupPoolUsesDedicatedThreadName)
{
    constexpr uint32_t asyncFlushIntervalMs = 1000U;
    constexpr uint32_t asyncFlushBatchLimit = 1U;
    const auto poolTaskWaitTimeout = std::chrono::seconds(3);
    const std::string backupPoolNamePrefix = "backup_pool";

    auto defaultConf = MmcMakeRef<MMCMetaBackUpConfDefault>(MetaNetServerPtr{});
    ASSERT_NE(defaultConf, nullptr);
    defaultConf->Setup(asyncFlushIntervalMs, asyncFlushBatchLimit, nullptr, nullptr);
    MMCMetaBackUpConfPtr conf = Convert<MMCMetaBackUpConfDefault, MMCMetaBackUpConf>(defaultConf);

    MMCMetaBackUpMgrDefault backupMgr;
    ASSERT_EQ(backupMgr.Start(conf), MMC_OK);

    MmcThreadPoolPtr backupPool = backupMgr.*GetMember(BackupPoolMember{});
    ASSERT_NE(backupPool, nullptr);
    auto threadObservation = backupPool->Enqueue([] {
        constexpr size_t threadNameBufferSize = 16U;
        char threadName[threadNameBufferSize] = {};
        const int32_t threadNameResult = pthread_getname_np(pthread_self(), threadName, sizeof(threadName));
        return ThreadObservation{threadNameResult, threadName};
    });

    ASSERT_TRUE(threadObservation.valid());
    const auto waitStatus = threadObservation.wait_for(poolTaskWaitTimeout);
    backupMgr.Stop();
    ASSERT_EQ(waitStatus, std::future_status::ready);

    const ThreadObservation observation = threadObservation.get();
    EXPECT_EQ(observation.threadNameResult, 0);
    EXPECT_EQ(observation.threadName.rfind(backupPoolNamePrefix, 0), 0U);
}
} // namespace mmc
} // namespace ock
