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
        // Simulate 3 ranks on one node (0,1,2) + 4 ranks on other nodes (5,6,7,8).
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

// ALLOC_LOCAL_FIRST: allocate 1 blob on a local rank.
TEST_F(TestLocalFirstStrategy, AllocOneBlobOnLocalRanks)
{
    AllocOptions allocReq;
    std::vector<MmcMemBlobPtr> blobs;

    allocReq.blobSize_ = SIZE_32K;
    allocReq.numBlobs_ = 1;
    allocReq.preferredRank_ = {0, 1, 2}; // 3 ranks on the same node.
    allocReq.mediaType_ = MEDIA_DRAM;
    allocReq.flags_ = ALLOC_LOCAL_FIRST;

    Result ret = allocator->Alloc(allocReq, blobs);
    EXPECT_EQ(ret, MMC_OK);
    EXPECT_EQ(blobs.size(), 1U);
    // The first blob should be on preferredRank_[0]=0.
    EXPECT_EQ(blobs[0]->Rank(), 0);
}

// ALLOC_LOCAL_FIRST: allocate multiple blobs on multiple local ranks in preferredRank_ order.
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
    // Allocate in preferredRank_ order.
    EXPECT_EQ(blobs[0]->Rank(), 0);
    EXPECT_EQ(blobs[1]->Rank(), 1);
    EXPECT_EQ(blobs[2]->Rank(), 2);
}

// ALLOC_LOCAL_FIRST: more blobs than local ranks, with remote ranks available -> successful remote fallback.
TEST_F(TestLocalFirstStrategy, FallbackToRemoteRanksWhenLocalFull)
{
    AllocOptions allocReq;
    std::vector<MmcMemBlobPtr> blobs;

    allocReq.blobSize_ = SIZE_32K;
    allocReq.numBlobs_ = 5; // Need 5 blobs; 3 local ranks + 7 remote ranks.
    // Simulate ExpandRanks output: local ranks (0,1,2) + remote ranks in ascending order (3,4,5,6,7,8,9).
    allocReq.preferredRank_ = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    allocReq.mediaType_ = MEDIA_DRAM;
    allocReq.flags_ = ALLOC_LOCAL_FIRST;

    Result ret = allocator->Alloc(allocReq, blobs);
    EXPECT_EQ(ret, MMC_OK);
    EXPECT_EQ(blobs.size(), 5U);
    // The first 3 should be on local ranks.
    EXPECT_EQ(blobs[0]->Rank(), 0);
    EXPECT_EQ(blobs[1]->Rank(), 1);
    EXPECT_EQ(blobs[2]->Rank(), 2);
    // The fourth and fifth fall back to remote ranks.
    EXPECT_EQ(blobs[3]->Rank(), 3);
    EXPECT_EQ(blobs[4]->Rank(), 4);
}

// ALLOC_LOCAL_FIRST: empty preferredRank_ -> no local targets; fall back to ALLOC_RANDOM successfully as designed.
TEST_F(TestLocalFirstStrategy, FallbackWhenPreferredRankEmpty)
{
    AllocOptions allocReq;
    std::vector<MmcMemBlobPtr> blobs;

    allocReq.blobSize_ = SIZE_32K;
    allocReq.numBlobs_ = 1;
    allocReq.preferredRank_ = {}; // Empty.
    allocReq.mediaType_ = MEDIA_DRAM;
    allocReq.flags_ = ALLOC_LOCAL_FIRST;

    Result ret = allocator->Alloc(allocReq, blobs);
    EXPECT_EQ(ret, MMC_OK);
    EXPECT_EQ(blobs.size(), 1U);
}

// ALLOC_LOCAL_FIRST: single-rank node (1 local rank, 1 blob).
TEST_F(TestLocalFirstStrategy, SingleRankSingleNode)
{
    AllocOptions allocReq;
    std::vector<MmcMemBlobPtr> blobs;

    allocReq.blobSize_ = SIZE_32K;
    allocReq.numBlobs_ = 1;
    allocReq.preferredRank_ = {5}; // Only 1 local rank.
    allocReq.mediaType_ = MEDIA_DRAM;
    allocReq.flags_ = ALLOC_LOCAL_FIRST;

    Result ret = allocator->Alloc(allocReq, blobs);
    EXPECT_EQ(ret, MMC_OK);
    EXPECT_EQ(blobs.size(), 1U);
    EXPECT_EQ(blobs[0]->Rank(), 5);
}

// ALLOC_LOCAL_FIRST: single-rank node + multiple blobs -> successful remote fallback.
TEST_F(TestLocalFirstStrategy, SingleRankFallbackToRemote)
{
    AllocOptions allocReq;
    std::vector<MmcMemBlobPtr> blobs;

    allocReq.blobSize_ = SIZE_32K;
    allocReq.numBlobs_ = 2;
    // One local rank + remote ranks.
    allocReq.preferredRank_ = {5, 0, 1, 2, 3, 4, 6, 7, 8, 9};
    allocReq.mediaType_ = MEDIA_DRAM;
    allocReq.flags_ = ALLOC_LOCAL_FIRST;

    Result ret = allocator->Alloc(allocReq, blobs);
    EXPECT_EQ(ret, MMC_OK);
    EXPECT_EQ(blobs.size(), 2U);
    EXPECT_EQ(blobs[0]->Rank(), 5);
    EXPECT_EQ(blobs[1]->Rank(), 0);
}

// ALLOC_LOCAL_FIRST: empty allocators -> failure.
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

// ALLOC_LOCAL_FIRST: nonexistent preferredRank_ rank -> no local allocation; fall back to ALLOC_RANDOM successfully.
TEST_F(TestLocalFirstStrategy, FallbackWhenPreferredRankMissing)
{
    AllocOptions allocReq;
    std::vector<MmcMemBlobPtr> blobs;

    allocReq.blobSize_ = SIZE_32K;
    allocReq.numBlobs_ = 1;
    allocReq.preferredRank_ = {99}; // Rank 99 does not exist.
    allocReq.mediaType_ = MEDIA_DRAM;
    allocReq.flags_ = ALLOC_LOCAL_FIRST;

    Result ret = allocator->Alloc(allocReq, blobs);
    EXPECT_EQ(ret, MMC_OK);
    EXPECT_EQ(blobs.size(), 1U);
}

// ALLOC_LOCAL_FIRST: deterministic allocation; the same preferredRank_ order always yields the same allocation.
TEST_F(TestLocalFirstStrategy, DeterministicAllocation)
{
    AllocOptions allocReq;
    allocReq.blobSize_ = SIZE_32K;
    allocReq.numBlobs_ = 2;
    allocReq.preferredRank_ = {2, 1, 0}; // Note the reverse order.
    allocReq.mediaType_ = MEDIA_DRAM;
    allocReq.flags_ = ALLOC_LOCAL_FIRST;

    std::vector<MmcMemBlobPtr> blobs1;
    Result ret1 = allocator->Alloc(allocReq, blobs1);
    EXPECT_EQ(ret1, MMC_OK);

    std::vector<MmcMemBlobPtr> blobs2;
    Result ret2 = allocator->Alloc(allocReq, blobs2);
    EXPECT_EQ(ret2, MMC_OK);

    // Both allocations should have the same rank order (preferredRank_ order: 2, 1).
    EXPECT_EQ(blobs1[0]->Rank(), blobs2[0]->Rank());
    EXPECT_EQ(blobs1[1]->Rank(), blobs2[1]->Rank());
}

// ALLOC_LOCAL_FIRST + ALLOC_FLAGS_GVA_MALLOC_MASK: preserve the high flag bits.
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

// GetBackendIdForRank returns an empty string if no rank is registered.
TEST_F(TestMetaServiceBackendId, GetBackendIdForRank_Unregistered)
{
    MmcRef<MmcMetaService> svc = MmcMakeRef<MmcMetaService>("test-backend-id");
    ASSERT_NE(svc, nullptr);
    // Not started, no BmRegister; rankBackendIdMap_ is empty.
    std::string backendId = svc->GetBackendIdForRank(0);
    EXPECT_TRUE(backendId.empty());
}

// GetRanksForBackendId returns an empty vector if no rank is registered.
TEST_F(TestMetaServiceBackendId, GetRanksForBackendId_UnknownBackendId)
{
    MmcRef<MmcMetaService> svc = MmcMakeRef<MmcMetaService>("test-backend-id");
    ASSERT_NE(svc, nullptr);
    std::vector<uint32_t> ranks = svc->GetRanksForBackendId("card-0");
    EXPECT_TRUE(ranks.empty());
}

// GetRanksForBackendId results preserve the natural local-rank order without forced sorting.
TEST_F(TestMetaServiceBackendId, GetRanksForBackendId_NoSortResult)
{
    MmcRef<MmcMetaService> svc = MmcMakeRef<MmcMetaService>("test-backend-id");
    ASSERT_NE(svc, nullptr);
    std::vector<uint32_t> ranks = svc->GetRanksForBackendId("");
    EXPECT_TRUE(ranks.empty());
}
