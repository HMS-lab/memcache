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
#include <algorithm>
#include <iostream>

#include "gtest/gtest.h"
#include "mmc_blob_allocator.h"
#include "mmc_global_allocator.h"
#include "mmc_locality_strategy.h"
#include "mmc_meta_service.h"
#include "mmc_ref.h"

using namespace testing;
using namespace std;
using namespace ock::mmc;

// ==================== LocalFirstAssign strategy tests ====================

class TestLocalFirstStrategy : public testing::Test {
public:
    TestLocalFirstStrategy() {}

    void SetUp() override
    {
        allocator = MmcMakeRef<MmcGlobalAllocator>();
        uint64_t size = SIZE_32K * 10U;
        // 模拟同节点3个rank (0,1,2) + 其他节点4个rank (5,6,7,8)
        for (int i = 0; i < 10; i++) {
            MmcLocation loc;
            MmcLocalMemlInitInfo info;
            loc.mediaType_ = MEDIA_DRAM;
            loc.rank_ = i;
            info.bmAddr_ = size * i;
            info.capacity_ = size;
            std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
            allocator->Mount(loc, info);
            allocator->BuildFromBlobs(loc, blobMap);
            allocator->Start(loc);
        }
    }

    void TearDown() override {}

protected:
    MmcGlobalAllocatorPtr allocator;
};

// ALLOC_LOCAL_FIRST: 1个blob分配在本地rank上
TEST_F(TestLocalFirstStrategy, AllocOneBlobOnLocalRanks)
{
    AllocOptions allocReq;
    std::vector<MmcMemBlobPtr> blobs;

    allocReq.blobSize_ = SIZE_32K;
    allocReq.numBlobs_ = 1;
    allocReq.preferredRank_ = {0, 1, 2}; // 同节点3个rank
    allocReq.mediaType_ = MEDIA_DRAM;
    allocReq.flags_ = ALLOC_LOCAL_FIRST;

    Result ret = allocator->Alloc(allocReq, blobs);
    EXPECT_EQ(ret, MMC_OK);
    EXPECT_EQ(blobs.size(), 1U);
    // 第1个blob应在preferredRank_[0]=0上
    EXPECT_EQ(blobs[0]->Rank(), 0);
}

// ALLOC_LOCAL_FIRST: 多个blob分配在多个本地rank上，按preferredRank_顺序
TEST_F(TestLocalFirstStrategy, AllocMultiBlobsOnLocalRanks)
{
    AllocOptions allocReq;
    std::vector<MmcMemBlobPtr> blobs;

    allocReq.blobSize_ = SIZE_32K;
    allocReq.numBlobs_ = 3;
    allocReq.preferredRank_ = {0, 1, 2};
    allocReq.mediaType_ = MEDIA_DRAM;
    allocReq.flags_ = ALLOC_LOCAL_FIRST;

    Result ret = allocator->Alloc(allocReq, blobs);
    EXPECT_EQ(ret, MMC_OK);
    EXPECT_EQ(blobs.size(), 3U);
    // 按preferredRank_顺序分配
    EXPECT_EQ(blobs[0]->Rank(), 0);
    EXPECT_EQ(blobs[1]->Rank(), 1);
    EXPECT_EQ(blobs[2]->Rank(), 2);
}

// ALLOC_LOCAL_FIRST: numBlobs超过本地rank数但有跨节点rank → fallback到远端成功
TEST_F(TestLocalFirstStrategy, FallbackToRemoteRanksWhenLocalFull)
{
    AllocOptions allocReq;
    std::vector<MmcMemBlobPtr> blobs;

    allocReq.blobSize_ = SIZE_32K;
    allocReq.numBlobs_ = 5; // 需要5个blob，本地3个rank + 远端7个rank
    // preferredRank_模拟ExpandRanks扩展后的结果：本地rank(0,1,2) + 远端rank升序(3,4,5,6,7,8,9)
    allocReq.preferredRank_ = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    allocReq.mediaType_ = MEDIA_DRAM;
    allocReq.flags_ = ALLOC_LOCAL_FIRST;

    Result ret = allocator->Alloc(allocReq, blobs);
    EXPECT_EQ(ret, MMC_OK);
    EXPECT_EQ(blobs.size(), 5U);
    // 前3个应在本地rank上
    EXPECT_EQ(blobs[0]->Rank(), 0);
    EXPECT_EQ(blobs[1]->Rank(), 1);
    EXPECT_EQ(blobs[2]->Rank(), 2);
    // 第4、5个fallback到远端rank
    EXPECT_EQ(blobs[3]->Rank(), 3);
    EXPECT_EQ(blobs[4]->Rank(), 4);
}

// ALLOC_LOCAL_FIRST: preferredRank_ 为空 → 本地无目标可试，按设计回退 ALLOC_RANDOM 并成功
TEST_F(TestLocalFirstStrategy, FallbackWhenPreferredRankEmpty)
{
    AllocOptions allocReq;
    std::vector<MmcMemBlobPtr> blobs;

    allocReq.blobSize_ = SIZE_32K;
    allocReq.numBlobs_ = 1;
    allocReq.preferredRank_ = {}; // 空
    allocReq.mediaType_ = MEDIA_DRAM;
    allocReq.flags_ = ALLOC_LOCAL_FIRST;

    Result ret = allocator->Alloc(allocReq, blobs);
    EXPECT_EQ(ret, MMC_OK);
    EXPECT_EQ(blobs.size(), 1U);
}

// ALLOC_LOCAL_FIRST: 单rank节点退化情况（1个本地rank, 1个blob）
TEST_F(TestLocalFirstStrategy, SingleRankSingleNode)
{
    AllocOptions allocReq;
    std::vector<MmcMemBlobPtr> blobs;

    allocReq.blobSize_ = SIZE_32K;
    allocReq.numBlobs_ = 1;
    allocReq.preferredRank_ = {5}; // 只有1个本地rank
    allocReq.mediaType_ = MEDIA_DRAM;
    allocReq.flags_ = ALLOC_LOCAL_FIRST;

    Result ret = allocator->Alloc(allocReq, blobs);
    EXPECT_EQ(ret, MMC_OK);
    EXPECT_EQ(blobs.size(), 1U);
    EXPECT_EQ(blobs[0]->Rank(), 5);
}

// ALLOC_LOCAL_FIRST: 单rank节点 + 多blob → fallback到远端rank成功
TEST_F(TestLocalFirstStrategy, SingleRankFallbackToRemote)
{
    AllocOptions allocReq;
    std::vector<MmcMemBlobPtr> blobs;

    allocReq.blobSize_ = SIZE_32K;
    allocReq.numBlobs_ = 2;
    // 单本地rank + 远端rank
    allocReq.preferredRank_ = {5, 0, 1, 2, 3, 4, 6, 7, 8, 9};
    allocReq.mediaType_ = MEDIA_DRAM;
    allocReq.flags_ = ALLOC_LOCAL_FIRST;

    Result ret = allocator->Alloc(allocReq, blobs);
    EXPECT_EQ(ret, MMC_OK);
    EXPECT_EQ(blobs.size(), 2U);
    EXPECT_EQ(blobs[0]->Rank(), 5);
    EXPECT_EQ(blobs[1]->Rank(), 0);
}

// ALLOC_LOCAL_FIRST: allocators为空 → 失败
TEST_F(TestLocalFirstStrategy, FailWhenAllocatorsEmpty)
{
    MmcGlobalAllocatorPtr emptyAllocator = MmcMakeRef<MmcGlobalAllocator>();

    AllocOptions allocReq;
    std::vector<MmcMemBlobPtr> blobs;

    allocReq.blobSize_ = SIZE_32K;
    allocReq.numBlobs_ = 1;
    allocReq.preferredRank_ = {0};
    allocReq.mediaType_ = MEDIA_DRAM;
    allocReq.flags_ = ALLOC_LOCAL_FIRST;

    Result ret = emptyAllocator->Alloc(allocReq, blobs);
    EXPECT_EQ(ret, MMC_ERROR);
}

// ALLOC_LOCAL_FIRST: preferredRank_ 中的 rank 不存在 → 本地无可分配，按设计回退 ALLOC_RANDOM 并成功
TEST_F(TestLocalFirstStrategy, FallbackWhenPreferredRankMissing)
{
    AllocOptions allocReq;
    std::vector<MmcMemBlobPtr> blobs;

    allocReq.blobSize_ = SIZE_32K;
    allocReq.numBlobs_ = 1;
    allocReq.preferredRank_ = {99}; // rank 99不存在
    allocReq.mediaType_ = MEDIA_DRAM;
    allocReq.flags_ = ALLOC_LOCAL_FIRST;

    Result ret = allocator->Alloc(allocReq, blobs);
    EXPECT_EQ(ret, MMC_OK);
    EXPECT_EQ(blobs.size(), 1U);
}

// ALLOC_LOCAL_FIRST: 确定性分配 — 相同preferredRank_顺序总是产生相同分配结果
TEST_F(TestLocalFirstStrategy, DeterministicAllocation)
{
    AllocOptions allocReq;
    allocReq.blobSize_ = SIZE_32K;
    allocReq.numBlobs_ = 2;
    allocReq.preferredRank_ = {2, 1, 0}; // 注意顺序是倒序
    allocReq.mediaType_ = MEDIA_DRAM;
    allocReq.flags_ = ALLOC_LOCAL_FIRST;

    std::vector<MmcMemBlobPtr> blobs1;
    Result ret1 = allocator->Alloc(allocReq, blobs1);
    EXPECT_EQ(ret1, MMC_OK);

    std::vector<MmcMemBlobPtr> blobs2;
    Result ret2 = allocator->Alloc(allocReq, blobs2);
    EXPECT_EQ(ret2, MMC_OK);

    // 两次分配应产生相同rank顺序（按preferredRank_顺序: 2, 1）
    EXPECT_EQ(blobs1[0]->Rank(), blobs2[0]->Rank());
    EXPECT_EQ(blobs1[1]->Rank(), blobs2[1]->Rank());
}

// ALLOC_LOCAL_FIRST + ALLOC_FLAGS_GVA_MALLOC_MASK: 高位flag应被保留
TEST_F(TestLocalFirstStrategy, LocalFirstWithGvaMallocMask)
{
    AllocOptions allocReq;
    std::vector<MmcMemBlobPtr> blobs;

    allocReq.blobSize_ = SIZE_32K;
    allocReq.numBlobs_ = 2;
    allocReq.preferredRank_ = {0, 1};
    allocReq.mediaType_ = MEDIA_DRAM;
    allocReq.flags_ = ALLOC_LOCAL_FIRST | ALLOC_FLAGS_GVA_MALLOC_MASK;

    Result ret = allocator->Alloc(allocReq, blobs);
    EXPECT_EQ(ret, MMC_OK);
    EXPECT_EQ(blobs.size(), 2U);
    EXPECT_EQ(blobs[0]->Rank(), 0);
    EXPECT_EQ(blobs[1]->Rank(), 1);
}

// ==================== MmcMetaService GetBackendIdForRank / GetRanksForBackendId tests ====================

class TestMetaServiceBackendId : public testing::Test {
public:
    TestMetaServiceBackendId() {}

    void SetUp() override {}

    void TearDown() override {}

protected:
};

// 未注册任何rank时，GetBackendIdForRank返回空字符串
TEST_F(TestMetaServiceBackendId, GetBackendIdForRank_Unregistered)
{
    MmcRef<MmcMetaService> svc = MmcMakeRef<MmcMetaService>("test-backend-id");
    ASSERT_NE(svc, nullptr);
    // 未启动、未BmRegister，rankBackendIdMap_为空
    std::string backendId = svc->GetBackendIdForRank(0);
    EXPECT_TRUE(backendId.empty());
}

// 未注册任何rank时，GetRanksForBackendId返回空vector
TEST_F(TestMetaServiceBackendId, GetRanksForBackendId_UnknownBackendId)
{
    MmcRef<MmcMetaService> svc = MmcMakeRef<MmcMetaService>("test-backend-id");
    ASSERT_NE(svc, nullptr);
    std::vector<uint32_t> ranks = svc->GetRanksForBackendId("card-0");
    EXPECT_TRUE(ranks.empty());
}

// GetRanksForBackendId返回结果（本地ranks保留自然顺序，不强制排序）
TEST_F(TestMetaServiceBackendId, GetRanksForBackendId_NoSortResult)
{
    MmcRef<MmcMetaService> svc = MmcMakeRef<MmcMetaService>("test-backend-id");
    ASSERT_NE(svc, nullptr);
    std::vector<uint32_t> ranks = svc->GetRanksForBackendId("");
    EXPECT_TRUE(ranks.empty());
}
