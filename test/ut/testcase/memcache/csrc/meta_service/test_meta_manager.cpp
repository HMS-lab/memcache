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
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <thread>
#include "gtest/gtest.h"
#include "mmc_meta_manager.h"
#include "mmc_def.h"
#include "mmc_ref.h"

using namespace testing;
using namespace std;

namespace ock {
namespace mmc {

class TestMmcMetaManager : public testing::Test {
public:
    TestMmcMetaManager();

    void SetUp() override;

    void TearDown() override;

protected:
    // 桥接访问 MmcMetaManager 私有成员（TestMmcMetaManager 是 friend）
    static auto &MetaContainer(MmcRef<MmcMetaManager> &mgr)
    {
        return mgr->metaContainer_;
    }
    static auto &GlobalAllocator(MmcRef<MmcMetaManager> &mgr)
    {
        return mgr->globalAllocator_;
    }
    static auto &RewarmThreadPool(MmcRef<MmcMetaManager> &mgr)
    {
        return mgr->rewarmThreadPool_;
    }
    // 桥接调用私有方法 ForceEvictForFragmentation（同步执行，返回释放的 key 个数）
    static size_t ForceEvictForFragmentation(MmcRef<MmcMetaManager> &mgr, MediaType media,
                                             const std::vector<uint32_t> &fragmentedRanks, uint64_t wantAllocSize)
    {
        return mgr->ForceEvictForFragmentation(media, fragmentedRanks, wantAllocSize);
    }
    // 桥接读取碎片整理单飞行标志（TestMmcMetaManager 是 friend），用于测试等待异步整理完成
    static bool DefragIdle(MmcRef<MmcMetaManager> &mgr)
    {
        return !mgr->defragCheck_.load(std::memory_order_relaxed);
    }
};
TestMmcMetaManager::TestMmcMetaManager() {}

void TestMmcMetaManager::SetUp() {}

void TestMmcMetaManager::TearDown() {}

static Result QueryKey(const MmcRef<MmcMetaManager> &metaMng, const std::string &key, uint64_t operateId,
                       MemObjQueryInfo &queryInfo)
{
    return metaMng->Query(key, operateId, 0, queryInfo);
}

TEST_F(TestMmcMetaManager, Init)
{
    MmcLocation loc{0, MEDIA_DRAM};
    MmcLocalMemlInitInfo locInfo{100, 1000};

    uint64_t defaultTtl = 2000;
    MmcRef<MmcMetaManager> metaMng = MmcMakeRef<MmcMetaManager>(defaultTtl, 70U, 60U);
    metaMng->Start();
    std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
    metaMng->Mount(loc, locInfo, blobMap);
    ASSERT_TRUE(metaMng != nullptr);
    metaMng->Stop();
}

TEST_F(TestMmcMetaManager, PendingGetDoesNotDependOnRewarmWorkerAvailability)
{
    MmcMetaExtConfig extConfig;
    extConfig.pendingWaitTimeoutMs = 500U;
    MmcRef<MmcMetaManager> metaMng = MmcMakeRef<MmcMetaManager>(2000U, 70U, 60U, extConfig);
    ASSERT_EQ(metaMng->Start(), MMC_OK);

    const std::string key = "pending_without_rewarm_worker";
    auto memObj = MmcMakeRef<MmcMemObjMeta>();
    auto pendingBlob = MmcMakeRef<MmcMemBlob>(0, 0x1000, SIZE_32K, MEDIA_DRAM, ALLOCATED, SIZE_32K);
    ASSERT_NE(memObj, nullptr);
    ASSERT_NE(pendingBlob, nullptr);
    ASSERT_EQ(memObj->AddBlob(pendingBlob), MMC_OK);
    ASSERT_EQ(MetaContainer(metaMng)->Insert(key, memObj), MMC_OK);

    std::promise<void> releaseWorkers;
    auto releaseFuture = releaseWorkers.get_future().share();
    std::atomic<size_t> startedWorkers{0};
    std::vector<std::future<void>> blockers;
    blockers.reserve(REWARM_POOL_BASE);
    for (size_t i = 0; i < REWARM_POOL_BASE; ++i) {
        blockers.push_back(RewarmThreadPool(metaMng)->Enqueue([&startedWorkers, releaseFuture]() {
            startedWorkers.fetch_add(1, std::memory_order_release);
            releaseFuture.wait();
        }));
    }

    const auto workersReadyDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (startedWorkers.load(std::memory_order_acquire) != REWARM_POOL_BASE &&
           std::chrono::steady_clock::now() < workersReadyDeadline) {
        std::this_thread::yield();
    }
    if (startedWorkers.load(std::memory_order_acquire) != REWARM_POOL_BASE) {
        releaseWorkers.set_value();
        for (auto &blocker : blockers) {
            blocker.get();
        }
        metaMng->Stop();
        FAIL() << "rewarm pool did not start all workers";
    }

    std::thread completeRewarm([memObj, pendingBlob]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        std::lock_guard<std::mutex> guard(memObj->Mutex());
        pendingBlob->UpdateState(MMC_WRITE_OK);
        pendingBlob->NotifyReadable();
    });

    std::vector<MmcMemMetaDesc> objMetas;
    auto getFuture = std::async(std::launch::async, [&]() { return metaMng->GetByRank({key}, 1U, objMetas); });
    const auto getStatus = getFuture.wait_for(std::chrono::milliseconds(200));

    // Always release the synthetic blockers before assertions so failures cannot hang teardown.
    releaseWorkers.set_value();
    for (auto &blocker : blockers) {
        blocker.get();
    }
    completeRewarm.join();

    EXPECT_EQ(getStatus, std::future_status::ready);
    EXPECT_EQ(getFuture.get(), MMC_OK);
    ASSERT_EQ(objMetas.size(), 1U);
    EXPECT_EQ(objMetas[0].numBlobs_, 1U);
    metaMng->Stop();
}

TEST_F(TestMmcMetaManager, PendingPrefetchDoesNotAddDramReadLease)
{
    auto metaMng = MmcMakeRef<MmcMetaManager>(2000U, 70U, 60U);
    ASSERT_EQ(metaMng->Start(), MMC_OK);

    const std::string key = "pending_prefetch_without_read_lease";
    auto memObj = MmcMakeRef<MmcMemObjMeta>();
    auto pendingBlob = MmcMakeRef<MmcMemBlob>(0, 0x1000, SIZE_32K, MEDIA_DRAM, ALLOCATED, SIZE_32K);
    ASSERT_EQ(memObj->AddBlob(pendingBlob), MMC_OK);
    ASSERT_EQ(MetaContainer(metaMng)->Insert(key, memObj), MMC_OK);

    std::thread completeRewarm([memObj, pendingBlob]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        std::lock_guard<std::mutex> guard(memObj->Mutex());
        pendingBlob->UpdateState(MMC_WRITE_OK);
        pendingBlob->NotifyReadable();
    });

    std::vector<MmcMemMetaDesc> objMetas;
    const auto ret = metaMng->GetByRank({key}, GenerateOperateId(UINT32_MAX), objMetas, {}, false);
    completeRewarm.join();
    EXPECT_EQ(ret, MMC_OK);
    ASSERT_EQ(objMetas.size(), 1U);
    EXPECT_EQ(objMetas[0].numBlobs_, 1U);
    EXPECT_EQ(pendingBlob->UseCount(), 0U);

    metaMng->Stop();
}

TEST_F(TestMmcMetaManager, AsyncPrefetchDoesNotWaitForPendingBlob)
{
    MmcMetaExtConfig extConfig;
    extConfig.pendingWaitTimeoutMs = 2000U;
    auto metaMng = MmcMakeRef<MmcMetaManager>(2000U, 70U, 60U, extConfig);
    ASSERT_EQ(metaMng->Start(), MMC_OK);

    const std::string key = "pending_async_prefetch";
    auto memObj = MmcMakeRef<MmcMemObjMeta>();
    auto pendingBlob = MmcMakeRef<MmcMemBlob>(0, 0x1000, SIZE_32K, MEDIA_DRAM, ALLOCATED, SIZE_32K);
    ASSERT_EQ(memObj->AddBlob(pendingBlob), MMC_OK);
    ASSERT_EQ(MetaContainer(metaMng)->Insert(key, memObj), MMC_OK);

    auto submitFuture = std::async(std::launch::async, [&]() { return metaMng->AsyncPrefetchKeys({key}); });
    const auto submitStatus = submitFuture.wait_for(std::chrono::milliseconds(500));
    {
        std::lock_guard<std::mutex> guard(memObj->Mutex());
        pendingBlob->UpdateState(MMC_WRITE_OK);
        pendingBlob->NotifyReadable();
    }
    EXPECT_EQ(submitStatus, std::future_status::ready);
    EXPECT_EQ(submitFuture.get(), MMC_OK);
    EXPECT_EQ(pendingBlob->UseCount(), 0U);

    metaMng->Stop();
}

TEST_F(TestMmcMetaManager, SsdAvailabilityFollowsStorageRegistration)
{
    MmcLocation loc{3U, MEDIA_DRAM};
    MmcLocalMemlInitInfo locInfo{100U, SIZE_32K};
    MmcRef<MmcMetaManager> metaMng = MmcMakeRef<MmcMetaManager>(2000U, 70U, 60U);
    ASSERT_EQ(metaMng->Start(), MMC_OK);
    EXPECT_FALSE(metaMng->IsSsdAvailable(loc.rank_));

    std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
    std::vector<MmcLocation> locs{loc};
    std::vector<MmcLocalMemlInitInfo> locInfos{locInfo};
    ASSERT_EQ(metaMng->Mount(locs, locInfos, blobMap, true), MMC_OK);
    EXPECT_TRUE(metaMng->IsSsdAvailable(loc.rank_));
    metaMng->SetSsdMetadataMode(MMC_SSD_METADATA_SEPARATED);
    // 存储能力来自 LocalService 注册，不随 SSD 元数据管理模式变化。
    EXPECT_TRUE(metaMng->IsSsdAvailable(loc.rank_));

    ASSERT_EQ(metaMng->Unmount(loc), MMC_OK);
    EXPECT_FALSE(metaMng->IsSsdAvailable(loc.rank_));
}

TEST_F(TestMmcMetaManager, GetRandomStorageRankUsesStorageRegistration)
{
    constexpr uint64_t defaultTtl = 2000U;
    constexpr uint16_t evictThresholdHigh = 70U;
    constexpr uint16_t evictThresholdLow = 60U;
    constexpr uint32_t requestRank = 3U;
    constexpr uint32_t anotherStorageRank = 5U;
    constexpr uint32_t allocatorOnlyRank = 7U;
    constexpr uint32_t unregisteredRank = 1U;
    MmcRef<MmcMetaManager> metaMng = MmcMakeRef<MmcMetaManager>(defaultTtl, evictThresholdHigh, evictThresholdLow);
    ASSERT_EQ(metaMng->Start(), MMC_OK);
    std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
    MmcLocalMemlInitInfo locInfo{100U, 1000U};
    MmcLocation requestLoc{requestRank, MEDIA_DRAM};
    MmcLocation anotherStorageLoc{anotherStorageRank, MEDIA_DRAM};
    MmcLocation allocatorOnlyLoc{allocatorOnlyRank, MEDIA_DRAM};
    uint32_t storageRank = UINT32_MAX;
    EXPECT_EQ(metaMng->GetRandomStorageRank(unregisteredRank, storageRank), MMC_INVALID_PARAM);
    metaMng->SetSsdMetadataMode(MMC_SSD_METADATA_SEPARATED);
    EXPECT_EQ(metaMng->GetRandomStorageRank(unregisteredRank, storageRank), MMC_SSD_NOT_AVAILABLE);

    std::vector<MmcLocation> allocatorOnlyLocs{allocatorOnlyLoc};
    std::vector<MmcLocalMemlInitInfo> allocatorOnlyInfos{locInfo};
    ASSERT_EQ(metaMng->Mount(allocatorOnlyLocs, allocatorOnlyInfos, blobMap, false), MMC_OK);
    EXPECT_EQ(metaMng->GetRandomStorageRank(allocatorOnlyRank, storageRank), MMC_SSD_NOT_AVAILABLE);

    std::vector<MmcLocation> requestLocs{requestLoc};
    std::vector<MmcLocalMemlInitInfo> requestInfos{locInfo};
    ASSERT_EQ(metaMng->Mount(requestLocs, requestInfos, blobMap, true), MMC_OK);
    storageRank = UINT32_MAX;
    ASSERT_EQ(metaMng->GetRandomStorageRank(requestRank, storageRank), MMC_OK);
    EXPECT_EQ(storageRank, requestRank);

    storageRank = UINT32_MAX;
    ASSERT_EQ(metaMng->GetRandomStorageRank(allocatorOnlyRank, storageRank), MMC_OK);
    EXPECT_EQ(storageRank, requestRank);

    std::vector<MmcLocation> anotherStorageLocs{anotherStorageLoc};
    std::vector<MmcLocalMemlInitInfo> anotherStorageInfos{locInfo};
    ASSERT_EQ(metaMng->Mount(anotherStorageLocs, anotherStorageInfos, blobMap, true), MMC_OK);
    storageRank = UINT32_MAX;
    ASSERT_EQ(metaMng->GetRandomStorageRank(anotherStorageRank, storageRank), MMC_OK);
    EXPECT_EQ(storageRank, anotherStorageRank);

    ASSERT_EQ(metaMng->Unmount(requestLoc), MMC_OK);
    storageRank = UINT32_MAX;
    ASSERT_EQ(metaMng->GetRandomStorageRank(unregisteredRank, storageRank), MMC_OK);
    EXPECT_EQ(storageRank, anotherStorageRank);

    ASSERT_EQ(metaMng->Unmount(anotherStorageLoc), MMC_OK);
    storageRank = UINT32_MAX;
    EXPECT_EQ(metaMng->GetRandomStorageRank(unregisteredRank, storageRank), MMC_SSD_NOT_AVAILABLE);
    EXPECT_EQ(storageRank, UINT32_MAX);
    ASSERT_EQ(metaMng->Unmount(allocatorOnlyLoc), MMC_OK);
}

TEST_F(TestMmcMetaManager, AllocAndFree)
{
    MmcLocation loc{0, MEDIA_DRAM};
    MmcLocalMemlInitInfo locInfo{0, 1000000};

    uint64_t defaultTtl = 2000;
    MmcRef<MmcMetaManager> metaMng = MmcMakeRef<MmcMetaManager>(defaultTtl, 70U, 60U);
    metaMng->Start();
    std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
    metaMng->Mount(loc, locInfo, blobMap);

    AllocOptions allocReq{SIZE_32K, 1, MEDIA_DRAM, {0}, 0}; // blobSize, numBlobs, mediaType, preferredRank, flags
    MmcMemMetaDesc objMeta;
    Result ret = metaMng->Alloc("test_string", allocReq, 1, 0, objMeta);
    ASSERT_TRUE(ret == MMC_OK);
    ASSERT_TRUE(objMeta.NumBlobs() == 1);
    ASSERT_TRUE(objMeta.Size() == SIZE_32K);

    metaMng->UpdateState("test_string", loc, MMC_WRITE_OK, 1);

    ret = metaMng->Remove("test_string");
    ASSERT_TRUE(ret == MMC_OK);
    metaMng->Stop();
}

TEST_F(TestMmcMetaManager, AllocUsesPerRequestLeaseTtl)
{
    MmcLocation loc{0, MEDIA_DRAM};
    MmcLocalMemlInitInfo locInfo{0, 1000000};

    const uint64_t defaultTtl = 2000;
    MmcRef<MmcMetaManager> metaMng = MmcMakeRef<MmcMetaManager>(defaultTtl, 70U, 60U);
    metaMng->Start();
    std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
    metaMng->Mount(loc, locInfo, blobMap);

    AllocOptions allocReq{SIZE_32K, 1, MEDIA_DRAM, {0}, 0};
    MmcMemMetaDesc objMetaDefault;
    ASSERT_EQ(metaMng->Alloc("lease_default", allocReq, 1, 0, objMetaDefault), MMC_OK);
    ASSERT_EQ(objMetaDefault.NumBlobs(), 1U);
    const uint64_t timeoutDefault = objMetaDefault.blobs_[0].leaseTimeoutTtlMs_;

    constexpr uint64_t perRequestLeaseTtl = 5000;
    MmcMemMetaDesc objMetaCustom;
    ASSERT_EQ(metaMng->Alloc("lease_custom", allocReq, 1, perRequestLeaseTtl, objMetaCustom), MMC_OK);
    ASSERT_EQ(objMetaCustom.NumBlobs(), 1U);
    const uint64_t timeoutCustom = objMetaCustom.blobs_[0].leaseTimeoutTtlMs_;

    // Per-request leaseTtlMs (5000) must yield a later lease timeout than the default
    // (2000). The two grants happen back-to-back, so the grant-time gap is negligible
    // and (timeoutCustom - timeoutDefault) reflects the 5000 vs 2000 difference.
    EXPECT_GT(timeoutCustom, timeoutDefault);
    EXPECT_GE(timeoutCustom - timeoutDefault, perRequestLeaseTtl - defaultTtl);

    metaMng->Stop();
}

TEST_F(TestMmcMetaManager, AllocAndFreeMulti)
{
    MmcLocation loc{0, MEDIA_DRAM};
    MmcLocalMemlInitInfo locInfo{0, 1000000};

    uint64_t defaultTtl = 200;
    MmcRef<MmcMetaManager> metaMng = MmcMakeRef<MmcMetaManager>(defaultTtl, 70U, 60U);
    metaMng->Start();
    std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
    metaMng->Mount(loc, locInfo, blobMap);

    uint16_t numKeys = 10U;
    std::vector<std::string> keys;
    std::vector<MmcMemMetaDesc> memMetaObjs;
    AllocOptions allocReq{SIZE_32K, 1, MEDIA_DRAM, {0}, 0};
    Result ret;
    for (int i = 0; i < numKeys; ++i) {
        MmcMemMetaDesc objMeta;
        string key = "testKey" + std::to_string(i);
        ret = metaMng->Alloc(key, allocReq, 1, 0, objMeta);
        memMetaObjs.push_back(objMeta);
        keys.push_back(key);
    }
    ASSERT_TRUE(ret == MMC_OK);
    ASSERT_TRUE(memMetaObjs[0].NumBlobs() == 1);
    ASSERT_TRUE(memMetaObjs[0].Size() == SIZE_32K);

    for (int i = 0; i < numKeys; ++i) {
        ret = metaMng->Remove(keys[i]);
    }
    metaMng->Stop();
}

TEST_F(TestMmcMetaManager, GetAndUpdate)
{
    // MmcMemPoolInitInfo poolInitInfo;
    MmcLocation loc{0, MEDIA_DRAM};
    MmcLocalMemlInitInfo locInfo{0, 1000000};
    // poolInitInfo[loc] = locInfo;
    uint64_t defaultTtl = 200;
    MmcRef<MmcMetaManager> metaMng = MmcMakeRef<MmcMetaManager>(defaultTtl, 70U, 60U);
    metaMng->Start();
    std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
    metaMng->Mount(loc, locInfo, blobMap);

    uint16_t numKeys = 20U;
    std::vector<std::string> keys;
    std::vector<MmcMemMetaDesc> memMetaObjs;
    AllocOptions allocReq{SIZE_32K, 1, MEDIA_DRAM, {0}, 0}; // blobSize, numBlobs, mediaType, preferredRank, flags
    Result ret;
    for (int i = 0; i < numKeys; ++i) {
        MmcMemMetaDesc objMeta;
        string key = "testKey" + std::to_string(i);
        ret = metaMng->Alloc(key, allocReq, 1, 0, objMeta);
        memMetaObjs.push_back(objMeta);
        keys.push_back(key);
    }
    ASSERT_TRUE(ret == MMC_OK);
    ASSERT_TRUE(memMetaObjs[0].NumBlobs() == 1);
    ASSERT_TRUE(memMetaObjs[0].Size() == SIZE_32K);

    ret = metaMng->UpdateState(keys[2], loc, MMC_WRITE_OK, 1);
    ASSERT_TRUE(ret == MMC_OK);

    MmcMemMetaDesc objMeta2;
    ret = metaMng->Get(keys[2U], 1, nullptr, objMeta2);
    ASSERT_TRUE(ret == MMC_OK);
    std::vector<MmcMemBlobDesc> blobs = objMeta2.blobs_;
    ASSERT_TRUE(blobs.size() == 1);
    metaMng->Stop();
}

TEST_F(TestMmcMetaManager, LRU)
{
    MmcLocation loc{0, MEDIA_DRAM};
    MmcLocalMemlInitInfo locInfo{0, 163840};
    uint64_t defaultTtl = 100;

    MmcRef<MmcMetaManager> metaMng = MmcMakeRef<MmcMetaManager>(defaultTtl, 70U, 50U);
    metaMng->Start();
    std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
    metaMng->Mount(loc, locInfo, blobMap);

    uint16_t numKeys = 8U;
    std::vector<std::string> keys;
    std::vector<MmcMemMetaDesc> memMetaObjs;
    AllocOptions allocReq{SIZE_32K, 1, MEDIA_DRAM, {0}, 0}; // blobSize, numBlobs, mediaType, preferredRank, flags
    Result ret;
    Result writeRet;

    std::vector<MmcMemMetaDesc> objMetas;
    for (int i = 0; i < numKeys; ++i) {
        MmcMemMetaDesc objMeta;
        string key = "testKey" + std::to_string(i);
        MetaNetServerPtr server;
        metaMng->CheckAndEvict(MEDIA_DRAM, SIZE_32K);
        usleep(1000 * 500);
        ret = metaMng->Alloc(key, allocReq, 1, 0, objMeta);
        ASSERT_TRUE(ret == MMC_OK);
        memMetaObjs.push_back(objMeta);
        keys.push_back(key);
        writeRet = metaMng->UpdateState(key, loc, MMC_WRITE_OK, 1);
        ASSERT_TRUE(writeRet == MMC_OK);
        objMetas.push_back(objMeta);
    }

    ASSERT_TRUE(metaMng->ExistKey(keys[0]) == MMC_UNMATCHED_KEY);
    ASSERT_TRUE(metaMng->ExistKey(keys[numKeys - 1]) == MMC_OK);
    ASSERT_TRUE(memMetaObjs[numKeys - 1].Size() == SIZE_32K);

    for (int i = 0; i < numKeys; ++i) {
        ret = metaMng->Remove(keys[i]);
    }
    ASSERT_TRUE(metaMng->ExistKey(keys[0]) == MMC_UNMATCHED_KEY);
    ASSERT_TRUE(metaMng->ExistKey(keys[numKeys - 1]) == MMC_UNMATCHED_KEY);
    metaMng->Stop();
}

TEST_F(TestMmcMetaManager, DefragEvictReleasesConsecutiveColdKeys)
{
    // 场景：k0 用 256K 大块、其余 59 个用 32K 顺序分配，抬升分配器 maxAlignedSize 为 256K
    // （即碎片判断的可用块阈值）；再每隔 3 个释放 1 个（k1/k4/.../k58 共 20 个），
    // 产生互不相邻的 32K 空洞，低于可用块阈值而计为碎片。剩余 40 key，LRU 尾部 20% = 8 个冷 key
    // （k0/k2/k3/k5/k6/k8/k9/k11），地址连续被空洞串联成同一窗口。
    // 5% 释放下限 = 2，选中窗口内所有连续冷 key 整体释放（8 个），非冷 key 不受影响
    constexpr int kTotalKeys = 60;
    constexpr int kHoleStart = 1; // 从 k1 开始每隔 3 个挖一个空洞
    constexpr int kHoleInterval = 3;
    // 容量按总分配量精确收紧，避免尾部大块空闲稀释碎片浪费率
    constexpr uint64_t kBigBlobSize = SIZE_32K * 8;
    constexpr uint64_t kDramCapacity = kBigBlobSize + (kTotalKeys - 1) * SIZE_32K;
    constexpr auto kWaitTimeout = std::chrono::seconds(5);

    MmcLocation loc{0, MEDIA_DRAM};
    MmcLocalMemlInitInfo locInfo{0, kDramCapacity};
    MmcRef<MmcMetaManager> metaMng = MmcMakeRef<MmcMetaManager>(2000, 70U, 60U);
    ASSERT_EQ(metaMng->Start(), MMC_OK);
    std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
    metaMng->Mount(loc, locInfo, blobMap, false);

    std::vector<std::string> keys;
    keys.reserve(kTotalKeys);
    AllocOptions allocReq{SIZE_32K, 1, MEDIA_DRAM, {0}, 0};
    for (int i = 0; i < kTotalKeys; ++i) {
        std::string key = "defragKey" + std::to_string(i);
        allocReq.blobSize_ = (i == 0) ? kBigBlobSize : SIZE_32K;
        MmcMemMetaDesc objMeta;
        ASSERT_EQ(metaMng->Alloc(key, allocReq, 1, 0, objMeta), MMC_OK);
        ASSERT_EQ(metaMng->UpdateState(key, loc, MMC_WRITE_OK, 1), MMC_OK);
        keys.push_back(key);
    }
    for (int i = kHoleStart; i < kTotalKeys; i += kHoleInterval) {
        ASSERT_EQ(metaMng->Remove(keys[i]), MMC_OK);
    }

    metaMng->DefragEvict();

    // 先等碎片整理主任务结束（此时释放任务已全部投递到线程池）
    const auto idleDeadline = std::chrono::steady_clock::now() + kWaitTimeout;
    while (!DefragIdle(metaMng) && std::chrono::steady_clock::now() < idleDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(DefragIdle(metaMng));

    // 再等异步释放生效：窗口内 8 个连续冷 key 全部释放（k0/k2/k3/k5/k6/k8/k9/k11）
    const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
    bool released = false;
    while (std::chrono::steady_clock::now() < deadline) {
        if (metaMng->ExistKey(keys[0]) != MMC_OK && metaMng->ExistKey(keys[2]) != MMC_OK &&
            metaMng->ExistKey(keys[3]) != MMC_OK && metaMng->ExistKey(keys[5]) != MMC_OK) {
            released = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(released);
    // 窗口内所有连续冷 key 均被释放
    EXPECT_EQ(metaMng->ExistKey(keys[3]), MMC_UNMATCHED_KEY);
    EXPECT_EQ(metaMng->ExistKey(keys[5]), MMC_UNMATCHED_KEY);
    // 非冷 key 不在选中窗口内，不受影响
    EXPECT_EQ(metaMng->ExistKey(keys[12]), MMC_OK);
    metaMng->Stop();
}

TEST_F(TestMmcMetaManager, DefragEvictKeepsSsdBlobOfDualMediaKey)
{
    // 场景：碎片整理选中"DRAM + SSD 共存"的 key（Get 回温后的稳态）时仅释放 DRAM blob，
    // SSD 持久副本与其容器条目保留（等价 MOVE_DOWN 完成后的降级驻留状态）；
    // 仅持有 DRAM blob 的 key 照常整 key 删除（仅持选中介质时先容器 Erase 再释放）
    constexpr int kTotalKeys = 60;
    constexpr int kHoleStart = 1; // 从 k1 开始每隔 3 个挖一个空洞
    constexpr int kHoleInterval = 3;
    constexpr int kDualMediaIdx = 0; // k0 附加 SSD 副本，模拟回温后共存稳态
    constexpr int kDramOnlyIdx = 2;  // k2 仅 DRAM，被整 key 删除
    // k0 用 256K 大块抬升分配器 maxAlignedSize（可用块阈值），使 32K 空洞计为碎片；
    // 容量按总分配量精确收紧，避免尾部大块空闲稀释碎片浪费率
    constexpr uint64_t kBigBlobSize = SIZE_32K * 8;
    constexpr uint64_t kDramCapacity = kBigBlobSize + (kTotalKeys - 1) * SIZE_32K;
    constexpr auto kWaitTimeout = std::chrono::seconds(5);

    MmcLocation loc{0, MEDIA_DRAM};
    MmcLocalMemlInitInfo locInfo{0, kDramCapacity};
    MmcRef<MmcMetaManager> metaMng = MmcMakeRef<MmcMetaManager>(2000, 70U, 60U);
    ASSERT_EQ(metaMng->Start(), MMC_OK);
    std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
    metaMng->Mount(loc, locInfo, blobMap, false);

    std::vector<std::string> keys;
    keys.reserve(kTotalKeys);
    AllocOptions allocReq{SIZE_32K, 1, MEDIA_DRAM, {0}, 0};
    for (int i = 0; i < kTotalKeys; ++i) {
        std::string key = "defragSsdKey" + std::to_string(i);
        allocReq.blobSize_ = (i == kDualMediaIdx) ? kBigBlobSize : SIZE_32K;
        MmcMemMetaDesc objMeta;
        ASSERT_EQ(metaMng->Alloc(key, allocReq, 1, 0, objMeta), MMC_OK);
        ASSERT_EQ(metaMng->UpdateState(key, loc, MMC_WRITE_OK, 1), MMC_OK);
        keys.push_back(key);
    }
    for (int i = kHoleStart; i < kTotalKeys; i += kHoleInterval) {
        ASSERT_EQ(metaMng->Remove(keys[i]), MMC_OK);
    }

    // k0 附加 SSD 持久副本（gva=0 为 SSD blob 惯例）
    MmcMemObjMetaPtr dualMeta;
    ASSERT_EQ(MetaContainer(metaMng)->Get(keys[kDualMediaIdx], dualMeta), MMC_OK);
    {
        std::lock_guard<std::mutex> guard(dualMeta->Mutex());
        MmcMemBlobPtr ssdBlob = MmcMakeRef<MmcMemBlob>(0, 0, kBigBlobSize, MEDIA_SSD, READABLE, kBigBlobSize);
        ASSERT_NE(ssdBlob, nullptr);
        ASSERT_EQ(dualMeta->AddBlob(ssdBlob), MMC_OK);
    }
    const size_t lruCountBefore = MetaContainer(metaMng)->GetKeyCount(MEDIA_DRAM);
    ASSERT_EQ(lruCountBefore, static_cast<size_t>(kTotalKeys - kTotalKeys / kHoleInterval));

    metaMng->DefragEvict();

    // 先等碎片整理主任务结束（此时释放任务已全部投递到线程池）
    const auto idleDeadline = std::chrono::steady_clock::now() + kWaitTimeout;
    while (!DefragIdle(metaMng) && std::chrono::steady_clock::now() < idleDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(DefragIdle(metaMng));

    // 仅 DRAM 的冷 key（k2 等）被整 key 删除，此处以 k2 为代表等待删除生效
    const auto removeDeadline = std::chrono::steady_clock::now() + kWaitTimeout;
    bool k2Removed = false;
    while (std::chrono::steady_clock::now() < removeDeadline) {
        if (metaMng->ExistKey(keys[kDramOnlyIdx]) != MMC_OK) {
            k2Removed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(k2Removed);

    // k0 仅释放 DRAM blob：容器条目保留，最终只剩 SSD 持久副本
    const auto ssdOnlyDeadline = std::chrono::steady_clock::now() + kWaitTimeout;
    bool ssdOnly = false;
    while (std::chrono::steady_clock::now() < ssdOnlyDeadline) {
        MmcMemObjMetaPtr objMeta;
        if (MetaContainer(metaMng)->Get(keys[kDualMediaIdx], objMeta) == MMC_OK) {
            std::lock_guard<std::mutex> guard(objMeta->Mutex());
            const auto blobs = objMeta->GetBlobs();
            if (blobs.size() == 1 && static_cast<MediaType>(blobs[0]->Type()) == MEDIA_SSD) {
                ssdOnly = true;
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(ssdOnly);

    // k0 已从 DRAM LRU 摘除（降级为 SSD 驻留，不在任何 LRU），窗口内其余 7 个仅 DRAM 冷 key 随容器删除
    EXPECT_EQ(MetaContainer(metaMng)->GetKeyCount(MEDIA_DRAM), lruCountBefore - 8);
    metaMng->Stop();
}

TEST_F(TestMmcMetaManager, DefragEvictSkipsWhenMaxContiguousFreeSufficient)
{
    // 场景：1.5G DRAM 容量仅分配 60 个 32K blob 并挖出 32K 空洞（多段空闲），
    // 但尾部空闲段约 1.5G，最大连续空闲块 ≥ 1000MB，大块 put 可直接满足，
    // 碎片判断跳过该 rank，原本会被优先释放的窗口内冷 key 均保留
    constexpr int kTotalKeys = 60;
    constexpr int kHoleStart = 1; // 从 k1 开始每隔 3 个挖一个空洞
    constexpr int kHoleInterval = 3;
    constexpr uint64_t kDramCapacity = 1500ULL * 1024 * 1024; // 1500MB，大于 1000MB 碎片判断阈值
    constexpr auto kWaitTimeout = std::chrono::seconds(5);

    MmcLocation loc{0, MEDIA_DRAM};
    MmcLocalMemlInitInfo locInfo{0, kDramCapacity};
    MmcRef<MmcMetaManager> metaMng = MmcMakeRef<MmcMetaManager>(2000, 70U, 60U);
    ASSERT_EQ(metaMng->Start(), MMC_OK);
    std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
    metaMng->Mount(loc, locInfo, blobMap, false);

    std::vector<std::string> keys;
    keys.reserve(kTotalKeys);
    AllocOptions allocReq{SIZE_32K, 1, MEDIA_DRAM, {0}, 0};
    for (int i = 0; i < kTotalKeys; ++i) {
        std::string key = "defragSkipKey" + std::to_string(i);
        MmcMemMetaDesc objMeta;
        ASSERT_EQ(metaMng->Alloc(key, allocReq, 1, 0, objMeta), MMC_OK);
        ASSERT_EQ(metaMng->UpdateState(key, loc, MMC_WRITE_OK, 1), MMC_OK);
        keys.push_back(key);
    }
    for (int i = kHoleStart; i < kTotalKeys; i += kHoleInterval) {
        ASSERT_EQ(metaMng->Remove(keys[i]), MMC_OK);
    }
    const size_t lruCountBefore = MetaContainer(metaMng)->GetKeyCount(MEDIA_DRAM);

    metaMng->DefragEvict();

    // 等碎片整理主任务结束后断言：最大连续空闲块 ≥ 1000MB，无 key 被释放
    const auto idleDeadline = std::chrono::steady_clock::now() + kWaitTimeout;
    while (!DefragIdle(metaMng) && std::chrono::steady_clock::now() < idleDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(DefragIdle(metaMng));

    EXPECT_EQ(MetaContainer(metaMng)->GetKeyCount(MEDIA_DRAM), lruCountBefore);
    // 有碎片（最大连续空闲块 < 1000MB）时会被优先释放（gva 最低）的 k0 与 k2 均保留
    EXPECT_EQ(metaMng->ExistKey(keys[0]), MMC_OK);
    EXPECT_EQ(metaMng->ExistKey(keys[2]), MMC_OK);
    EXPECT_EQ(metaMng->ExistKey(keys[3]), MMC_OK);
    metaMng->Stop();
}

TEST_F(TestMmcMetaManager, AllocAndExistKey)
{
    MmcLocation loc{0, MEDIA_DRAM};
    MmcLocalMemlInitInfo locInfo{0, 1000000};
    uint64_t defaultTtl = 2000;
    MmcRef<MmcMetaManager> metaMng = MmcMakeRef<MmcMetaManager>(defaultTtl, 70U, 60U);
    metaMng->Start();
    std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
    metaMng->Mount(loc, locInfo, blobMap);

    AllocOptions allocReq{SIZE_32K, 1, MEDIA_DRAM, {0}, 0};
    MmcMemMetaDesc objMeta;
    Result ret = metaMng->Alloc("test_string", allocReq, 1, 0, objMeta);
    metaMng->UpdateState("test_string", loc, MMC_WRITE_OK, 1);
    ASSERT_TRUE(ret == MMC_OK);
    ASSERT_TRUE(objMeta.NumBlobs() == 1);
    ASSERT_TRUE(objMeta.Size() == SIZE_32K);

    ASSERT_TRUE(metaMng->ExistKey("test_string") == MMC_OK);
    ASSERT_TRUE(metaMng->ExistKey("another_test_string") == MMC_UNMATCHED_KEY);
    metaMng->Stop();
}

TEST_F(TestMmcMetaManager, AllocAndBatchExistKey)
{
    MmcLocation loc{0, MEDIA_DRAM};
    MmcLocalMemlInitInfo locInfo{0, 1000000};
    uint64_t defaultTtl = 2000;
    MmcRef<MmcMetaManager> metaMng = MmcMakeRef<MmcMetaManager>(defaultTtl, 70U, 60U);
    metaMng->Start();
    std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
    metaMng->Mount(loc, locInfo, blobMap);

    uint16_t numKeys = 5U;
    std::vector<std::string> keys;
    std::vector<MmcMemMetaDesc> memMetaObjs;
    AllocOptions allocReq{SIZE_32K, 1, MEDIA_DRAM, {0}, 0}; // blobSize, numBlobs, mediaType, preferredRank, flags
    Result ret = MMC_ERROR;
    for (uint16_t i = 0U; i < numKeys; ++i) {
        MmcMemMetaDesc objMeta;
        string key = "testKey_" + std::to_string(i);
        ret = metaMng->Alloc(key, allocReq, 1, 0, objMeta);
        memMetaObjs.push_back(objMeta);
        keys.push_back(key);
        metaMng->UpdateState(key, loc, MMC_WRITE_OK, 1);
    }
    ASSERT_TRUE(ret == MMC_OK);
    ASSERT_TRUE(memMetaObjs[0].NumBlobs() == 1);
    ASSERT_TRUE(memMetaObjs[0].Size() == SIZE_32K);

    std::vector<std::string> allExistKeys;
    std::vector<std::string> partExistKeys;
    std::vector<std::string> allNotExistKeys;

    auto GetKeys = [](uint16_t start, uint16_t end, std::vector<std::string> &keys) {
        for (uint16_t i = start; i < end; ++i) {
            string key = "testKey_" + std::to_string(i);
            keys.push_back(key);
        }
    };
    GetKeys(0U, 5U, allExistKeys);
    GetKeys(2U, 7U, partExistKeys);
    GetKeys(5U, 10U, allNotExistKeys);

    auto CheckReturn = [&metaMng](const std::vector<std::string> &keys, const std::vector<Result> &targetResults) {
        std::vector<Result> results;
        for (auto &key : keys) {
            results.push_back(metaMng->ExistKey(key));
        }
        ASSERT_TRUE(results.size() == targetResults.size());
        for (size_t i = 0; i < results.size(); ++i) {
            ASSERT_TRUE(targetResults[i] == results[i]);
        }
    };
    CheckReturn(allExistKeys, std::vector<Result>(5, MMC_OK));
    CheckReturn(partExistKeys, {MMC_OK, MMC_OK, MMC_OK, MMC_UNMATCHED_KEY, MMC_UNMATCHED_KEY});
    CheckReturn(allNotExistKeys, std::vector<Result>(5, MMC_UNMATCHED_KEY));
    metaMng->Stop();
}

TEST_F(TestMmcMetaManager, BatchExist)
{
    MmcLocation loc{0, MEDIA_DRAM};
    MmcLocalMemlInitInfo locInfo{0, 1000000};
    uint64_t defaultTtl = 2000;
    MmcRef<MmcMetaManager> metaMng = MmcMakeRef<MmcMetaManager>(defaultTtl, 70U, 60U);
    metaMng->Start();
    std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
    metaMng->Mount(loc, locInfo, blobMap);

    AllocOptions allocReq{SIZE_32K, 1, MEDIA_DRAM, {0}, 0};
    for (uint16_t i = 0U; i < 5U; ++i) {
        string key = "batchExistKey_" + std::to_string(i);
        MmcMemMetaDesc objMeta;
        ASSERT_EQ(metaMng->Alloc(key, allocReq, 1, 0, objMeta), MMC_OK);
        metaMng->UpdateState(key, loc, MMC_WRITE_OK, 1);
    }

    std::vector<std::string> allExistKeys;
    std::vector<std::string> partExistKeys;
    for (uint16_t i = 0U; i < 5U; ++i) {
        allExistKeys.push_back("batchExistKey_" + std::to_string(i));
    }
    partExistKeys.push_back("batchExistKey_2");
    partExistKeys.push_back("batchExistKey_3");
    partExistKeys.push_back("missingKey_0");

    std::vector<Result> allExistResults;
    ASSERT_EQ(metaMng->BatchExist(allExistKeys, allExistResults), MMC_OK);
    ASSERT_TRUE(allExistResults == std::vector<Result>(allExistKeys.size(), MMC_OK));

    constexpr size_t partUnmatchedIdx = 2;
    std::vector<Result> partResults;
    ASSERT_EQ(metaMng->BatchExist(partExistKeys, partResults), MMC_OK);
    ASSERT_EQ(partResults.size(), 3U);
    EXPECT_EQ(partResults[0], MMC_OK);
    EXPECT_EQ(partResults[1], MMC_OK);
    EXPECT_EQ(partResults[partUnmatchedIdx], MMC_UNMATCHED_KEY);

    metaMng->Stop();
}

TEST_F(TestMmcMetaManager, Remove)
{
    MmcLocation loc{0, MEDIA_DRAM};
    MmcLocalMemlInitInfo locInfo{0, 1000000};
    uint64_t defaultTtl = 2000;
    MmcRef<MmcMetaManager> metaMng = MmcMakeRef<MmcMetaManager>(defaultTtl, 70U, 60U);
    metaMng->Start();
    std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
    metaMng->Mount(loc, locInfo, blobMap);

    AllocOptions allocReq{SIZE_32K, 1, MEDIA_DRAM, {0}, 0};
    MmcMemMetaDesc objMeta;
    Result ret = metaMng->Alloc("testKey", allocReq, 1, 0, objMeta);
    ASSERT_TRUE(ret == MMC_OK);

    // std::this_thread::sleep_for(std::chrono::milliseconds(3000));

    ret = metaMng->Remove("testKey");
    ASSERT_TRUE(ret == MMC_OK);

    ret = metaMng->Remove("nonexistentKey");
    ASSERT_TRUE(ret == MMC_UNMATCHED_KEY);

    ret = metaMng->Alloc("testKey2", allocReq, 1, 0, objMeta);
    ASSERT_TRUE(ret == MMC_OK);
    ret = metaMng->Remove("testKey2");
    ASSERT_TRUE(ret == MMC_OK);
    metaMng->Stop();
}

TEST_F(TestMmcMetaManager, Get_NotAllBlobsReady)
{
    MmcLocation loc{0, MEDIA_DRAM};
    MmcLocalMemlInitInfo locInfo{0, 1000000};
    uint64_t defaultTtl = 2000;
    MmcRef<MmcMetaManager> metaMng = MmcMakeRef<MmcMetaManager>(defaultTtl, 70U, 60U);
    metaMng->Start();
    std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
    metaMng->Mount(loc, locInfo, blobMap);

    AllocOptions allocReq{SIZE_32K, 1, MEDIA_DRAM, {0}, 0};
    MmcMemMetaDesc objMeta;
    Result ret = metaMng->Alloc("test_key", allocReq, 1, 0, objMeta);
    ASSERT_EQ(ret, MMC_OK);

    MmcMemMetaDesc resultMeta;
    ret = metaMng->Get("test_key", 1, nullptr, resultMeta);
    ASSERT_EQ(ret, MMC_OK);
    ASSERT_EQ(resultMeta.NumBlobs(), 0);

    metaMng->Remove("test_key");
    metaMng->Stop();
}

TEST_F(TestMmcMetaManager, Alloc_ThresholdEviction)
{
    MmcLocation loc{0, MEDIA_DRAM};
    MmcLocalMemlInitInfo locInfo{0, 96 * 1024};
    uint64_t defaultTtl = 2000;
    MmcRef<MmcMetaManager> metaMng = MmcMakeRef<MmcMetaManager>(defaultTtl, 70U, 60U);
    metaMng->Start();
    std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
    metaMng->Mount(loc, locInfo, blobMap);

    std::vector<std::string> keys = {"key1", "key2"};
    for (const auto &key : keys) {
        AllocOptions allocReq{SIZE_32K, 1, MEDIA_DRAM, {0}, 0};
        MmcMemMetaDesc objMeta;
        Result ret = metaMng->Alloc(key, allocReq, 1, 0, objMeta);
        ASSERT_EQ(ret, MMC_OK);
        metaMng->UpdateState(key, loc, MMC_WRITE_OK, 1);
    }

    MmcMemMetaDesc temp;
    metaMng->Get("key2", 1, nullptr, temp);

    AllocOptions allocReq{SIZE_32K, 1, MEDIA_DRAM, {0}, 0};
    MmcMemMetaDesc newObjMeta;
    Result ret = metaMng->Alloc("key3", allocReq, 1, 0, newObjMeta);
    ASSERT_EQ(ret, MMC_OK);

    ASSERT_EQ(metaMng->ExistKey("key1"), MMC_OK);
    ASSERT_EQ(metaMng->ExistKey("key2"), MMC_OK);

    metaMng->Remove("key2");
    metaMng->Remove("key3");
    metaMng->Stop();
}

// EvictCallBackFunction uses MoveDown(srcMediaType) — when no SSD is mounted,
// GetFreeSpace(MEDIA_SSD)=0, callback returns REMOVE, key is deleted from metaMap
TEST_F(TestMmcMetaManager, EvictCallback_NoSsd_GoesToRemove)
{
    MmcLocation dramLoc{0, MEDIA_DRAM};
    MmcLocalMemlInitInfo dramInfo{0, 96 * 1024};
    uint64_t operateId = 1;

    uint64_t defaultTtl = 2000;
    MmcRef<MmcMetaManager> metaMng = MmcMakeRef<MmcMetaManager>(defaultTtl, 70U, 50U);
    metaMng->Start();
    std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
    metaMng->Mount(dramLoc, dramInfo, blobMap);

    std::vector<std::string> keys = {"evict_key1", "evict_key2", "evict_key3"};
    for (size_t i = 0; i < keys.size(); ++i) {
        AllocOptions allocReq{SIZE_32K, 1, MEDIA_DRAM, {0}, 0};
        uint64_t operateId = 1;
        MmcMemMetaDesc objMeta;
        ASSERT_EQ(metaMng->Alloc(keys[i], allocReq, operateId, 0, objMeta), MMC_OK);
        ASSERT_EQ(metaMng->UpdateState(keys[i], dramLoc, MMC_WRITE_OK, operateId), MMC_OK);
    }
    // 访问 key2 使其成为 MRU，确保 key0 是 LRU 末端
    MmcMemMetaDesc temp;
    uint32_t keyId = 2;
    metaMng->Get(keys[keyId], operateId, nullptr, temp);

    // 触发 DRAM 淘汰：MoveDown(MEDIA_DRAM)=MEDIA_SSD，GetFreeSpace(SSD)=0
    // → 回调返回 REMOVE → EvictOneLeastRecentlyUsed 从 metaMap 删除 key
    metaMng->CheckAndEvict(MEDIA_DRAM, SIZE_32K);
    usleep(500000UL); // 等待异步淘汰完成

    EXPECT_EQ(metaMng->ExistKey(keys[0]), MMC_UNMATCHED_KEY);
    EXPECT_EQ(metaMng->ExistKey(keys[keyId]), MMC_OK);

    metaMng->Stop();
}

// A key holding only ALLOCATED blobs (write not finished / WRITE_OK lost) must still be
// evictable, otherwise the LRU list is stuck and no key can ever be evicted.
TEST_F(TestMmcMetaManager, EvictCallback_AllocatedOnly_GoesToRemove)
{
    MmcLocation dramLoc{0, MEDIA_DRAM};
    MmcLocalMemlInitInfo dramInfo{0, 96 * 1024};
    uint64_t defaultTtl = 200;
    MmcRef<MmcMetaManager> metaMng = MmcMakeRef<MmcMetaManager>(defaultTtl, 70U, 50U);
    metaMng->Start();
    std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
    metaMng->Mount(dramLoc, dramInfo, blobMap);
    uint64_t operateId = 1;
    constexpr size_t survivingKeyIdx = 2;

    std::vector<std::string> keys = {"alloc_only_key1", "alloc_only_key2", "alloc_only_key3"};
    for (size_t i = 0; i < keys.size(); ++i) {
        AllocOptions allocReq{SIZE_32K, 1, MEDIA_DRAM, {0}, 0};
        MmcMemMetaDesc objMeta;
        ASSERT_EQ(metaMng->Alloc(keys[i], allocReq, operateId, 0, objMeta), MMC_OK);
        // Intentionally skip UpdateState(WRITE_OK) so blobs stay in ALLOCATED state
    }

    ASSERT_EQ(metaMng->UpdateState(keys[survivingKeyIdx], dramLoc, MMC_WRITE_OK, operateId), MMC_OK);
    // Eviction removes ALLOCATED-only keys instead of blocking the LRU
    metaMng->CheckAndEvict(MEDIA_DRAM, SIZE_32K);
    usleep(500000UL); // wait for lease timeout + async eviction to finish

    EXPECT_EQ(metaMng->ExistKey(keys[0]), MMC_UNMATCHED_KEY);
    EXPECT_EQ(metaMng->ExistKey(keys[1]), MMC_UNMATCHED_KEY);
    EXPECT_EQ(metaMng->ExistKey(keys[survivingKeyIdx]), MMC_OK);

    metaMng->Stop();
}

// MoveDown is independent of blobs_ iteration order — verify with SSD Mount + Start/Stop
TEST_F(TestMmcMetaManager, EvictCallback_MoveDownIndependentOfBlobOrder)
{
    MmcLocation dramLoc{0, MEDIA_DRAM};
    MmcLocalMemlInitInfo dramInfo{0, 96 * 1024};
    MmcLocation ssdLoc{0, MEDIA_SSD};
    MmcLocalMemlInitInfo ssdInfo{0, 16 * 1024}; // SSD 空间不足一个 32K blob

    uint64_t defaultTtl = 2000;
    MmcRef<MmcMetaManager> metaMng = MmcMakeRef<MmcMetaManager>(defaultTtl, 70U, 50U);
    metaMng->Start();
    std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
    metaMng->Mount(dramLoc, dramInfo, blobMap);
    metaMng->Mount(ssdLoc, ssdInfo, blobMap);

    std::vector<std::string> keys = {"order_key1", "order_key2", "order_key3"};
    for (size_t i = 0; i < keys.size(); ++i) {
        AllocOptions allocReq{SIZE_32K, 1, MEDIA_DRAM, {0}, 0};
        MmcMemMetaDesc objMeta;
        ASSERT_EQ(metaMng->Alloc(keys[i], allocReq, 1, 0, objMeta), MMC_OK);
        ASSERT_EQ(metaMng->UpdateState(keys[i], dramLoc, MMC_WRITE_OK, 1), MMC_OK);
    }
    MmcMemMetaDesc temp;
    metaMng->Get(keys[2U], 1, nullptr, temp);

    // SSD 空间不足(32KB allocated = 32KB capacity) → 回调返回 REMOVE
    // 验证 srcMediaType 由调用方传入，而非依赖 blobs_ 迭代顺序
    metaMng->CheckAndEvict(MEDIA_DRAM, SIZE_32K);
    usleep(500000UL);

    EXPECT_EQ(metaMng->ExistKey(keys[0]), MMC_UNMATCHED_KEY);

    metaMng->Stop();
}

// RewarmBlob stub compile verification — construct + Start/Stop works
TEST_F(TestMmcMetaManager, RewarmBlob_StubCompiles)
{
    uint64_t defaultTtl = 2000;
    MmcRef<MmcMetaManager> metaMng = MmcMakeRef<MmcMetaManager>(defaultTtl, 70U, 60U);
    ASSERT_NE(metaMng, nullptr);
    metaMng->Start();
    // RewarmBlob is a reserved interface, current stub returns MMC_ERROR
    metaMng->Stop();
}

// DRAM→SSD eviction delegates SSD I/O to LocalService via RPC
// EvictCallBackFunction → MoveBlob → CopyBlob uses metaNetServer_ RPC instead of direct ubsIo
// Without MetaNetServer, MoveBlob/CopyBlob fails and falls back to Remove
TEST_F(TestMmcMetaManager, EvictCallback_SsdEvictionDelegatesViaRpc)
{
    MmcLocation dramLoc{0, MEDIA_DRAM};
    MmcLocalMemlInitInfo dramInfo{0, 96 * 1024};
    MmcLocation ssdLoc{0, MEDIA_SSD};
    MmcLocalMemlInitInfo ssdInfo{0, 64 * 1024}; // SSD 足以容纳一个 32K blob

    uint64_t defaultTtl = 2000;
    MmcRef<MmcMetaManager> metaMng = MmcMakeRef<MmcMetaManager>(defaultTtl, 70U, 50U);
    ASSERT_EQ(metaMng->Start(), MMC_OK);

    std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
    metaMng->Mount(dramLoc, dramInfo, blobMap);
    metaMng->Mount(ssdLoc, ssdInfo, blobMap);

    std::vector<std::string> keys = {"rpc_key1", "rpc_key2", "rpc_key3"};
    for (size_t i = 0; i < keys.size(); ++i) {
        AllocOptions allocReq{SIZE_32K, 1, MEDIA_DRAM, {0}, 0};
        MmcMemMetaDesc objMeta;
        ASSERT_EQ(metaMng->Alloc(keys[i], allocReq, 1, 0, objMeta), MMC_OK);
        ASSERT_EQ(metaMng->UpdateState(keys[i], dramLoc, MMC_WRITE_OK, 1), MMC_OK);
    }
    // 访问 key2 使其成为 MRU
    MmcMemMetaDesc temp;
    metaMng->Get(keys[2U], 1, nullptr, temp);

    // 触发 DRAM 淘汰: MoveDown(MEDIA_DRAM)=MEDIA_SSD, SSD 有空闲空间
    // MoveBlob → CopyBlob attempts RPC (metaNetServer_ is null)
    // → CopyBlob fails → Free SSD + Remove key
    metaMng->CheckAndEvict(MEDIA_DRAM, SIZE_32K);
    usleep(500000UL);

    // 由于没有 MetaNetServer (RPC 不可用)，淘汰的 key 会被 Remove
    EXPECT_EQ(metaMng->ExistKey(keys[0]), MMC_UNMATCHED_KEY);
    // key2 未被淘汰（MRU）
    EXPECT_EQ(metaMng->ExistKey(keys[2]), MMC_OK);

    metaMng->Stop();
}

// MetaManager no longer holds ubsIoProxy_ — SSD I/O (Exist/Put/Get/Delete)
// is delegated to LocalService via metaNetServer_ RPC
TEST_F(TestMmcMetaManager, MountSsdAndDram_NoUbsIoProxy)
{
    MmcLocation dramLoc{0, MEDIA_DRAM};
    MmcLocalMemlInitInfo dramInfo{0, 96 * 1024};
    MmcLocation ssdLoc{0, MEDIA_SSD};
    MmcLocalMemlInitInfo ssdInfo{0, 1024 * 1024 * 1024};

    uint64_t defaultTtl = 2000;
    MmcRef<MmcMetaManager> metaMng = MmcMakeRef<MmcMetaManager>(defaultTtl, 70U, 60U);
    ASSERT_NE(metaMng, nullptr);
    ASSERT_EQ(metaMng->Start(), MMC_OK);

    std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
    metaMng->Mount(dramLoc, dramInfo, blobMap);
    metaMng->Mount(ssdLoc, ssdInfo, blobMap);

    // MetaManager no longer holds ubsIoProxy_; SSD I/O is delegated via RPC to LocalService

    // 验证 GetAllSegmentInfo 返回包含 SSD 段
    nlohmann::json segments = metaMng->GetAllSegmentInfo();
    EXPECT_TRUE(segments.is_array());
    EXPECT_GE(segments.size(), 2u); // DRAM + SSD

    metaMng->Stop();
}

// Multi-media Remove — key with HBM + DRAM, Remove individually
// Verify: each media allocator capacity freed independently, no interference
TEST_F(TestMmcMetaManager, RemoveMixedMedia_FreesAllAllocators)
{
    MmcLocation hbmLoc{0, MEDIA_HBM};
    MmcLocalMemlInitInfo hbmInfo{0, 128U * 1024U};
    MmcLocation dramLoc{0, MEDIA_DRAM};
    MmcLocalMemlInitInfo dramInfo{0, 128U * 1024U};

    uint64_t defaultTtl = 2000U;
    MmcRef<MmcMetaManager> metaMng = MmcMakeRef<MmcMetaManager>(defaultTtl, 70U, 50U);
    ASSERT_EQ(metaMng->Start(), MMC_OK);

    std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
    metaMng->Mount(hbmLoc, hbmInfo, blobMap);
    metaMng->Mount(dramLoc, dramInfo, blobMap);

    // HBM 上直接分配一个 key
    std::string key = "mixed_key";
    AllocOptions allocReq{SIZE_32K, 1, MEDIA_HBM, {0}, 0};
    MmcMemMetaDesc objMeta;
    ASSERT_EQ(metaMng->Alloc(key, allocReq, 1, 0, objMeta), MMC_OK);
    ASSERT_EQ(metaMng->UpdateState(key, hbmLoc, MMC_WRITE_OK, 1), MMC_OK);

    // 记录 Remove 前各介质使用量
    auto getUsed = [&](const std::string &medium) -> uint64_t {
        for (const auto &s : metaMng->GetAllSegmentInfo()) {
            if (s["medium"] == medium)
                return s["allocatedSize"];
        }
        return 0;
    };
    EXPECT_GT(getUsed("HBM"), 0u);

    // Remove key (DetachBlobs+FinalizeBlobs 异步执行，需等待 threadPool)
    ASSERT_EQ(metaMng->Remove(key), MMC_OK);
    usleep(100000U);

    // 验证 HBM 用量归零
    EXPECT_EQ(getUsed("HBM"), 0u) << "HBM should be freed after Remove";

    metaMng->Stop();
}

// DRAM-only key Remove — key has only DRAM blob (no SSD), execute Remove
// Verify: SsdPreFree callback no-ops for non-SSD blobs, DRAM capacity freed normally
TEST_F(TestMmcMetaManager, DramOnlyRemove_SsdPreFreeNoop)
{
    MmcLocation dramLoc{0, MEDIA_DRAM};
    MmcLocalMemlInitInfo dramInfo{0, 128 * 1024};

    uint64_t defaultTtl = 2000;
    MmcRef<MmcMetaManager> metaMng = MmcMakeRef<MmcMetaManager>(defaultTtl, 70U, 50U);
    ASSERT_EQ(metaMng->Start(), MMC_OK);

    std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
    metaMng->Mount(dramLoc, dramInfo, blobMap);

    std::string key = "dram_only";
    AllocOptions allocReq{SIZE_32K, 1, MEDIA_DRAM, {0}, 0};
    MmcMemMetaDesc objMeta;
    ASSERT_EQ(metaMng->Alloc(key, allocReq, 1, 0, objMeta), MMC_OK);
    ASSERT_EQ(metaMng->UpdateState(key, dramLoc, MMC_WRITE_OK, 1), MMC_OK);

    auto segs = metaMng->GetAllSegmentInfo();
    uint64_t dramUsed = 0;
    for (const auto &s : segs) {
        if (s["medium"] == "DRAM")
            dramUsed = s["allocatedSize"];
    }
    EXPECT_GT(dramUsed, 0u);

    ASSERT_EQ(metaMng->Remove(key), MMC_OK);
    usleep(100000U);

    segs = metaMng->GetAllSegmentInfo();
    for (const auto &s : segs) {
        if (s["medium"] == "DRAM")
            EXPECT_EQ(s["allocatedSize"], 0u);
    }

    metaMng->Stop();
}

// Get hits DRAM blob — key already has DRAM(READABLE), Get returns directly
// Verify: no unnecessary SSD rewarm triggered, returns DRAM blob directly
TEST_F(TestMmcMetaManager, Get_DramHit_NoSsdRewarm)
{
    MmcLocation dramLoc{0, MEDIA_DRAM};
    MmcLocalMemlInitInfo dramInfo{0, 128 * 1024};

    uint64_t defaultTtl = 2000;
    MmcRef<MmcMetaManager> metaMng = MmcMakeRef<MmcMetaManager>(defaultTtl, 70U, 50U);
    ASSERT_EQ(metaMng->Start(), MMC_OK);

    std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
    metaMng->Mount(dramLoc, dramInfo, blobMap);
    // 不 Mount SSD — 也没有 SSD 回温的可能

    std::string key = "get_dram_key";
    AllocOptions allocReq{SIZE_32K, 1, MEDIA_DRAM, {0}, 0};
    MmcMemMetaDesc objMeta;
    ASSERT_EQ(metaMng->Alloc(key, allocReq, 1, 0, objMeta), MMC_OK);
    ASSERT_EQ(metaMng->UpdateState(key, dramLoc, MMC_WRITE_OK, 1), MMC_OK);

    MmcMemMetaDesc resultMeta;
    Result ret = metaMng->Get(key, 1, nullptr, resultMeta);
    EXPECT_EQ(ret, MMC_OK);
    EXPECT_EQ(resultMeta.NumBlobs(), 1u);

    // 返回的 blob 是 DRAM 类型
    EXPECT_TRUE(resultMeta.blobs_.size() > 0);
    if (!resultMeta.blobs_.empty()) {
        EXPECT_EQ(resultMeta.blobs_[0].mediaType_, MEDIA_DRAM);
    }

    metaMng->Stop();
}

// RewarmBlob no SSD blob — key has only DRAM blob (no SSD mounted), Get returns DRAM blob
// Verify: when key has no SSD blob, Get returns DRAM blob normally, no rewarm branch
TEST_F(TestMmcMetaManager, RewarmBlob_NoSsdBlob_ReturnsError)
{
    MmcLocation dramLoc{0, MEDIA_DRAM};
    MmcLocalMemlInitInfo dramInfo{0, 128 * 1024};

    uint64_t defaultTtl = 2000;
    MmcRef<MmcMetaManager> metaMng = MmcMakeRef<MmcMetaManager>(defaultTtl, 70U, 50U);
    ASSERT_EQ(metaMng->Start(), MMC_OK);

    std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
    metaMng->Mount(dramLoc, dramInfo, blobMap);
    // 不 Mount SSD

    // 在 DRAM 上分配 key（非 SSD）
    std::string key = "rewarm_nossd_key";
    AllocOptions allocReq{SIZE_32K, 1, MEDIA_DRAM, {0}, 0};
    MmcMemMetaDesc objMeta;
    ASSERT_EQ(metaMng->Alloc(key, allocReq, 1, 0, objMeta), MMC_OK);
    ASSERT_EQ(metaMng->UpdateState(key, dramLoc, MMC_WRITE_OK, 1), MMC_OK);

    // key 没有 SSD blob → Get 正常返回 DRAM blob，不触发回温
    MmcMemMetaDesc getMeta;
    MmcBlobFilterPtr filter = MmcMakeRef<MmcBlobFilter>(UINT32_MAX, MEDIA_NONE, READABLE);
    ASSERT_EQ(metaMng->Get(key, 1, filter, getMeta), MMC_OK);
    EXPECT_GT(getMeta.numBlobs_, 0);

    metaMng->Stop();
}
// RewarmBlob skips when DRAM already exists — objMeta already has READABLE DRAM blob
TEST_F(TestMmcMetaManager, GvaAlloc_PendingWriteCannotRead)
{
    MmcLocation loc{0, MEDIA_DRAM};
    MmcLocalMemlInitInfo locInfo{0, 1000000};
    uint64_t defaultTtl = 2000;
    uint64_t opId1 = 1;
    uint64_t opId2 = 2;
    auto metaMng = MmcMakeRef<MmcMetaManager>(defaultTtl, 70U, 60U);
    ASSERT_TRUE(metaMng != nullptr);
    ASSERT_EQ(metaMng->Start(), MMC_OK);
    std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
    ASSERT_EQ(metaMng->Mount(loc, locInfo, blobMap), MMC_OK);

    AllocOptions allocReq{SIZE_32K, 1, MEDIA_DRAM, {0}, ALLOC_FLAGS_GVA_MALLOC_MASK};

    MmcMemMetaDesc objMeta;
    Result ret = metaMng->Alloc("gva_key_1", allocReq, 1, 0, objMeta);
    ASSERT_EQ(ret, MMC_OK);
    ASSERT_EQ(objMeta.NumBlobs(), 1);

    MemObjQueryInfo queryInfo;
    ret = QueryKey(metaMng, "gva_key_1", opId2, queryInfo);
    ASSERT_EQ(ret, MMC_OK);
    ASSERT_TRUE(queryInfo.valid_);
    ASSERT_EQ(queryInfo.numBlobs_, 1);
    ASSERT_EQ(queryInfo.blobs_[0].state_, ALLOCATED);

    metaMng->Remove("gva_key_1");
    metaMng->Stop();
}

TEST_F(TestMmcMetaManager, GvaWriteOk_BecomesReadable)
{
    MmcLocation loc{0, MEDIA_DRAM};
    MmcLocalMemlInitInfo locInfo{0, 1000000};
    uint64_t defaultTtl = 3210;
    uint64_t opId1 = 1;
    uint64_t opId2 = 2;
    auto metaMng = MmcMakeRef<MmcMetaManager>(defaultTtl, 70U, 60U);
    ASSERT_TRUE(metaMng != nullptr);
    ASSERT_EQ(metaMng->Start(), MMC_OK);
    std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
    ASSERT_EQ(metaMng->Mount(loc, locInfo, blobMap), MMC_OK);

    AllocOptions allocReq{SIZE_32K, 1, MEDIA_DRAM, {0}, ALLOC_FLAGS_GVA_MALLOC_MASK};

    MmcMemMetaDesc objMeta;
    Result ret = metaMng->Alloc("gva_key_2", allocReq, opId1, 0, objMeta);
    ASSERT_EQ(ret, MMC_OK);
    ASSERT_EQ(objMeta.NumBlobs(), 1);

    const auto &blob = objMeta.blobs_[0];
    MmcLocation blobLoc{blob.rank_, static_cast<MediaType>(blob.mediaType_)};
    ret = metaMng->UpdateState("gva_key_2", blobLoc, MMC_WRITE_OK, opId1);
    ASSERT_EQ(ret, MMC_OK);

    bool matched = false;
    int loopTimes = 50;
    int64_t sleepMs = 20;
    for (int i = 0; i < loopTimes; ++i) {
        MemObjQueryInfo queryInfo;
        ret = metaMng->AddLease("gva_key_2", opId2, 0, queryInfo);
        if (ret == MMC_OK && queryInfo.valid_ && queryInfo.numBlobs_ == 1 &&
            queryInfo.blobs_[0].leaseTimeoutTtlMs_ > 0) {
            ASSERT_EQ(queryInfo.blobs_[0].state_, READABLE);
            ASSERT_LE(queryInfo.blobs_[0].leaseTimeoutTtlMs_, defaultTtl);
            ASSERT_EQ(metaMng->RemoveLease("gva_key_2", opId2), MMC_OK);
            matched = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
    }
    ASSERT_TRUE(matched);

    metaMng->Remove("gva_key_2");
    metaMng->Stop();
}

TEST_F(TestMmcMetaManager, AddRemoveLease_WorksForRegularReadableSingleBlob)
{
    MmcLocation loc{0, MEDIA_DRAM};
    MmcLocalMemlInitInfo locInfo{0, 1000000};
    uint64_t defaultTtl = 3210;
    uint64_t opId1 = 1;
    uint64_t opId2 = 2;
    auto metaMng = MmcMakeRef<MmcMetaManager>(defaultTtl, 70U, 60U);
    ASSERT_TRUE(metaMng != nullptr);
    ASSERT_EQ(metaMng->Start(), MMC_OK);
    std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
    ASSERT_EQ(metaMng->Mount(loc, locInfo, blobMap), MMC_OK);

    AllocOptions allocReq{SIZE_32K, 1, MEDIA_DRAM, {0}, 0};

    MmcMemMetaDesc objMeta;
    Result ret = metaMng->Alloc("regular_key_1", allocReq, opId1, 0, objMeta);
    ASSERT_EQ(ret, MMC_OK);
    ASSERT_EQ(objMeta.NumBlobs(), 1);
    ASSERT_EQ(metaMng->UpdateState("regular_key_1", loc, MMC_WRITE_OK, opId1), MMC_OK);

    MemObjQueryInfo queryInfo;
    ret = metaMng->AddLease("regular_key_1", opId2, 0, queryInfo);
    ASSERT_EQ(ret, MMC_OK);
    ASSERT_TRUE(queryInfo.valid_);
    ASSERT_EQ(queryInfo.numBlobs_, 1);
    ASSERT_EQ(queryInfo.blobs_[0].state_, READABLE);
    ASSERT_GT(queryInfo.blobs_[0].leaseTimeoutTtlMs_, 0);
    ASSERT_LE(queryInfo.blobs_[0].leaseTimeoutTtlMs_, defaultTtl);
    ASSERT_EQ(metaMng->RemoveLease("regular_key_1", opId2), MMC_OK);

    metaMng->Remove("regular_key_1");
    metaMng->Stop();
}

TEST_F(TestMmcMetaManager, GvaRemoveAfterReadable_CleansIndex)
{
    MmcLocation loc{0, MEDIA_DRAM};
    MmcLocalMemlInitInfo locInfo{0, 1000000};
    uint64_t defaultTtl = 2000;
    uint64_t opId1 = 1;
    uint64_t opId2 = 2;
    auto metaMng = MmcMakeRef<MmcMetaManager>(defaultTtl, 70U, 60U);
    ASSERT_TRUE(metaMng != nullptr);
    ASSERT_EQ(metaMng->Start(), MMC_OK);
    std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
    ASSERT_EQ(metaMng->Mount(loc, locInfo, blobMap), MMC_OK);

    AllocOptions allocReq{SIZE_32K, 1, MEDIA_DRAM, {0}, ALLOC_FLAGS_GVA_MALLOC_MASK};

    MmcMemMetaDesc objMeta;
    Result ret = metaMng->Alloc("gva_key_4", allocReq, opId1, 0, objMeta);
    ASSERT_EQ(ret, MMC_OK);
    ASSERT_EQ(objMeta.NumBlobs(), 1);

    const auto &blob = objMeta.blobs_[0];
    MmcLocation blobLoc4{blob.rank_, static_cast<MediaType>(blob.mediaType_)};
    ASSERT_EQ(metaMng->UpdateState("gva_key_4", blobLoc4, MMC_WRITE_OK, opId1), MMC_OK);
    ASSERT_EQ(metaMng->Remove("gva_key_4"), MMC_OK);
    ASSERT_EQ(metaMng->ExistKey("gva_key_4"), MMC_UNMATCHED_KEY);

    bool matched = false;
    int loopTimes = 50;
    int64_t sleepMs = 20;
    for (int i = 0; i < loopTimes; ++i) {
        MemObjQueryInfo queryInfo;
        ret = QueryKey(metaMng, "gva_key_4", opId2, queryInfo);
        if (ret == MMC_UNMATCHED_KEY) {
            matched = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
    }
    ASSERT_TRUE(matched);

    metaMng->Stop();
}

TEST_F(TestMmcMetaManager, GvaWriteFail_RemovesKey)
{
    MmcLocation loc{0, MEDIA_DRAM};
    MmcLocalMemlInitInfo locInfo{0, 1000000};
    uint64_t defaultTtl = 2000;
    uint64_t opId1 = 1;
    uint64_t opId2 = 2;
    auto metaMng = MmcMakeRef<MmcMetaManager>(defaultTtl, 70U, 60U);
    ASSERT_TRUE(metaMng != nullptr);
    ASSERT_EQ(metaMng->Start(), MMC_OK);
    std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
    ASSERT_EQ(metaMng->Mount(loc, locInfo, blobMap), MMC_OK);

    AllocOptions allocReq{SIZE_32K, 1, MEDIA_DRAM, {0}, ALLOC_FLAGS_GVA_MALLOC_MASK};

    MmcMemMetaDesc objMeta;
    Result ret = metaMng->Alloc("gva_key_5", allocReq, opId1, 0, objMeta);
    ASSERT_EQ(ret, MMC_OK);
    ASSERT_EQ(objMeta.NumBlobs(), 1);

    const auto &blob = objMeta.blobs_[0];
    MmcLocation blobLoc5{blob.rank_, static_cast<MediaType>(blob.mediaType_)};
    ret = metaMng->UpdateState("gva_key_5", blobLoc5, MMC_WRITE_FAIL, opId1);
    ASSERT_EQ(ret, MMC_OK);
    ASSERT_EQ(metaMng->ExistKey("gva_key_5"), MMC_UNMATCHED_KEY);

    bool matched = false;
    int loopTimes = 50;
    int64_t sleepMs = 20;
    for (int i = 0; i < loopTimes; ++i) {
        MemObjQueryInfo queryInfo;
        ret = QueryKey(metaMng, "gva_key_5", opId2, queryInfo);
        if (ret == MMC_UNMATCHED_KEY) {
            matched = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
    }
    ASSERT_TRUE(matched);

    metaMng->Stop();
}

TEST_F(TestMmcMetaManager, GvaUnmount_CleansSegmentIndex)
{
    MmcLocation loc{0, MEDIA_DRAM};
    MmcLocalMemlInitInfo locInfo{0, 1000000};
    uint64_t defaultTtl = 2000;
    uint64_t opId1 = 1;
    uint64_t opId2 = 2;
    auto metaMng = MmcMakeRef<MmcMetaManager>(defaultTtl, 70U, 60U);
    ASSERT_TRUE(metaMng != nullptr);
    ASSERT_EQ(metaMng->Start(), MMC_OK);
    std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
    ASSERT_EQ(metaMng->Mount(loc, locInfo, blobMap), MMC_OK);

    AllocOptions allocReq{SIZE_32K, 1, MEDIA_DRAM, {0}, ALLOC_FLAGS_GVA_MALLOC_MASK};

    MmcMemMetaDesc objMeta;
    Result ret = metaMng->Alloc("gva_key_6", allocReq, opId1, 0, objMeta);
    ASSERT_EQ(ret, MMC_OK);
    ASSERT_EQ(objMeta.NumBlobs(), 1);

    const auto &blob = objMeta.blobs_[0];
    MmcLocation blobLoc6{blob.rank_, static_cast<MediaType>(blob.mediaType_)};
    ASSERT_EQ(metaMng->UpdateState("gva_key_6", blobLoc6, MMC_WRITE_OK, opId1), MMC_OK);

    ASSERT_EQ(metaMng->Unmount(loc), MMC_OK);

    MemObjQueryInfo queryInfo;
    ret = QueryKey(metaMng, "gva_key_6", opId2, queryInfo);
    ASSERT_EQ(ret, MMC_UNMATCHED_KEY);

    metaMng->Stop();
}

TEST_F(TestMmcMetaManager, UpdateConfigAppliesNewValues)
{
    constexpr uint64_t defaultTtl = 2000U;
    constexpr uint64_t newTtl = 5000U;
    constexpr uint16_t highWatermark = 70U;
    constexpr uint16_t lowWatermark = 60U;
    constexpr uint16_t newHighWatermark = 85U;
    constexpr uint16_t newLowWatermark = 75U;
    MmcRef<MmcMetaManager> metaMng = MmcMakeRef<MmcMetaManager>(defaultTtl, highWatermark, lowWatermark);

    ASSERT_EQ(metaMng->UpdateConfig(newTtl, newHighWatermark, newLowWatermark), MMC_OK);
    EXPECT_EQ(metaMng->Ttl(), newTtl);
    auto watermarks = metaMng->GetEvictWatermark();
    ASSERT_GT(watermarks.size(), static_cast<size_t>(MEDIA_DRAM));
    EXPECT_EQ(watermarks[MEDIA_DRAM].first, newHighWatermark);
    EXPECT_EQ(watermarks[MEDIA_DRAM].second, newLowWatermark);

    metaMng->Stop();
}

// PrefetchKeys with empty keys silently passes and returns MMC_OK
TEST_F(TestMmcMetaManager, PrefetchKeys_EmptyKeys_ReturnsOk)
{
    uint64_t defaultTtl = 2000;
    MmcRef<MmcMetaManager> metaMng = MmcMakeRef<MmcMetaManager>(defaultTtl, 70U, 60U);
    ASSERT_EQ(metaMng->Start(), MMC_OK);

    std::vector<std::string> emptyKeys;
    EXPECT_EQ(metaMng->PrefetchKeys(emptyKeys), MMC_OK);

    metaMng->Stop();
}

TEST_F(TestMmcMetaManager, GetByRankPrefetchDoesNotAddDramReadLease)
{
    auto metaMng = MmcMakeRef<MmcMetaManager>(2000U, 70U, 60U);
    ASSERT_EQ(metaMng->Start(), MMC_OK);

    const std::string key = "prefetch_without_read_lease";
    auto memObj = MmcMakeRef<MmcMemObjMeta>();
    auto blob = MmcMakeRef<MmcMemBlob>(0, 0x1000, SIZE_32K, MEDIA_DRAM, READABLE, SIZE_32K);
    ASSERT_EQ(memObj->AddBlob(blob), MMC_OK);
    ASSERT_EQ(MetaContainer(metaMng)->Insert(key, memObj), MMC_OK);

    std::vector<MmcMemMetaDesc> objMetas;
    ASSERT_EQ(metaMng->GetByRank({key}, GenerateOperateId(UINT32_MAX), objMetas, {}, false), MMC_OK);
    ASSERT_EQ(objMetas.size(), 1U);
    EXPECT_EQ(objMetas[0].numBlobs_, 1U);
    EXPECT_EQ(blob->UseCount(), 0U);

    const uint64_t getOperateId = GenerateOperateId(0);
    ASSERT_EQ(metaMng->GetByRank({key}, getOperateId, objMetas), MMC_OK);
    EXPECT_EQ(blob->UseCount(), 1U);
    EXPECT_EQ(metaMng->UpdateState(key, {0, MEDIA_DRAM}, MMC_READ_FINISH, getOperateId), MMC_OK);
    EXPECT_EQ(blob->UseCount(), 0U);

    metaMng->Stop();
}

// EvictKeys with empty keys returns MMC_INVALID_PARAM
TEST_F(TestMmcMetaManager, EvictKeys_EmptyKeys_ReturnsError)
{
    uint64_t defaultTtl = 2000;
    MmcRef<MmcMetaManager> metaMng = MmcMakeRef<MmcMetaManager>(defaultTtl, 70U, 60U);
    ASSERT_EQ(metaMng->Start(), MMC_OK);

    std::vector<std::string> emptyKeys;
    EXPECT_EQ(metaMng->EvictKeys(emptyKeys), MMC_INVALID_PARAM);

    metaMng->Stop();
}

TEST_F(TestMmcMetaManager, UpdateConfigRejectsInvalidWatermark)
{
    constexpr uint64_t defaultTtl = 2000U;
    constexpr uint16_t highWatermark = 70U;
    constexpr uint16_t lowWatermark = 60U;
    constexpr uint16_t invalidHighWatermark = 50U;
    constexpr uint16_t invalidLowWatermark = 60U;
    MmcRef<MmcMetaManager> metaMng = MmcMakeRef<MmcMetaManager>(defaultTtl, highWatermark, lowWatermark);

    // high <= low is rejected and keeps current values
    ASSERT_EQ(metaMng->UpdateConfig(defaultTtl, invalidHighWatermark, invalidLowWatermark), MMC_INVALID_PARAM);
    EXPECT_EQ(metaMng->Ttl(), defaultTtl);
    auto watermarks = metaMng->GetEvictWatermark();
    ASSERT_GT(watermarks.size(), static_cast<size_t>(MEDIA_DRAM));
    EXPECT_EQ(watermarks[MEDIA_DRAM].first, highWatermark);
    EXPECT_EQ(watermarks[MEDIA_DRAM].second, lowWatermark);

    // zero TTL falls back to MMC_DATA_TTL_MS, not an error
    ASSERT_EQ(metaMng->UpdateConfig(0, highWatermark, lowWatermark), MMC_OK);
    EXPECT_EQ(metaMng->Ttl(), MMC_DATA_TTL_MS);

    metaMng->Stop();
}

TEST_F(TestMmcMetaManager, UpdateConfigUnchangedKeepsValues)
{
    constexpr uint64_t defaultTtl = 2000U;
    constexpr uint16_t highWatermark = 70U;
    constexpr uint16_t lowWatermark = 60U;
    MmcRef<MmcMetaManager> metaMng = MmcMakeRef<MmcMetaManager>(defaultTtl, highWatermark, lowWatermark);

    ASSERT_EQ(metaMng->UpdateConfig(defaultTtl, highWatermark, lowWatermark), MMC_OK);
    EXPECT_EQ(metaMng->Ttl(), defaultTtl);
    auto watermarks = metaMng->GetEvictWatermark();
    ASSERT_GT(watermarks.size(), static_cast<size_t>(MEDIA_DRAM));
    EXPECT_EQ(watermarks[MEDIA_DRAM].first, highWatermark);
    EXPECT_EQ(watermarks[MEDIA_DRAM].second, lowWatermark);

    metaMng->Stop();
}

// PrefetchKeys with valid keys returns MMC_OK after synchronous prefetch
TEST_F(TestMmcMetaManager, PrefetchKeys_ValidKeys_ReturnsOk)
{
    MmcLocation dramLoc{0, MEDIA_DRAM};
    MmcLocalMemlInitInfo dramInfo{0, 128 * 1024};

    uint64_t defaultTtl = 2000;
    MmcRef<MmcMetaManager> metaMng = MmcMakeRef<MmcMetaManager>(defaultTtl, 70U, 60U);
    ASSERT_EQ(metaMng->Start(), MMC_OK);

    std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
    metaMng->Mount(dramLoc, dramInfo, blobMap);

    // Allocate a key in DRAM
    std::string key = "prefetch_test_key";
    AllocOptions allocReq{SIZE_32K, 1, MEDIA_DRAM, {0}, 0};
    MmcMemMetaDesc objMeta;
    ASSERT_EQ(metaMng->Alloc(key, allocReq, 1, 0, objMeta), MMC_OK);
    ASSERT_EQ(metaMng->UpdateState(key, dramLoc, MMC_WRITE_OK, 1), MMC_OK);

    // PrefetchKeys should return MMC_OK after synchronous prefetch
    std::vector<std::string> keys = {key};
    EXPECT_EQ(metaMng->PrefetchKeys(keys), MMC_OK);

    metaMng->Stop();
}

// EvictKeys with valid keys returns MMC_OK (async operation)
TEST_F(TestMmcMetaManager, EvictKeys_ValidKeys_ReturnsOk)
{
    MmcLocation dramLoc{0, MEDIA_DRAM};
    MmcLocalMemlInitInfo dramInfo{0, 128 * 1024};

    uint64_t defaultTtl = 2000;
    MmcRef<MmcMetaManager> metaMng = MmcMakeRef<MmcMetaManager>(defaultTtl, 70U, 60U);
    ASSERT_EQ(metaMng->Start(), MMC_OK);

    std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
    metaMng->Mount(dramLoc, dramInfo, blobMap);

    // Allocate a key in DRAM
    std::string key = "evict_test_key";
    AllocOptions allocReq{SIZE_32K, 1, MEDIA_DRAM, {0}, 0};
    MmcMemMetaDesc objMeta;
    ASSERT_EQ(metaMng->Alloc(key, allocReq, 1, 0, objMeta), MMC_OK);
    ASSERT_EQ(metaMng->UpdateState(key, dramLoc, MMC_WRITE_OK, 1), MMC_OK);

    // EvictKeys should return MMC_OK (async operation dispatched)
    std::vector<std::string> keys = {key};
    EXPECT_EQ(metaMng->EvictKeys(keys), MMC_OK);

    // Wait for async operation
    usleep(100000U);

    metaMng->Stop();
}

// PrefetchKey single-key variant works
TEST_F(TestMmcMetaManager, PrefetchKey_SingleKey_ReturnsOk)
{
    MmcLocation dramLoc{0, MEDIA_DRAM};
    MmcLocalMemlInitInfo dramInfo{0, 128 * 1024};

    uint64_t defaultTtl = 2000;
    MmcRef<MmcMetaManager> metaMng = MmcMakeRef<MmcMetaManager>(defaultTtl, 70U, 60U);
    ASSERT_EQ(metaMng->Start(), MMC_OK);

    std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
    metaMng->Mount(dramLoc, dramInfo, blobMap);

    std::string key = "prefetch_single_key";
    AllocOptions allocReq{SIZE_32K, 1, MEDIA_DRAM, {0}, 0};
    MmcMemMetaDesc objMeta;
    ASSERT_EQ(metaMng->Alloc(key, allocReq, 1, 0, objMeta), MMC_OK);
    ASSERT_EQ(metaMng->UpdateState(key, dramLoc, MMC_WRITE_OK, 1), MMC_OK);

    EXPECT_EQ(metaMng->PrefetchKey(key), MMC_OK);
    usleep(100000U);

    metaMng->Stop();
}

// EvictKey single-key variant works
TEST_F(TestMmcMetaManager, EvictKey_SingleKey_ReturnsOk)
{
    MmcLocation dramLoc{0, MEDIA_DRAM};
    MmcLocalMemlInitInfo dramInfo{0, 128 * 1024};

    uint64_t defaultTtl = 2000;
    MmcRef<MmcMetaManager> metaMng = MmcMakeRef<MmcMetaManager>(defaultTtl, 70U, 60U);
    ASSERT_EQ(metaMng->Start(), MMC_OK);

    std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
    metaMng->Mount(dramLoc, dramInfo, blobMap);

    std::string key = "evict_single_key";
    AllocOptions allocReq{SIZE_32K, 1, MEDIA_DRAM, {0}, 0};
    MmcMemMetaDesc objMeta;
    ASSERT_EQ(metaMng->Alloc(key, allocReq, 1, 0, objMeta), MMC_OK);
    ASSERT_EQ(metaMng->UpdateState(key, dramLoc, MMC_WRITE_OK, 1), MMC_OK);

    EXPECT_EQ(metaMng->EvictKey(key), MMC_OK);
    usleep(100000U);

    metaMng->Stop();
}

TEST_F(TestMmcMetaManager, BackupReadLeaseReleasesOriginalObjectAfterKeyRemovalOrRecreation)
{
    constexpr uint64_t leaseTtlMs = 10000;
    constexpr uint16_t evictHigh = 70;
    constexpr uint16_t evictLow = 60;
    constexpr uint64_t gva = 4096;
    constexpr uint64_t size = 4096;
    constexpr uint32_t clientRank = 1;
    constexpr uint32_t clientRequest = 1;
    constexpr uint32_t oneReader = 1;
    constexpr uint32_t twoReaders = 2;
    const std::string key = "backup_source";
    auto manager = MmcMakeRef<MmcMetaManager>(leaseTtlMs, evictHigh, evictLow);
    ASSERT_EQ(manager->Start(), MMC_OK);
    auto object = MmcMakeRef<MmcMemObjMeta>();
    auto blob = MmcMakeRef<MmcMemBlob>(0, gva, size, MEDIA_DRAM, READABLE, size);
    ASSERT_EQ(object->AddBlob(blob), MMC_OK);
    ASSERT_EQ(MetaContainer(manager)->Insert(key, object), MMC_OK);
    auto desc = blob->GetDesc();
    auto stale = desc;
    stale.gva_ += size;
    BackupReadLease invalidLease;
    EXPECT_NE(manager->AcquireBackupReadLease(key, stale, invalidLease), MMC_OK);
    EXPECT_NE(manager->AcquireBackupReadLease("missing", desc, invalidLease), MMC_OK);
    BackupReadLease firstLease;
    BackupReadLease secondLease;
    ASSERT_EQ(manager->AcquireBackupReadLease(key, desc, firstLease), MMC_OK);
    ASSERT_EQ(manager->AcquireBackupReadLease(key, desc, secondLease), MMC_OK);
    EXPECT_EQ(blob->UseCount(), twoReaders);

    ASSERT_EQ(MetaContainer(manager)->Erase(key), MMC_OK);
    manager->ReleaseBackupReadLease(firstLease);
    EXPECT_EQ(blob->UseCount(), oneReader);
    auto replacementObject = MmcMakeRef<MmcMemObjMeta>();
    auto replacement = MmcMakeRef<MmcMemBlob>(0, gva, size, MEDIA_DRAM, READABLE, size);
    ASSERT_EQ(replacementObject->AddBlob(replacement), MMC_OK);
    ASSERT_EQ(MetaContainer(manager)->Insert(key, replacementObject), MMC_OK);
    ASSERT_EQ(replacement->UpdateState(key, clientRank, clientRequest, MMC_READ_START), MMC_OK);
    manager->ReleaseBackupReadLease(secondLease);
    EXPECT_EQ(blob->UseCount(), 0U);
    EXPECT_EQ(replacement->UseCount(), oneReader);
    EXPECT_EQ(replacement->UpdateState(key, clientRank, clientRequest, MMC_READ_FINISH), MMC_OK);
    manager->Stop();
}

TEST_F(TestMmcMetaManager, BackupReadLeaseReleaseFindsMatchingLeaseAmongBlobs)
{
    constexpr uint64_t leaseTtlMs = 10000;
    constexpr uint16_t evictHigh = 70;
    constexpr uint16_t evictLow = 60;
    constexpr uint64_t gva = 4096;
    constexpr uint64_t size = 4096;
    constexpr uint32_t otherRank = 1;
    constexpr uint32_t oneReader = 1;
    const std::string key = "backup_multiple_blobs";
    auto manager = MmcMakeRef<MmcMetaManager>(leaseTtlMs, evictHigh, evictLow);
    ASSERT_EQ(manager->Start(), MMC_OK);
    auto object = MmcMakeRef<MmcMemObjMeta>();
    auto firstBlob = MmcMakeRef<MmcMemBlob>(0, gva, size, MEDIA_DRAM, READABLE, size);
    auto secondBlob = MmcMakeRef<MmcMemBlob>(otherRank, gva, size, MEDIA_DRAM, READABLE, size);
    ASSERT_EQ(object->AddBlob(firstBlob), MMC_OK);
    ASSERT_EQ(object->AddBlob(secondBlob), MMC_OK);
    ASSERT_EQ(MetaContainer(manager)->Insert(key, object), MMC_OK);
    BackupReadLease firstLease;
    BackupReadLease secondLease;
    ASSERT_EQ(manager->AcquireBackupReadLease(key, firstBlob->GetDesc(), firstLease), MMC_OK);
    ASSERT_EQ(manager->AcquireBackupReadLease(key, secondBlob->GetDesc(), secondLease), MMC_OK);

    manager->ReleaseBackupReadLease(secondLease);
    EXPECT_EQ(firstBlob->UseCount(), oneReader);
    EXPECT_EQ(secondBlob->UseCount(), 0U);
    manager->ReleaseBackupReadLease(secondLease);
    EXPECT_EQ(firstBlob->UseCount(), oneReader);
    manager->ReleaseBackupReadLease(firstLease);
    EXPECT_EQ(firstBlob->UseCount(), 0U);
    manager->Stop();
}

TEST_F(TestMmcMetaManager, BackupReadLeaseReleaseSkipsReusedBlobInOriginalObject)
{
    constexpr uint64_t leaseTtlMs = 50;
    constexpr uint16_t evictHigh = 70;
    constexpr uint16_t evictLow = 60;
    constexpr uint64_t gva = 4096;
    constexpr uint64_t size = 4096;
    constexpr uint32_t oneBlob = 1;
    const std::string key = "backup_reused_blob";
    auto manager = MmcMakeRef<MmcMetaManager>(leaseTtlMs, evictHigh, evictLow);
    ASSERT_EQ(manager->Start(), MMC_OK);
    std::vector<std::pair<std::string, MmcMemBlobDesc>> mounted;
    ASSERT_EQ(manager->Mount({0, MEDIA_DRAM}, {gva, size}, mounted), MMC_OK);
    AllocOptions options{size, oneBlob, MEDIA_DRAM, {0}, 0};
    std::vector<MmcMemBlobPtr> allocated;
    ASSERT_EQ(GlobalAllocator(manager)->Alloc(options, allocated), MMC_OK);
    auto original = allocated.front();
    original->SetDefaultLeaseTtlMs(leaseTtlMs);
    ASSERT_EQ(original->UpdateState(MMC_WRITE_OK), MMC_OK);
    auto object = MmcMakeRef<MmcMemObjMeta>();
    ASSERT_EQ(object->AddBlob(original), MMC_OK);
    ASSERT_EQ(MetaContainer(manager)->Insert(key, object), MMC_OK);
    BackupReadLease oldLease;
    ASSERT_EQ(manager->AcquireBackupReadLease(key, original->GetDesc(), oldLease), MMC_OK);
    {
        std::unique_lock<std::mutex> guard(object->Mutex());
        auto blobs = object->DetachBlobs(nullptr);
        guard.unlock();
        object->FinalizeBlobs(key, GlobalAllocator(manager), blobs, false);
        EXPECT_EQ(blobs.size(), oneBlob);
    }

    allocated.clear();
    ASSERT_EQ(GlobalAllocator(manager)->Alloc(options, allocated), MMC_OK);
    auto replacement = allocated.front();
    ASSERT_EQ(replacement->Gva(), original->Gva());
    ASSERT_EQ(replacement->UpdateState(MMC_WRITE_OK), MMC_OK);
    {
        std::lock_guard<std::mutex> guard(object->Mutex());
        ASSERT_EQ(object->AddBlob(replacement), MMC_OK);
    }
    BackupReadLease newLease;
    ASSERT_EQ(manager->AcquireBackupReadLease(key, replacement->GetDesc(), newLease), MMC_OK);
    ASSERT_NE(newLease.sequence, oldLease.sequence);

    manager->ReleaseBackupReadLease(oldLease);
    EXPECT_EQ(replacement->UseCount(), oneBlob);
    manager->ReleaseBackupReadLease(newLease);
    EXPECT_EQ(replacement->UseCount(), 0U);
    manager->Stop();
}

TEST_F(TestMmcMetaManager, BackupReadLeaseReleaseHandlesConcurrentRemoval)
{
    constexpr uint64_t leaseTtlMs = 50;
    constexpr uint16_t evictHigh = 70;
    constexpr uint16_t evictLow = 60;
    constexpr uint64_t gva = 4096;
    constexpr uint64_t size = 4096;
    constexpr uint32_t oneBlob = 1;
    const std::string key = "backup_removing";
    auto manager = MmcMakeRef<MmcMetaManager>(leaseTtlMs, evictHigh, evictLow);
    ASSERT_EQ(manager->Start(), MMC_OK);
    std::vector<std::pair<std::string, MmcMemBlobDesc>> mounted;
    ASSERT_EQ(manager->Mount({0, MEDIA_DRAM}, {gva, size}, mounted), MMC_OK);
    std::vector<MmcMemBlobPtr> allocated;
    ASSERT_EQ(GlobalAllocator(manager)->Alloc({size, oneBlob, MEDIA_DRAM, {0}, 0}, allocated), MMC_OK);
    auto blob = allocated.front();
    blob->SetDefaultLeaseTtlMs(leaseTtlMs);
    ASSERT_EQ(blob->UpdateState(MMC_WRITE_OK), MMC_OK);
    auto object = MmcMakeRef<MmcMemObjMeta>();
    ASSERT_EQ(object->AddBlob(blob), MMC_OK);
    ASSERT_EQ(MetaContainer(manager)->Insert(key, object), MMC_OK);
    BackupReadLease lease;
    ASSERT_EQ(manager->AcquireBackupReadLease(key, blob->GetDesc(), lease), MMC_OK);
    std::promise<void> removalLocked;
    auto removal = std::async(std::launch::async, [object, &removalLocked, &key, &manager] {
        std::unique_lock<std::mutex> guard(object->Mutex());
        auto blobs = object->DetachBlobs(nullptr);
        removalLocked.set_value();
        guard.unlock();
        object->FinalizeBlobs(key, GlobalAllocator(manager), blobs, false);
        return blobs.size();
    });
    removalLocked.get_future().wait();
    manager->ReleaseBackupReadLease(lease);
    EXPECT_EQ(removal.get(), oneBlob);
    EXPECT_EQ(blob->State(), REMOVING);
    EXPECT_EQ(GlobalAllocator(manager)->GetFreeSpace(MEDIA_DRAM), size);
    BackupReadLease removedLease;
    EXPECT_NE(manager->AcquireBackupReadLease(key, blob->GetDesc(), removedLease), MMC_OK);
    manager->Stop();
}

// PromoteKey on non-existent key returns MMC_UNMATCHED_KEY
TEST_F(TestMmcMetaManager, PromoteKey_NonExistentKey_ReturnsUnmatchedKey)
{
    constexpr uint64_t defaultTtl = 2000U;
    constexpr uint16_t evictHigh = 70U;
    constexpr uint16_t evictLow = 60U;
    MmcRef<MmcMetaManager> metaMng = MmcMakeRef<MmcMetaManager>(defaultTtl, evictHigh, evictLow);
    ASSERT_EQ(metaMng->Start(), MMC_OK);

    EXPECT_EQ(metaMng->PromoteKey("non_existent_key"), MMC_UNMATCHED_KEY);

    metaMng->Stop();
}

// PromoteKey on existing key returns MMC_OK
TEST_F(TestMmcMetaManager, PromoteKey_ExistingKey_ReturnsOk)
{
    constexpr uint64_t defaultTtl = 2000U;
    constexpr uint16_t evictHigh = 70U;
    constexpr uint16_t evictLow = 60U;
    constexpr uint64_t dramCapacity = 128 * 1024;
    constexpr uint64_t testOperateId = 1;
    MmcLocation dramLoc{0, MEDIA_DRAM};
    MmcLocalMemlInitInfo dramInfo{0, dramCapacity};

    MmcRef<MmcMetaManager> metaMng = MmcMakeRef<MmcMetaManager>(defaultTtl, evictHigh, evictLow);
    ASSERT_EQ(metaMng->Start(), MMC_OK);

    std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
    metaMng->Mount(dramLoc, dramInfo, blobMap);

    std::string key = "promote_test_key";
    AllocOptions allocReq{SIZE_32K, 1, MEDIA_DRAM, {0}, 0};
    MmcMemMetaDesc objMeta;
    ASSERT_EQ(metaMng->Alloc(key, allocReq, testOperateId, 0, objMeta), MMC_OK);
    ASSERT_EQ(metaMng->UpdateState(key, dramLoc, MMC_WRITE_OK, testOperateId), MMC_OK);

    EXPECT_EQ(metaMng->PromoteKey(key), MMC_OK);

    metaMng->Stop();
}

// Query with layer-wise flag skips non-READABLE blobs (ALLOCATED state)
TEST_F(TestMmcMetaManager, Query_WithLayerWiseFlag_SkipsNonReadableBlobs)
{
    constexpr uint64_t defaultTtl = 2000U;
    constexpr uint16_t evictHigh = 70U;
    constexpr uint16_t evictLow = 60U;
    constexpr uint64_t dramCapacity = 128 * 1024;
    constexpr uint64_t testOperateId = 1;
    MmcLocation dramLoc{0, MEDIA_DRAM};
    MmcLocalMemlInitInfo dramInfo{0, dramCapacity};

    MmcRef<MmcMetaManager> metaMng = MmcMakeRef<MmcMetaManager>(defaultTtl, evictHigh, evictLow);
    ASSERT_EQ(metaMng->Start(), MMC_OK);

    std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
    metaMng->Mount(dramLoc, dramInfo, blobMap);

    std::string key = "layer_wise_non_readable";
    AllocOptions allocReq{SIZE_32K, 1, MEDIA_DRAM, {0}, 0};
    MmcMemMetaDesc objMeta;
    ASSERT_EQ(metaMng->Alloc(key, allocReq, testOperateId, 0, objMeta), MMC_OK);

    MemObjQueryInfo queryInfo;
    EXPECT_EQ(metaMng->Query(key, testOperateId, GET_KEY_INFO_FOR_LAYER_WISE, queryInfo), MMC_OK);
    EXPECT_TRUE(queryInfo.blobs_.empty());
    EXPECT_FALSE(queryInfo.valid_);
    EXPECT_EQ(queryInfo.size_, 0U);

    metaMng->Stop();
}

// 内存碎片强制淘汰：直接从 LRU 尾部强制释放 5% 的 key，无需相邻空闲条件
// 布局：key_i 位于偏移 i*32K，Remove key_1 后在 [32K,64K) 留下空洞，
// LRU 尾部 key_0 被直接释放，释放后与空洞合并为 64K 连续块
TEST_F(TestMmcMetaManager, ForceEvict_ReleasesLruTailKey)
{
    MmcLocation loc{0, MEDIA_DRAM};
    MmcLocalMemlInitInfo locInfo{0, SIZE_32K * 11};
    MmcRef<MmcMetaManager> metaMng = MmcMakeRef<MmcMetaManager>(2000, 70U, 60U);
    ASSERT_EQ(metaMng->Start(), MMC_OK);
    std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
    ASSERT_EQ(metaMng->Mount(loc, locInfo, blobMap, false), MMC_OK);

    const int numKeys = 10;
    for (int i = 0; i < numKeys; ++i) {
        AllocOptions allocReq{SIZE_32K, 1, MEDIA_DRAM, {0}, 0};
        MmcMemMetaDesc objMeta;
        ASSERT_EQ(metaMng->Alloc("frag_key_" + std::to_string(i), allocReq, 1, 0, objMeta), MMC_OK);
        ASSERT_EQ(metaMng->UpdateState("frag_key_" + std::to_string(i), loc, MMC_WRITE_OK, 1), MMC_OK);
    }
    // 挖洞：key_1 位于 [32K,64K)，删除后在 key_0 上方留下空闲块
    ASSERT_EQ(metaMng->Remove("frag_key_1"), MMC_OK);
    usleep(200000U); // 等待异步释放完成，空洞就位

    // LRU 尾部即 frag_key_0，直接强制释放即完成 1 个 key 的目标（5% 向下取整为 1）；
    // 同步淘汰：返回后内存已归还分配器，无需等待
    EXPECT_EQ(TestMmcMetaManager::ForceEvictForFragmentation(metaMng, MEDIA_DRAM, {0U}, SIZE_32K * 2), 1U);

    EXPECT_EQ(metaMng->ExistKey("frag_key_0"), MMC_UNMATCHED_KEY);
    EXPECT_EQ(metaMng->ExistKey("frag_key_2"), MMC_OK);

    // key_0 的 blob 释放后与 [32K,64K) 空洞合并为 64K 连续空闲块，64K 分配应成功
    AllocOptions bigReq{SIZE_32K * 2, 1, MEDIA_DRAM, {0}, 0};
    MmcMemMetaDesc bigMeta;
    EXPECT_EQ(metaMng->Alloc("frag_big_key", bigReq, 1, 0, bigMeta), MMC_OK);

    metaMng->Stop();
}

// Query with layer-wise flag returns READABLE DRAM blobs
TEST_F(TestMmcMetaManager, Query_WithLayerWiseFlag_ReturnsReadableDramBlobs)
{
    constexpr uint64_t defaultTtl = 2000U;
    constexpr uint16_t evictHigh = 70U;
    constexpr uint16_t evictLow = 60U;
    constexpr uint64_t dramCapacity = 128 * 1024;
    constexpr uint64_t testOperateId = 1;
    MmcLocation dramLoc{0, MEDIA_DRAM};
    MmcLocalMemlInitInfo dramInfo{0, dramCapacity};

    MmcRef<MmcMetaManager> metaMng = MmcMakeRef<MmcMetaManager>(defaultTtl, evictHigh, evictLow);
    ASSERT_EQ(metaMng->Start(), MMC_OK);

    std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
    metaMng->Mount(dramLoc, dramInfo, blobMap);

    std::string key = "layer_wise_readable";
    AllocOptions allocReq{SIZE_32K, 1, MEDIA_DRAM, {0}, 0};
    MmcMemMetaDesc objMeta;
    ASSERT_EQ(metaMng->Alloc(key, allocReq, testOperateId, 0, objMeta), MMC_OK);

    MemObjQueryInfo queryInfoBefore;
    EXPECT_EQ(metaMng->Query(key, testOperateId, GET_KEY_INFO_FOR_LAYER_WISE, queryInfoBefore), MMC_OK);
    EXPECT_TRUE(queryInfoBefore.blobs_.empty());
    EXPECT_FALSE(queryInfoBefore.valid_);

    ASSERT_EQ(metaMng->UpdateState(key, dramLoc, MMC_WRITE_OK, testOperateId), MMC_OK);

    MemObjQueryInfo queryInfoAfter;
    EXPECT_EQ(metaMng->Query(key, testOperateId, GET_KEY_INFO_FOR_LAYER_WISE, queryInfoAfter), MMC_OK);
    EXPECT_EQ(queryInfoAfter.blobs_.size(), 1U);
    EXPECT_TRUE(queryInfoAfter.valid_);
    EXPECT_EQ(queryInfoAfter.size_, SIZE_32K);

    metaMng->Stop();
}

// 内存碎片强制淘汰：尾部 key 上下均被占用（无相邻空闲块）时同样直接释放
TEST_F(TestMmcMetaManager, ForceEvict_ReleasesTailWithoutAdjacentFree)
{
    MmcLocation loc{0, MEDIA_DRAM};
    MmcLocalMemlInitInfo locInfo{0, SIZE_32K * 11};
    MmcRef<MmcMetaManager> metaMng = MmcMakeRef<MmcMetaManager>(2000, 70U, 60U);
    ASSERT_EQ(metaMng->Start(), MMC_OK);
    std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
    ASSERT_EQ(metaMng->Mount(loc, locInfo, blobMap, false), MMC_OK);

    const int numKeys = 10;
    for (int i = 0; i < numKeys; ++i) {
        AllocOptions allocReq{SIZE_32K, 1, MEDIA_DRAM, {0}, 0};
        MmcMemMetaDesc objMeta;
        ASSERT_EQ(metaMng->Alloc("fill_key_" + std::to_string(i), allocReq, 1, 0, objMeta), MMC_OK);
        ASSERT_EQ(metaMng->UpdateState("fill_key_" + std::to_string(i), loc, MMC_WRITE_OK, 1), MMC_OK);
    }
    // 不挖洞：10 个 key 连续填充 [0,320K)，尾部 key_0 的上下邻居均被占用
    // 释放目标 = max(10 * 5%, 1) = 1，直接强制释放 LRU 尾部的 key_0
    EXPECT_EQ(TestMmcMetaManager::ForceEvictForFragmentation(metaMng, MEDIA_DRAM, {0U}, SIZE_32K * 2), 1U);

    EXPECT_EQ(metaMng->ExistKey("fill_key_0"), MMC_UNMATCHED_KEY);
    EXPECT_EQ(metaMng->ExistKey("fill_key_1"), MMC_OK);

    metaMng->Stop();
}

// Query without layer-wise flag returns all blobs regardless of state
TEST_F(TestMmcMetaManager, Query_WithoutLayerWiseFlag_ReturnsAllBlobs)
{
    constexpr uint64_t defaultTtl = 2000U;
    constexpr uint16_t evictHigh = 70U;
    constexpr uint16_t evictLow = 60U;
    constexpr uint64_t dramCapacity = 128 * 1024;
    constexpr uint64_t testOperateId = 1;
    MmcLocation dramLoc{0, MEDIA_DRAM};
    MmcLocalMemlInitInfo dramInfo{0, dramCapacity};

    MmcRef<MmcMetaManager> metaMng = MmcMakeRef<MmcMetaManager>(defaultTtl, evictHigh, evictLow);
    ASSERT_EQ(metaMng->Start(), MMC_OK);

    std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
    metaMng->Mount(dramLoc, dramInfo, blobMap);

    std::string key = "no_flag_alloc_only";
    AllocOptions allocReq{SIZE_32K, 1, MEDIA_DRAM, {0}, 0};
    MmcMemMetaDesc objMeta;
    ASSERT_EQ(metaMng->Alloc(key, allocReq, testOperateId, 0, objMeta), MMC_OK);

    MemObjQueryInfo queryInfo;
    EXPECT_EQ(metaMng->Query(key, testOperateId, 0, queryInfo), MMC_OK);
    EXPECT_EQ(queryInfo.blobs_.size(), 1U);
    EXPECT_TRUE(queryInfo.valid_);
    EXPECT_EQ(queryInfo.size_, SIZE_32K);

    metaMng->Stop();
}

// 有活跃租约的 key 不强制释放：跳过后向后延伸，释放下一个可释放的 key 补足 5% 目标
TEST_F(TestMmcMetaManager, ForceEvict_SkipsKeyWithActiveLease)
{
    MmcLocation loc{0, MEDIA_DRAM};
    MmcLocalMemlInitInfo locInfo{0, SIZE_32K * 11};
    MmcRef<MmcMetaManager> metaMng = MmcMakeRef<MmcMetaManager>(2000, 70U, 60U);
    ASSERT_EQ(metaMng->Start(), MMC_OK);
    std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
    ASSERT_EQ(metaMng->Mount(loc, locInfo, blobMap, false), MMC_OK);

    const int numKeys = 10;
    for (int i = 0; i < numKeys; ++i) {
        AllocOptions allocReq{SIZE_32K, 1, MEDIA_DRAM, {0}, 0};
        MmcMemMetaDesc objMeta;
        ASSERT_EQ(metaMng->Alloc("lease_key_" + std::to_string(i), allocReq, 1, 0, objMeta), MMC_OK);
        ASSERT_EQ(metaMng->UpdateState("lease_key_" + std::to_string(i), loc, MMC_WRITE_OK, 1), MMC_OK);
    }

    // LRU 尾部 key_0 持有读租约：被跳过不释放，向后延伸释放 key_1 补足 1 个 key 的目标
    MemObjQueryInfo queryInfo;
    ASSERT_EQ(metaMng->AddLease("lease_key_0", 2, 0, queryInfo), MMC_OK);
    EXPECT_EQ(TestMmcMetaManager::ForceEvictForFragmentation(metaMng, MEDIA_DRAM, {0U}, SIZE_32K * 2), 1U);
    // 有租约的 key_0 保留，延伸释放的是 key_1
    EXPECT_EQ(metaMng->ExistKey("lease_key_0"), MMC_OK);
    EXPECT_EQ(metaMng->ExistKey("lease_key_1"), MMC_UNMATCHED_KEY);

    metaMng->Stop();
}

// 延伸仅一次：候选 = 目标×2（目标 1 个时取尾部 2 个），候选内 key 均不可释放时
// 遍历完即结束，不继续向后延伸释放更深的 key。
// AddLease 会 Promote key 至 LRU 头部，须在加租约后将无租约 key 逐一 Promote 回头部，
// 使有租约的 ext_key_0/ext_key_1 滑回 LRU 尾部成为候选
TEST_F(TestMmcMetaManager, ForceEvict_ExtendsOnlyOnceWhenTailKeysHaveLease)
{
    MmcLocation loc{0, MEDIA_DRAM};
    MmcLocalMemlInitInfo locInfo{0, SIZE_32K * 11};
    MmcRef<MmcMetaManager> metaMng = MmcMakeRef<MmcMetaManager>(2000, 70U, 60U);
    ASSERT_EQ(metaMng->Start(), MMC_OK);
    std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
    ASSERT_EQ(metaMng->Mount(loc, locInfo, blobMap, false), MMC_OK);

    const int numKeys = 10;
    for (int i = 0; i < numKeys; ++i) {
        AllocOptions allocReq{SIZE_32K, 1, MEDIA_DRAM, {0}, 0};
        MmcMemMetaDesc objMeta;
        ASSERT_EQ(metaMng->Alloc("ext_key_" + std::to_string(i), allocReq, 1, 0, objMeta), MMC_OK);
        ASSERT_EQ(metaMng->UpdateState("ext_key_" + std::to_string(i), loc, MMC_WRITE_OK, 1), MMC_OK);
    }

    // AddLease 会 Promote key 至 LRU 头部：加完租约后把无租约的 key_2~key_9 逐一 ExistKey 提升回头部，
    // 使有活跃租约的 ext_key_0/ext_key_1 滑回 LRU 尾部成为候选
    MemObjQueryInfo queryInfo;
    ASSERT_EQ(metaMng->AddLease("ext_key_0", 2, 0, queryInfo), MMC_OK);
    ASSERT_EQ(metaMng->AddLease("ext_key_1", 3, 0, queryInfo), MMC_OK);
    for (int i = 2; i < numKeys; ++i) {
        ASSERT_EQ(metaMng->ExistKey("ext_key_" + std::to_string(i)), MMC_OK);
    }
    // 尾部 key_0/key_1 均持有读租约：候选（目标 1×2 = 2 个）全部不可释放，
    // 遍历完即结束，本次释放数为 0
    EXPECT_EQ(TestMmcMetaManager::ForceEvictForFragmentation(metaMng, MEDIA_DRAM, {0U}, SIZE_32K * 2), 0U);
    // 不继续延伸：候选之外的 key 保持不动
    EXPECT_EQ(metaMng->ExistKey("ext_key_0"), MMC_OK);
    EXPECT_EQ(metaMng->ExistKey("ext_key_1"), MMC_OK);
    EXPECT_EQ(metaMng->ExistKey("ext_key_2"), MMC_OK);

    metaMng->Stop();
}

// Query with layer-wise flag skips SSD blobs even when READABLE
TEST_F(TestMmcMetaManager, Query_WithLayerWiseFlag_SkipsSsdBlobs)
{
    constexpr uint64_t defaultTtl = 2000U;
    constexpr uint16_t evictHigh = 70U;
    constexpr uint16_t evictLow = 60U;
    constexpr uint64_t ssdCapacity = 128 * 1024;
    constexpr uint64_t testOperateId = 1;
    MmcLocation ssdLoc{0, MEDIA_SSD};
    MmcLocalMemlInitInfo ssdInfo{0, ssdCapacity};

    MmcRef<MmcMetaManager> metaMng = MmcMakeRef<MmcMetaManager>(defaultTtl, evictHigh, evictLow);
    ASSERT_EQ(metaMng->Start(), MMC_OK);

    std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
    metaMng->Mount(ssdLoc, ssdInfo, blobMap);

    std::string key = "layer_wise_ssd";
    AllocOptions allocReq{SIZE_32K, 1, MEDIA_SSD, {0}, 0};
    MmcMemMetaDesc objMeta;
    ASSERT_EQ(metaMng->Alloc(key, allocReq, testOperateId, 0, objMeta), MMC_OK);
    ASSERT_EQ(metaMng->UpdateState(key, ssdLoc, MMC_WRITE_OK, testOperateId), MMC_OK);

    MemObjQueryInfo queryFiltered;
    EXPECT_EQ(metaMng->Query(key, testOperateId, GET_KEY_INFO_FOR_LAYER_WISE, queryFiltered), MMC_OK);
    EXPECT_TRUE(queryFiltered.blobs_.empty());
    EXPECT_FALSE(queryFiltered.valid_);

    MemObjQueryInfo queryAll;
    EXPECT_EQ(metaMng->Query(key, testOperateId, 0, queryAll), MMC_OK);
    EXPECT_EQ(queryAll.blobs_.size(), 1U);
    EXPECT_TRUE(queryAll.valid_);

    metaMng->Stop();
}

// Alloc 因碎片失败（申请大小 > 最大连续空闲块）自动触发同步强制淘汰：
// 首次淘汰释放尾部 key_0（与空洞合并为 64K 仍不足）→ 循环再次淘汰释放 key_2
// （与 [0,64K) 合并为 96K）→ 本次 Alloc 调用内重试分配成功
TEST_F(TestMmcMetaManager, AllocFragmentationFailure_TriggerForceEvict)
{
    MmcLocation loc{0, MEDIA_DRAM};
    MmcLocalMemlInitInfo locInfo{0, SIZE_32K * 12};
    MmcRef<MmcMetaManager> metaMng = MmcMakeRef<MmcMetaManager>(2000, 70U, 60U);
    ASSERT_EQ(metaMng->Start(), MMC_OK);
    std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
    ASSERT_EQ(metaMng->Mount(loc, locInfo, blobMap, false), MMC_OK);

    const int numKeys = 10;
    for (int i = 0; i < numKeys; ++i) {
        AllocOptions allocReq{SIZE_32K, 1, MEDIA_DRAM, {0}, 0};
        MmcMemMetaDesc objMeta;
        ASSERT_EQ(metaMng->Alloc("trig_key_" + std::to_string(i), allocReq, 1, 0, objMeta), MMC_OK);
        ASSERT_EQ(metaMng->UpdateState("trig_key_" + std::to_string(i), loc, MMC_WRITE_OK, 1), MMC_OK);
    }
    // 挖洞 [32K,64K)：10 个 key 用 320K，尾部空闲 64K，最大连续空闲块 = 64K < 96K
    ASSERT_EQ(metaMng->Remove("trig_key_1"), MMC_OK);
    usleep(200000U); // 等待 Remove 异步释放完成，空洞就位

    AllocOptions bigReq{SIZE_32K * 3, 1, MEDIA_DRAM, {0}, 0};
    MmcMemMetaDesc bigMeta;
    // 首次分配失败触发同步强制淘汰，清理后在本次调用内重试成功
    EXPECT_EQ(metaMng->Alloc("trig_big_key", bigReq, 1, 0, bigMeta), MMC_OK);
    // 淘汰只从 LRU 尾部进行：非尾部 key 应保留
    EXPECT_EQ(metaMng->ExistKey("trig_key_9"), MMC_OK);
    // 尾部被淘汰的 key 已删除
    EXPECT_EQ(metaMng->ExistKey("trig_key_0"), MMC_UNMATCHED_KEY);
    EXPECT_EQ(metaMng->ExistKey("trig_key_2"), MMC_UNMATCHED_KEY);

    metaMng->Stop();
}

// 多 rank 场景：强制淘汰仅释放碎片化 rank 的 blob，不误删健康 rank 的数据。
// 布局：rank0 分配 10 个 32K key（rank0_0..rank0_9），rank1 分配 1 个 32K key（rank1_0，最先分配位于 LRU 尾部）。
// 挖洞：删除 rank0_1 在 rank0 上留下 [32K,64K) 空洞；rank0 最大连续空闲 = 32K < 64K。
// 调用 ForceEvictForFragmentation 限定 fragmentedRanks={0}：候选首位 rank1_0 无 rank0 blob → 跳过，
// 候选第二位 rank0_0 有 rank0 blob → 释放成功。rank1_0 保留不丢数据。
TEST_F(TestMmcMetaManager, ForceEvict_MultiRankOnlyReleasesFragmentedRank)
{
    MmcLocation loc0{0, MEDIA_DRAM};
    MmcLocation loc1{1, MEDIA_DRAM};
    MmcLocalMemlInitInfo locInfo0{0, SIZE_32K * 11};
    MmcLocalMemlInitInfo locInfo1{SIZE_32K * 11, SIZE_32K * 11};
    MmcRef<MmcMetaManager> metaMng = MmcMakeRef<MmcMetaManager>(2000, 70U, 60U);
    ASSERT_EQ(metaMng->Start(), MMC_OK);
    std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
    ASSERT_EQ(metaMng->Mount(loc0, locInfo0, blobMap, false), MMC_OK);
    ASSERT_EQ(metaMng->Mount(loc1, locInfo1, blobMap, false), MMC_OK);

    // rank1_0 最先分配 → 位于 LRU 尾部（最久未使用）
    AllocOptions rank1Req{SIZE_32K, 1, MEDIA_DRAM, {1}, 0};
    MmcMemMetaDesc rank1Meta;
    ASSERT_EQ(metaMng->Alloc("rank1_0", rank1Req, 1, 0, rank1Meta), MMC_OK);
    ASSERT_EQ(metaMng->UpdateState("rank1_0", loc1, MMC_WRITE_OK, 1), MMC_OK);

    // rank0_0..rank0_9 随后分配 → 位于 rank1_0 之前（靠近 LRU 头部）
    const int numRank0Keys = 10;
    for (int i = 0; i < numRank0Keys; ++i) {
        AllocOptions allocReq{SIZE_32K, 1, MEDIA_DRAM, {0}, 0};
        MmcMemMetaDesc objMeta;
        ASSERT_EQ(metaMng->Alloc("rank0_" + std::to_string(i), allocReq, 1, 0, objMeta), MMC_OK);
        ASSERT_EQ(metaMng->UpdateState("rank0_" + std::to_string(i), loc0, MMC_WRITE_OK, 1), MMC_OK);
    }
    // 挖洞：删除 rank0_1 在 rank0 上留下 [32K,64K) 空洞
    ASSERT_EQ(metaMng->Remove("rank0_1"), MMC_OK);
    usleep(200000U); // 等待异步释放完成，空洞就位

    // 限定 fragmentedRanks={0}：LRU 尾部候选 = [rank1_0, rank0_0]
    // rank1_0 无 rank0 blob → ForceReleaseKey 返回 false 跳过
    // rank0_0 有 rank0 blob → 释放成功，releasedCount=1 达到目标
    EXPECT_EQ(TestMmcMetaManager::ForceEvictForFragmentation(metaMng, MEDIA_DRAM, {0U}, SIZE_32K * 2), 1U);

    // rank1 的 key 未被淘汰：健康 rank 数据无丢失
    EXPECT_EQ(metaMng->ExistKey("rank1_0"), MMC_OK);
    // rank0 尾部 key 已释放
    EXPECT_EQ(metaMng->ExistKey("rank0_0"), MMC_UNMATCHED_KEY);
    // rank0 非尾部 key 保留
    EXPECT_EQ(metaMng->ExistKey("rank0_2"), MMC_OK);

    metaMng->Stop();
}

// 多副本多 rank 场景：key 在 rank0（碎片化）与 rank1（健康）各持有一个 DRAM blob，
// 强制淘汰仅释放 rank0 的 blob：key 条目保留（rank1 副本未丢）且回插 DRAM LRU 保持可淘汰。
TEST_F(TestMmcMetaManager, ForceEvict_MultiReplicaReleasesOnlyTargetRankBlob)
{
    MmcLocation loc0{0, MEDIA_DRAM};
    MmcLocation loc1{1, MEDIA_DRAM};
    MmcLocalMemlInitInfo locInfo0{0, SIZE_32K * 11};
    MmcLocalMemlInitInfo locInfo1{SIZE_32K * 11, SIZE_32K * 11};
    MmcRef<MmcMetaManager> metaMng = MmcMakeRef<MmcMetaManager>(2000, 70U, 60U);
    ASSERT_EQ(metaMng->Start(), MMC_OK);
    std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
    ASSERT_EQ(metaMng->Mount(loc0, locInfo0, blobMap, false), MMC_OK);
    ASSERT_EQ(metaMng->Mount(loc1, locInfo1, blobMap, false), MMC_OK);

    // 双副本：preferredRank={0} 在 rank0 分配 1 个，剩余 1 个副本经 RandomAssign 排除 rank0 后落到 rank1
    const std::string key = "replica_key";
    AllocOptions allocReq{SIZE_32K, 2, MEDIA_DRAM, {0}, 0};
    MmcMemMetaDesc objMeta;
    ASSERT_EQ(metaMng->Alloc(key, allocReq, 1, 0, objMeta), MMC_OK);
    ASSERT_EQ(metaMng->UpdateState(key, loc0, MMC_WRITE_OK, 1), MMC_OK);
    ASSERT_EQ(metaMng->UpdateState(key, loc1, MMC_WRITE_OK, 1), MMC_OK);

    // 限定 fragmentedRanks={0}：仅释放 rank0 的 blob，key 计入释放数
    EXPECT_EQ(TestMmcMetaManager::ForceEvictForFragmentation(metaMng, MEDIA_DRAM, {0U}, SIZE_32K * 2), 1U);

    // key 条目保留（rank1 副本未丢）且回插 DRAM LRU 保持可淘汰
    EXPECT_EQ(metaMng->ExistKey(key), MMC_OK);
    EXPECT_EQ(MetaContainer(metaMng)->GetKeyCount(MEDIA_DRAM), 1U);

    // rank0 blob 已释放，rank1 blob 保留
    MmcMemObjMetaPtr metaObj;
    ASSERT_EQ(MetaContainer(metaMng)->Get(key, metaObj), MMC_OK);
    MmcBlobFilterPtr rank0Filter = MmcMakeRef<MmcBlobFilter>(0, MEDIA_DRAM, NONE);
    MmcBlobFilterPtr rank1Filter = MmcMakeRef<MmcBlobFilter>(1, MEDIA_DRAM, NONE);
    ASSERT_NE(rank0Filter, nullptr);
    ASSERT_NE(rank1Filter, nullptr);
    {
        std::lock_guard<std::mutex> guard(metaObj->Mutex());
        EXPECT_EQ(metaObj->GetBlobs(rank0Filter).size(), 0U);
        EXPECT_EQ(metaObj->GetBlobs(rank1Filter).size(), 1U);
    }

    metaMng->Stop();
}

} // namespace mmc
} // namespace ock
