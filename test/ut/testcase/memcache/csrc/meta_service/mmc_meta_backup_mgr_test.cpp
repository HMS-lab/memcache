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

struct BackupSendMember {
    using Type = void (MMCMetaBackUpMgrDefault::*)(uint32_t, std::vector<MetaBackUpOperate> &);
    friend Type GetMember(BackupSendMember);
};
template struct PrivateMemberAccessor<BackupSendMember, &MMCMetaBackUpMgrDefault::SendBackupForRank>;

TEST_F(MmcMetaBackupMgrTest, BackupSkipsRejectedDramSources)
{
    constexpr uint32_t rank = 1;
    constexpr uint64_t gva = 4096;
    constexpr uint64_t size = 4096;
    constexpr uint32_t oneAttempt = 1;
    uint32_t acquireCount = 0;
    uint32_t releaseCount = 0;
    // No server is configured: rejected sources must never reach the RPC path.
    auto defaultConf = MmcMakeRef<MMCMetaBackUpConfDefault>(MetaNetServerPtr{});
    defaultConf->acquireReadLease = [&acquireCount](const std::string &key, const MmcMemBlobDesc &, BackupReadLease &) {
        EXPECT_EQ(key, "stale");
        ++acquireCount;
        return MMC_UNMATCHED_KEY;
    };
    defaultConf->releaseReadLease = [&releaseCount](const BackupReadLease &) { ++releaseCount; };
    MMCMetaBackUpConfPtr conf = defaultConf.Get();
    MMCMetaBackUpMgrDefault backupMgr;
    ASSERT_EQ(backupMgr.Start(conf), MMC_OK);
    MmcMemBlobDesc desc{rank, gva, size, MEDIA_DRAM};
    std::vector<MetaBackUpOperate> entries{{META_BACKUP_ADD, "stale", desc}};
    (backupMgr.*GetMember(BackupSendMember{}))(rank, entries);
    EXPECT_EQ(acquireCount, oneAttempt);
    EXPECT_EQ(releaseCount, 0U);
    backupMgr.Stop();
}

TEST_F(MmcMetaBackupMgrTest, BackupSkipsDramSourcesWithoutLeaseHooks)
{
    constexpr uint32_t rank = 1;
    constexpr uint64_t gva = 4096;
    constexpr uint64_t size = 4096;
    for (bool provideAcquire : {false, true}) {
        uint32_t acquireCount = 0;
        uint32_t releaseCount = 0;
        auto defaultConf = MmcMakeRef<MMCMetaBackUpConfDefault>(MetaNetServerPtr{});
        if (provideAcquire) {
            defaultConf->acquireReadLease = [&acquireCount](const std::string &, const MmcMemBlobDesc &,
                                                            BackupReadLease &) {
                ++acquireCount;
                return MMC_OK;
            };
        } else {
            defaultConf->releaseReadLease = [&releaseCount](const BackupReadLease &) { ++releaseCount; };
        }
        MMCMetaBackUpConfPtr conf = defaultConf.Get();
        MMCMetaBackUpMgrDefault backupMgr;
        ASSERT_EQ(backupMgr.Start(conf), MMC_OK);
        MmcMemBlobDesc desc{rank, gva, size, MEDIA_DRAM};
        std::vector<MetaBackUpOperate> entries{{META_BACKUP_ADD, "missing_hook", desc}};
        (backupMgr.*GetMember(BackupSendMember{}))(rank, entries);
        EXPECT_EQ(acquireCount, 0U);
        EXPECT_EQ(releaseCount, 0U);
        backupMgr.Stop();
    }
}

} // namespace mmc
} // namespace ock
