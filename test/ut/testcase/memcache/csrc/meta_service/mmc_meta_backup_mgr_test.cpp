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
#include "mmc_meta_service.h"

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

struct BackupListsByRankMember {
    using Type = std::map<uint32_t, std::list<MetaBackUpOperate>> MMCMetaBackUpMgrDefault::*;
    friend Type GetMember(BackupListsByRankMember);
};
template struct PrivateMemberAccessor<BackupListsByRankMember, &MMCMetaBackUpMgrDefault::backupListsByRank_>;

struct BackupPendingCountMember {
    using Type = size_t MMCMetaBackUpMgrDefault::*;
    friend Type GetMember(BackupPendingCountMember);
};
template struct PrivateMemberAccessor<BackupPendingCountMember, &MMCMetaBackUpMgrDefault::backupPendingCount_>;

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

TEST_F(MmcMetaBackupMgrTest, DrainRankOnlyRemovesItsOwnPendingQueue)
{
    constexpr uint32_t asyncFlushIntervalMs = 60000U;
    constexpr uint32_t asyncFlushBatchLimit = 1000U;
    constexpr uint32_t drainedRank = 1U;
    constexpr uint32_t retainedRank = 2U;
    constexpr uint64_t drainedGva = 4096U;
    constexpr uint64_t retainedGva = 8192U;
    constexpr uint64_t blobSize = 4096U;
    auto defaultConf = MmcMakeRef<MMCMetaBackUpConfDefault>(MetaNetServerPtr{});
    ASSERT_NE(defaultConf, nullptr);
    defaultConf->Setup(asyncFlushIntervalMs, asyncFlushBatchLimit, nullptr, nullptr);
    MMCMetaBackUpConfPtr conf = Convert<MMCMetaBackUpConfDefault, MMCMetaBackUpConf>(defaultConf);

    MMCMetaBackUpMgrDefault backupMgr;
    ASSERT_EQ(backupMgr.Start(conf), MMC_OK);
    MmcMemBlobDesc drainedDesc{drainedRank, drainedGva, blobSize, MEDIA_DRAM};
    MmcMemBlobDesc retainedDesc{retainedRank, retainedGva, blobSize, MEDIA_DRAM};
    ASSERT_EQ(backupMgr.Add("drained", drainedDesc, META_BACKUP_ADD), MMC_OK);
    ASSERT_EQ(backupMgr.Add("retained", retainedDesc, META_BACKUP_ADD), MMC_OK);

    ASSERT_TRUE(backupMgr.DrainRank(drainedRank));
    const auto &pending = backupMgr.*GetMember(BackupListsByRankMember{});
    EXPECT_EQ(pending.count(drainedRank), 0U);
    ASSERT_EQ(pending.count(retainedRank), 1U);
    EXPECT_EQ(pending.at(retainedRank).size(), 1U);
    EXPECT_EQ(backupMgr.*GetMember(BackupPendingCountMember{}), 1U);
    backupMgr.Stop();
}

struct BackupSendMember {
    using Type = void (MMCMetaBackUpMgrDefault::*)(uint32_t, std::vector<MetaBackUpOperate> &);
    friend Type GetMember(BackupSendMember);
};
template struct PrivateMemberAccessor<BackupSendMember, &MMCMetaBackUpMgrDefault::SendBackupForRank>;

struct BackupServerEngineMember {
    using Type = NetEnginePtr MetaNetServer::*;
    friend Type GetMember(BackupServerEngineMember);
};
template struct PrivateMemberAccessor<BackupServerEngineMember, &MetaNetServer::engine_>;

class BackupTestNetEngine : public NetEngine {
public:
    Result Start(const NetEngineOptions &) override
    {
        return MMC_OK;
    }

    void Stop() override {}

    Result ConnectToPeer(uint32_t, const std::string &, uint16_t, NetLinkPtr &, bool, bool) override
    {
        return MMC_ERROR;
    }

    Result Send(uint32_t, const char *, uint32_t, int32_t) override
    {
        return MMC_ERROR;
    }

    Result Call(uint32_t, int16_t, const char *, uint32_t, char **, uint32_t &, int32_t) override
    {
        ++callCount;
        return MMC_ERROR;
    }

    uint32_t callCount = 0;
};

TEST_F(MmcMetaBackupMgrTest, BackupSkipsRejectedDramSources)
{
    constexpr uint32_t rank = 1;
    constexpr uint64_t gva = 4096;
    constexpr uint64_t size = 4096;
    constexpr uint32_t oneAttempt = 1;
    uint32_t acquireCount = 0;
    // No server is configured: rejected sources must never reach the RPC path.
    auto defaultConf = MmcMakeRef<MMCMetaBackUpConfDefault>(MetaNetServerPtr{});
    defaultConf->acquireReadLease = [&acquireCount](const std::string &key, const MmcMemBlobDesc &, BackupReadLease &) {
        EXPECT_EQ(key, "stale");
        ++acquireCount;
        return MMC_UNMATCHED_KEY;
    };
    MMCMetaBackUpConfPtr conf = defaultConf.Get();
    MMCMetaBackUpMgrDefault backupMgr;
    ASSERT_EQ(backupMgr.Start(conf), MMC_OK);
    MmcMemBlobDesc desc{rank, gva, size, MEDIA_DRAM};
    std::vector<MetaBackUpOperate> entries{{META_BACKUP_ADD, "stale", desc}};
    (backupMgr.*GetMember(BackupSendMember{}))(rank, entries);
    EXPECT_EQ(acquireCount, oneAttempt);
    backupMgr.Stop();
}

TEST_F(MmcMetaBackupMgrTest, BackupSendsDramSourceOnlyWhenLeaseIsAcquired)
{
    constexpr uint32_t rank = 1;
    constexpr uint64_t gva = 4096;
    constexpr uint64_t size = 4096;
    constexpr uint32_t oneCall = 1;
    for (bool provideAcquire : {false, true}) {
        uint32_t acquireCount = 0;
        auto engine = MmcMakeRef<BackupTestNetEngine>();
        auto server = MmcMakeRef<MetaNetServer>(MmcMetaServicePtr{});
        server.Get()->*GetMember(BackupServerEngineMember{}) = engine.Get();
        auto defaultConf = MmcMakeRef<MMCMetaBackUpConfDefault>(server);
        if (provideAcquire) {
            defaultConf->acquireReadLease = [&acquireCount](const std::string &, const MmcMemBlobDesc &,
                                                            BackupReadLease &) {
                ++acquireCount;
                return MMC_OK;
            };
        }
        MMCMetaBackUpConfPtr conf = defaultConf.Get();
        MMCMetaBackUpMgrDefault backupMgr;
        ASSERT_EQ(backupMgr.Start(conf), MMC_OK);
        MmcMemBlobDesc desc{rank, gva, size, MEDIA_DRAM};
        std::vector<MetaBackUpOperate> entries{{META_BACKUP_ADD, "missing_hook", desc}};
        (backupMgr.*GetMember(BackupSendMember{}))(rank, entries);
        const uint32_t expectedCalls = provideAcquire ? oneCall : 0U;
        EXPECT_EQ(acquireCount, expectedCalls);
        EXPECT_EQ(engine->callCount, expectedCalls);
        backupMgr.Stop();
    }
}

} // namespace mmc
} // namespace ock
