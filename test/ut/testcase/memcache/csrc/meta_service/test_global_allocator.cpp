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
#include "mmc_global_allocator.h"
#include "mmc_blob_allocator.h"
#include "mmc_ref.h"
#include "gtest/gtest.h"
#include <iostream>

using namespace testing;
using namespace std;
using namespace ock::mmc;

class TestMmcGlobalAllocator : public testing::Test {
public:
    TestMmcGlobalAllocator();

    void SetUp() override;

    void TearDown() override;

protected:
};
TestMmcGlobalAllocator::TestMmcGlobalAllocator() {}

void TestMmcGlobalAllocator::SetUp()
{
    cout << "this is Allocator TEST_F setup:";
}

void TestMmcGlobalAllocator::TearDown()
{
    cout << "this is Allocator TEST_F teardown";
}

TEST_F(TestMmcGlobalAllocator, AllocOne)
{
    MmcGlobalAllocatorPtr allocator = MmcMakeRef<MmcGlobalAllocator>();
    uint64_t size = SIZE_32K * 10U;
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
    for (int i = 0; i < 5; i++) {
        MmcLocation loc;
        MmcLocalMemlInitInfo info;
        loc.mediaType_ = MEDIA_HBM;
        loc.rank_ = i;
        info.bmAddr_ = size * i;
        info.capacity_ = size;
        std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
        allocator->Mount(loc, info);
        allocator->BuildFromBlobs(loc, blobMap);
        allocator->Start(loc);
    }

    AllocOptions allocReq;
    std::vector<MmcMemBlobPtr> blobs;

    allocReq.blobSize_ = SIZE_32K;
    allocReq.numBlobs_ = 1;
    allocReq.preferredRank_.push_back(5);
    allocReq.mediaType_ = MEDIA_DRAM;

    Result ret = allocator->Alloc(allocReq, blobs);
    EXPECT_EQ(ret, MMC_OK);
    EXPECT_EQ(blobs.size(), 1U);
    EXPECT_EQ(blobs[0]->Rank(), allocReq.preferredRank_.empty() ? 0 : allocReq.preferredRank_[0]);
    EXPECT_EQ(blobs[0]->Size(), allocReq.blobSize_);
    EXPECT_EQ(blobs[0]->Type(), allocReq.mediaType_);
    EXPECT_EQ(blobs[0]->Gva(), size * (allocReq.preferredRank_.empty() ? 0 : allocReq.preferredRank_[0]));
}

TEST_F(TestMmcGlobalAllocator, AllocMulti)
{
    MmcGlobalAllocatorPtr allocator = MmcMakeRef<MmcGlobalAllocator>();
    uint64_t size = SIZE_32K * 10U;
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
    for (int i = 0; i < 5; i++) {
        MmcLocation loc;
        MmcLocalMemlInitInfo info;
        loc.mediaType_ = MEDIA_HBM;
        loc.rank_ = i;
        info.bmAddr_ = size * i;
        info.capacity_ = size;
        std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
        allocator->Mount(loc, info);
        allocator->BuildFromBlobs(loc, blobMap);
        allocator->Start(loc);
    }

    AllocOptions allocReq;
    std::vector<MmcMemBlobPtr> blobs;

    allocReq.blobSize_ = SIZE_32K;
    allocReq.numBlobs_ = 10;
    allocReq.preferredRank_.push_back(5);
    allocReq.mediaType_ = MEDIA_DRAM;

    Result ret = allocator->Alloc(allocReq, blobs);
    EXPECT_EQ(ret, MMC_OK);
    EXPECT_EQ(blobs.size(), 10U);
    for (size_t i = 0; i < blobs.size(); i++) {
        cout << "blob[" << i << "] rank: " << blobs[i]->Rank() << endl;
        EXPECT_EQ(blobs[i]->Size(), allocReq.blobSize_);
        EXPECT_EQ(blobs[i]->Gva(), size * blobs[i]->Rank());
    }
}

TEST_F(TestMmcGlobalAllocator, AllocCrossRank)
{
    MmcGlobalAllocatorPtr allocator = MmcMakeRef<MmcGlobalAllocator>();
    uint64_t size = SIZE_32K * 10U;
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
    for (int i = 0; i < 5; i++) {
        MmcLocation loc;
        MmcLocalMemlInitInfo info;
        loc.mediaType_ = MEDIA_HBM;
        loc.rank_ = i;
        info.bmAddr_ = size * i;
        info.capacity_ = size;
        std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
        allocator->Mount(loc, info);
        allocator->BuildFromBlobs(loc, blobMap);
        allocator->Start(loc);
    }

    AllocOptions allocReq;
    std::vector<MmcMemBlobPtr> blobs;

    allocReq.blobSize_ = SIZE_32K;
    allocReq.numBlobs_ = 12;
    allocReq.preferredRank_.push_back(5);
    allocReq.mediaType_ = MEDIA_DRAM;

    Result ret = allocator->Alloc(allocReq, blobs);
    EXPECT_EQ(ret, -1);
}

TEST_F(TestMmcGlobalAllocator, FreeOne)
{
    MmcGlobalAllocatorPtr allocator = MmcMakeRef<MmcGlobalAllocator>();
    uint64_t size = SIZE_32K * 10U;
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
    for (int i = 0; i < 5; i++) {
        MmcLocation loc;
        MmcLocalMemlInitInfo info;
        loc.mediaType_ = MEDIA_HBM;
        loc.rank_ = i;
        info.bmAddr_ = size * i;
        info.capacity_ = size;
        std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
        allocator->Mount(loc, info);
        allocator->BuildFromBlobs(loc, blobMap);
        allocator->Start(loc);
    }

    AllocOptions allocReq;
    std::vector<MmcMemBlobPtr> blobs;

    allocReq.blobSize_ = SIZE_32K;
    allocReq.numBlobs_ = 1;
    allocReq.preferredRank_.push_back(5);
    allocReq.mediaType_ = MEDIA_DRAM;

    Result ret = allocator->Alloc(allocReq, blobs);
    EXPECT_EQ(ret, MMC_OK);
    EXPECT_EQ(blobs.size(), 1U);
    EXPECT_EQ(blobs[0]->Rank(), allocReq.preferredRank_.empty() ? 0 : allocReq.preferredRank_[0]);
    EXPECT_EQ(blobs[0]->Size(), allocReq.blobSize_);
    EXPECT_EQ(blobs[0]->Type(), allocReq.mediaType_);
    EXPECT_EQ(blobs[0]->Gva(), size * (allocReq.preferredRank_.empty() ? 0 : allocReq.preferredRank_[0]));

    ret = allocator->Free(blobs[0]);
    EXPECT_EQ(ret, MMC_OK);
    blobs.clear();

    ret = allocator->Alloc(allocReq, blobs);
    EXPECT_EQ(ret, MMC_OK);
    EXPECT_EQ(blobs.size(), 1U);
    EXPECT_EQ(blobs[0]->Rank(), allocReq.preferredRank_.empty() ? 0 : allocReq.preferredRank_[0]);
    EXPECT_EQ(blobs[0]->Size(), allocReq.blobSize_);
    EXPECT_EQ(blobs[0]->Type(), allocReq.mediaType_);
    EXPECT_EQ(blobs[0]->Gva(), size * (allocReq.preferredRank_.empty() ? 0 : allocReq.preferredRank_[0]));
}

TEST_F(TestMmcGlobalAllocator, FreeCrossRank)
{
    MmcGlobalAllocatorPtr allocator = MmcMakeRef<MmcGlobalAllocator>();
    uint64_t size = SIZE_32K * 10U;
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
    for (int i = 0; i < 5; i++) {
        MmcLocation loc;
        MmcLocalMemlInitInfo info;
        loc.mediaType_ = MEDIA_HBM;
        loc.rank_ = i;
        info.bmAddr_ = size * i;
        info.capacity_ = size;
        std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
        allocator->Mount(loc, info);
        allocator->BuildFromBlobs(loc, blobMap);
        allocator->Start(loc);
    }

    AllocOptions allocReq;
    std::vector<MmcMemBlobPtr> blobs;

    allocReq.blobSize_ = SIZE_32K;
    allocReq.numBlobs_ = 10;
    allocReq.preferredRank_.push_back(5);
    allocReq.mediaType_ = MEDIA_DRAM;

    Result ret = allocator->Alloc(allocReq, blobs);
    EXPECT_EQ(ret, MMC_OK);
    EXPECT_EQ(blobs.size(), allocReq.numBlobs_);
    for (int i = 0; i < 10; i++) {
        cout << "blob[" << i << "] rank: " << blobs[i]->Rank() << endl;
        EXPECT_EQ(blobs[i]->Size(), allocReq.blobSize_);
        EXPECT_EQ(blobs[i]->Gva(), size * blobs[i]->Rank());
    }

    ret = allocator->Free(blobs[3]);
    EXPECT_EQ(ret, MMC_OK);
    blobs.clear();

    allocReq.numBlobs_ = 1;
    ret = allocator->Alloc(allocReq, blobs);
    EXPECT_EQ(ret, MMC_OK);
    EXPECT_EQ(blobs.size(), 1U);
    EXPECT_EQ(blobs[0]->Rank(), allocReq.preferredRank_.empty() ? 0 : allocReq.preferredRank_[0]);
    EXPECT_EQ(blobs[0]->Size(), allocReq.blobSize_);
    EXPECT_EQ(blobs[0]->Type(), allocReq.mediaType_);
    EXPECT_EQ(blobs[0]->Gva(),
              size * (allocReq.preferredRank_.empty() ? 0 : allocReq.preferredRank_[0]) + 1 * allocReq.blobSize_);
}

TEST_F(TestMmcGlobalAllocator, MountUnmount)
{
    MmcGlobalAllocatorPtr allocator = MmcMakeRef<MmcGlobalAllocator>();
    uint64_t size = SIZE_32K * 10U;
    for (int i = 0; i < 10; i++) {
        if (i == 6) {
            continue;
        }
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
    for (int i = 0; i < 5; i++) {
        MmcLocation loc;
        MmcLocalMemlInitInfo info;
        loc.mediaType_ = MEDIA_HBM;
        loc.rank_ = i;
        info.bmAddr_ = size * i;
        info.capacity_ = size;
        std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
        allocator->Mount(loc, info);
        allocator->BuildFromBlobs(loc, blobMap);
        allocator->Start(loc);
    }

    AllocOptions allocReq;
    std::vector<MmcMemBlobPtr> blobs;

    allocReq.blobSize_ = SIZE_32K;
    allocReq.numBlobs_ = 9;
    allocReq.preferredRank_.push_back(5);
    allocReq.mediaType_ = MEDIA_DRAM;

    Result ret = allocator->Alloc(allocReq, blobs);
    EXPECT_EQ(ret, MMC_OK);
    EXPECT_EQ(blobs.size(), 9);

    ret = allocator->Free(blobs[3]);
    EXPECT_EQ(ret, MMC_OK);

    std::vector<MmcMemBlobPtr> blobs1;

    allocReq.numBlobs_ = 1;
    ret = allocator->Alloc(allocReq, blobs1);
    EXPECT_EQ(ret, MMC_OK);
    EXPECT_EQ(blobs1.size(), 1U);
    EXPECT_EQ(blobs1[0]->Rank(), allocReq.preferredRank_.empty() ? 0 : allocReq.preferredRank_[0]);
    EXPECT_EQ(blobs1[0]->Size(), allocReq.blobSize_);
    EXPECT_EQ(blobs1[0]->Type(), allocReq.mediaType_);
    EXPECT_EQ(blobs1[0]->Gva(),
              size * (allocReq.preferredRank_.empty() ? 0 : allocReq.preferredRank_[0]) + 1 * allocReq.blobSize_);

    MmcLocation loc;
    MmcLocalMemlInitInfo info;
    loc.mediaType_ = MEDIA_DRAM;
    loc.rank_ = 6;
    info.bmAddr_ = size * 6;
    info.capacity_ = size;
    std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
    ret = allocator->Mount(loc, info);
    EXPECT_EQ(ret, MMC_OK);
    ret = allocator->BuildFromBlobs(loc, blobMap);
    EXPECT_EQ(ret, MMC_OK);
    allocator->Start(loc);

    std::vector<MmcMemBlobPtr> blobs2;

    allocReq.numBlobs_ = 10;
    ret = allocator->Alloc(allocReq, blobs2);
    EXPECT_EQ(ret, MMC_OK);
    EXPECT_EQ(blobs2.size(), 10U);
    for (int i = 0; i < 10; i++) {
        cout << "blob[" << i << "] rank: " << blobs2[i]->Rank() << endl;
        EXPECT_EQ(blobs2[i]->Size(), allocReq.blobSize_);
    }
}

TEST_F(TestMmcGlobalAllocator, AllocWhenEmpty)
{
    MmcGlobalAllocatorPtr allocator = MmcMakeRef<MmcGlobalAllocator>();
    AllocOptions allocReq;
    std::vector<MmcMemBlobPtr> blobs;

    allocReq.blobSize_ = SIZE_32K;
    allocReq.numBlobs_ = 1;
    allocReq.preferredRank_.push_back(0);
    allocReq.mediaType_ = MEDIA_DRAM;

    Result ret = allocator->Alloc(allocReq, blobs);
    EXPECT_EQ(ret, MMC_ERROR);
    EXPECT_TRUE(blobs.empty());
}

TEST_F(TestMmcGlobalAllocator, MountDuplicateLocation)
{
    MmcGlobalAllocatorPtr allocator = MmcMakeRef<MmcGlobalAllocator>();
    uint64_t size = SIZE_32K * 10U;

    MmcLocation loc;
    MmcLocalMemlInitInfo info;
    loc.mediaType_ = MEDIA_DRAM;
    loc.rank_ = 0;
    info.capacity_ = size;
    std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
    Result ret = allocator->Mount(loc, info);
    EXPECT_EQ(ret, MMC_OK);
    ret = allocator->BuildFromBlobs(loc, blobMap);
    EXPECT_EQ(ret, MMC_OK);
    ret = allocator->Mount(loc, info);
    EXPECT_EQ(ret, MMC_OK);
}

TEST_F(TestMmcGlobalAllocator, UnmountInUseLocation)
{
    MmcGlobalAllocatorPtr allocator = MmcMakeRef<MmcGlobalAllocator>();
    uint64_t size = SIZE_32K * 10U;

    MmcLocation loc;
    loc.mediaType_ = MEDIA_DRAM;
    loc.rank_ = 0;
    MmcLocalMemlInitInfo info;
    info.capacity_ = size;
    std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
    allocator->Mount(loc, info);
    allocator->BuildFromBlobs(loc, blobMap);
    allocator->Start(loc);
    AllocOptions allocReq;
    std::vector<MmcMemBlobPtr> blobs;
    allocReq.blobSize_ = SIZE_32K;
    allocReq.numBlobs_ = 1;
    allocReq.preferredRank_.push_back(0);
    allocReq.mediaType_ = MEDIA_DRAM;
    Result ret = allocator->Alloc(allocReq, blobs);
    ASSERT_EQ(ret, MMC_OK);

    ret = allocator->Unmount(loc);
    EXPECT_EQ(ret, MMC_INVALID_PARAM);

    allocator->Free(blobs[0]);
    ret = allocator->Unmount(loc);
    EXPECT_EQ(ret, MMC_OK);
}

TEST_F(TestMmcGlobalAllocator, StopInvalidLocation)
{
    MmcGlobalAllocatorPtr allocator = MmcMakeRef<MmcGlobalAllocator>();
    MmcLocation loc;
    loc.mediaType_ = MEDIA_DRAM;
    loc.rank_ = 0;
    Result ret = allocator->Stop(loc);
    EXPECT_EQ(ret, MMC_INVALID_PARAM);
}

TEST_F(TestMmcGlobalAllocator, ReBuild)
{
    MmcGlobalAllocatorPtr allocator = MmcMakeRef<MmcGlobalAllocator>();
    uint64_t size = SIZE_32K * 10U;

    MmcLocation loc;
    MmcLocalMemlInitInfo info;
    loc.mediaType_ = MEDIA_DRAM;
    loc.rank_ = 0;
    info.bmAddr_ = 0;
    info.capacity_ = size;
    std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
    allocator->Mount(loc, info);
    allocator->BuildFromBlobs(loc, blobMap);
    allocator->Start(loc);

    AllocOptions allocReq;
    std::vector<MmcMemBlobPtr> blobs;

    allocReq.blobSize_ = SIZE_32K;
    allocReq.numBlobs_ = 1;
    allocReq.preferredRank_.push_back(0);
    allocReq.mediaType_ = MEDIA_DRAM;

    Result ret;
    for (size_t i = 0; i < 10; i++) {
        ret = allocator->Alloc(allocReq, blobs);
        EXPECT_EQ(ret, MMC_OK);
    }
    EXPECT_EQ(blobs.size(), 10U);
    for (size_t i = 0; i < blobs.size(); i++) {
        EXPECT_EQ(blobs[i]->Rank(), 0);
        EXPECT_EQ(blobs[i]->Size(), allocReq.blobSize_);
        EXPECT_EQ(blobs[i]->Type(), allocReq.mediaType_);
        EXPECT_EQ(blobs[i]->Gva(), i * allocReq.blobSize_);
    }

    for (size_t i = 0; i < blobs.size(); i++) {
        ret = allocator->Free(blobs[i]);
        EXPECT_EQ(ret, MMC_OK);
    }
    allocator->Stop(loc);

    blobMap.push_back({"1", blobs[1]->GetDesc()});
    blobMap.push_back({"4", blobs[4]->GetDesc()});
    blobMap.push_back({"7", blobs[7]->GetDesc()});

    blobs.erase(blobs.begin() + 7);
    blobs.erase(blobs.begin() + 4);
    blobs.erase(blobs.begin() + 1);

    ret = allocator->BuildFromBlobs(loc, blobMap);
    EXPECT_EQ(ret, MMC_OK);
    allocator->Start(loc);

    std::vector<MmcMemBlobPtr> blobsCompare;
    for (size_t i = 0; i < 7; i++) {
        ret = allocator->Alloc(allocReq, blobsCompare);
        EXPECT_EQ(ret, MMC_OK);
    }
    EXPECT_EQ(blobs.size(), 7U);
    for (size_t i = 0; i < blobs.size(); i++) {
        EXPECT_EQ(blobs[i]->Rank(), 0);
        EXPECT_EQ(blobs[i]->Size(), blobsCompare[i]->Size());
        EXPECT_EQ(blobs[i]->Type(), blobsCompare[i]->Type());
        EXPECT_EQ(blobs[i]->Gva(), blobsCompare[i]->Gva());
    }
}

TEST_F(TestMmcGlobalAllocator, ReBuildUnalignedSizeWithoutAllocSize)
{
    // 覆盖旧格式数据恢复：desc.allocSize_ 为 0 且 size_ 非 4KB 对齐时，
    // 恢复应按对齐后占用记账并回填 desc，释放后账目归零
    constexpr uint64_t unalignedSize = 5000U;           // 非 4KB 对齐的用户数据大小
    constexpr uint64_t alignedOccupied = SIZE_32K / 4U; // alignUp(5000) = 8192，真实切块占用
    constexpr uint64_t blobCount = 3U;

    MmcGlobalAllocatorPtr allocator = MmcMakeRef<MmcGlobalAllocator>();
    uint64_t capacity = SIZE_32K * 10U;

    MmcLocation loc;
    MmcLocalMemlInitInfo info;
    loc.mediaType_ = MEDIA_DRAM;
    loc.rank_ = 0;
    info.bmAddr_ = 0;
    info.capacity_ = capacity;
    allocator->Mount(loc, info);

    // 模拟旧格式备份：无 allocSize 字段（=0），gva 按旧版本真实切块间隔 8192 排布
    std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
    blobMap.push_back({"0", MmcMemBlobDesc(0, 0U, unalignedSize, MEDIA_DRAM)});
    blobMap.push_back({"1", MmcMemBlobDesc(0, alignedOccupied, unalignedSize, MEDIA_DRAM)});
    blobMap.push_back({"2", MmcMemBlobDesc(0, alignedOccupied * 2U, unalignedSize, MEDIA_DRAM)});

    Result ret = allocator->BuildFromBlobs(loc, blobMap);
    EXPECT_EQ(ret, MMC_OK);

    // 回填断言：恢复后 desc.allocSize_ 被修正为对齐后的实际占用，旧数据自愈
    for (auto &entry : blobMap) {
        EXPECT_EQ(entry.second.allocSize_, alignedOccupied);
    }

    // 账目断言：恢复占用 = 数量 * 对齐占用
    uint64_t totalSize[MEDIA_NONE] = {0};
    uint64_t usedSize[MEDIA_NONE] = {0};
    allocator->GetUsedInfo(totalSize, usedSize);
    EXPECT_EQ(usedSize[MEDIA_DRAM], blobCount * alignedOccupied);

    allocator->Start(loc);

    // 模拟 RebuildMeta 用回填后的 desc 构造 blob，逐个释放
    for (auto &entry : blobMap) {
        auto &desc = entry.second;
        MmcMemBlobPtr blob = MmcMakeRef<MmcMemBlob>(
            desc.rank_, desc.gva_, desc.size_, static_cast<MediaType>(desc.mediaType_), ALLOCATED, desc.allocSize_);
        ret = allocator->Free(blob);
        EXPECT_EQ(ret, MMC_OK);
    }

    // 全部释放后账目归零（GetUsedInfo 对入参数组是累加语义，复查须用清零后的新数组）
    uint64_t totalSizeAfter[MEDIA_NONE] = {0};
    uint64_t usedSizeAfter[MEDIA_NONE] = {0};
    allocator->GetUsedInfo(totalSizeAfter, usedSizeAfter);
    EXPECT_EQ(usedSizeAfter[MEDIA_DRAM], 0U);
}

TEST_F(TestMmcGlobalAllocator, AllocForce)
{
    MmcGlobalAllocatorPtr allocator = MmcMakeRef<MmcGlobalAllocator>();
    uint64_t size = SIZE_32K * 10U;
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
    for (int i = 0; i < 5; i++) {
        MmcLocation loc;
        MmcLocalMemlInitInfo info;
        loc.mediaType_ = MEDIA_HBM;
        loc.rank_ = i;
        info.bmAddr_ = size * i;
        info.capacity_ = size;
        std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
        allocator->Mount(loc, info);
        allocator->BuildFromBlobs(loc, blobMap);
        allocator->Start(loc);
    }

    AllocOptions allocReq;
    std::vector<MmcMemBlobPtr> blobs;

    allocReq.blobSize_ = SIZE_32K;
    allocReq.numBlobs_ = 10;
    allocReq.preferredRank_.push_back(5);
    allocReq.mediaType_ = MEDIA_DRAM;
    allocReq.flags_ = ALLOC_FORCE_BY_RANK;

    Result ret = allocator->Alloc(allocReq, blobs);
    EXPECT_EQ(ret, MMC_ERROR);
    EXPECT_EQ(blobs.size(), 0U);

    allocReq.numBlobs_ = 1;
    allocReq.preferredRank_.clear();
    allocReq.preferredRank_.push_back(10);
    ret = allocator->Alloc(allocReq, blobs);
    EXPECT_EQ(ret, MMC_ERROR);
    EXPECT_EQ(blobs.size(), 0U);

    allocReq.preferredRank_.clear();
    allocReq.preferredRank_.push_back(5);
    ret = allocator->Alloc(allocReq, blobs);
    EXPECT_EQ(ret, MMC_OK);
    EXPECT_EQ(blobs.size(), 1U);
    EXPECT_EQ(blobs[0]->Rank(), allocReq.preferredRank_.empty() ? 0 : allocReq.preferredRank_[0]);
    EXPECT_EQ(blobs[0]->Size(), allocReq.blobSize_);
    EXPECT_EQ(blobs[0]->Type(), allocReq.mediaType_);
    EXPECT_EQ(blobs[0]->Gva(), size * (allocReq.preferredRank_.empty() ? 0 : allocReq.preferredRank_[0]));
}

TEST_F(TestMmcGlobalAllocator, AllocRandom)
{
    MmcGlobalAllocatorPtr allocator = MmcMakeRef<MmcGlobalAllocator>();
    uint64_t size = SIZE_32K * 10U;
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
    for (int i = 0; i < 5; i++) {
        MmcLocation loc;
        MmcLocalMemlInitInfo info;
        loc.mediaType_ = MEDIA_HBM;
        loc.rank_ = i;
        info.bmAddr_ = size * i;
        info.capacity_ = size;
        std::vector<std::pair<std::string, MmcMemBlobDesc>> blobMap;
        allocator->Mount(loc, info);
        allocator->BuildFromBlobs(loc, blobMap);
        allocator->Start(loc);
    }

    AllocOptions allocReq;
    std::vector<MmcMemBlobPtr> blobs;

    allocReq.blobSize_ = SIZE_32K;
    allocReq.numBlobs_ = 10;
    allocReq.preferredRank_.push_back(0);
    allocReq.mediaType_ = MEDIA_HBM;
    allocReq.flags_ = ALLOC_RANDOM;

    Result ret = allocator->Alloc(allocReq, blobs);
    EXPECT_EQ(ret, MMC_ERROR);
    EXPECT_EQ(blobs.size(), 0U);

    allocReq.mediaType_ = MEDIA_DRAM;

    ret = allocator->Alloc(allocReq, blobs);
    EXPECT_EQ(ret, MMC_OK);
    EXPECT_EQ(blobs.size(), 10U);
    for (size_t i = 0; i < blobs.size(); i++) {
        cout << "blob[" << i << "] rank: " << blobs[i]->Rank() << endl;
        EXPECT_EQ(blobs[i]->Size(), allocReq.blobSize_);
        EXPECT_EQ(blobs[i]->Type(), allocReq.mediaType_);
        EXPECT_EQ(blobs[i]->Gva(), size * blobs[i]->Rank());
    }

    std::vector<MmcMemBlobPtr> blobs1;
    ret = allocator->Alloc(allocReq, blobs1);
    EXPECT_EQ(ret, MMC_OK);
    EXPECT_EQ(blobs1.size(), 10U);
    bool isEqual = true;
    for (size_t i = 0; i < blobs1.size(); i++) {
        cout << "blob[" << i << "] rank: " << blobs1[i]->Rank() << endl;
        if (blobs[i] != blobs1[i]) {
            isEqual = false;
        }
        EXPECT_EQ(blobs1[i]->Size(), allocReq.blobSize_);
        EXPECT_EQ(blobs1[i]->Type(), allocReq.mediaType_);
        EXPECT_EQ(blobs1[i]->Gva(), size * blobs1[i]->Rank() + SIZE_32K);
    }
    EXPECT_FALSE(isEqual);
}

// ==================== GlobalAllocator Mount MEDIA_SSD routing tests ====================

TEST_F(TestMmcGlobalAllocator, MountDramStillUsesBlobAllocator)
{
    MmcGlobalAllocatorPtr allocator = MmcMakeRef<MmcGlobalAllocator>();
    MmcLocation loc{0, MEDIA_DRAM};
    // 使用非零 bmAddr 确保 DRAM blob 的 gva ≠ 0，与 SSD (gva=0) 区分
    MmcLocalMemlInitInfo info{SIZE_32K * 1000, SIZE_32K * 100};

    auto ret = allocator->Mount(loc, info);
    EXPECT_EQ(ret, MMC_OK);
    allocator->Start(loc);

    AllocOptions allocOpt{SIZE_32K, 1, MEDIA_DRAM, {0}, 0};
    std::vector<MmcMemBlobPtr> blobs;
    ret = allocator->Alloc(allocOpt, blobs);
    EXPECT_EQ(ret, MMC_OK);
    EXPECT_EQ(blobs.size(), 1u);
    EXPECT_EQ(blobs[0]->Type(), static_cast<uint16_t>(MEDIA_DRAM));
    EXPECT_GE(blobs[0]->Gva(), SIZE_32K * 1000u); // DRAM gva 基于 bmAddr+offset
}

TEST_F(TestMmcGlobalAllocator, FreeSsdThroughGlobal)
{
    MmcGlobalAllocatorPtr allocator = MmcMakeRef<MmcGlobalAllocator>();
    MmcLocation loc{0, MEDIA_SSD};
    MmcLocalMemlInitInfo info{0, SIZE_32K * 100};
    allocator->Mount(loc, info);
    allocator->Start(loc);

    // Free of SSD blob via GlobalAllocator is a no-op (UBS IO managed)
    auto ssdBlob = MmcMakeRef<MmcMemBlob>(0, 0, SIZE_32K, MEDIA_SSD, ALLOCATED, 0);
    ASSERT_NE(ssdBlob, nullptr);
    auto ret = allocator->Free(ssdBlob);
    EXPECT_EQ(ret, MMC_OK);
}

TEST_F(TestMmcGlobalAllocator, SsdCanUnmountWhenEmpty)
{
    MmcGlobalAllocatorPtr allocator = MmcMakeRef<MmcGlobalAllocator>();
    MmcLocation loc{0, MEDIA_SSD};
    MmcLocalMemlInitInfo info{0, SIZE_32K * 100};
    allocator->Mount(loc, info);
    allocator->Start(loc);

    // SSD unmount always returns OK (UBS IO managed, no allocator)
    auto ret = allocator->Unmount(loc);
    EXPECT_EQ(ret, MMC_OK);
}

// 最大连续空闲块查询 / 碎片场景判定
TEST_F(TestMmcGlobalAllocator, MaxContiguousFreeSizeAndFragmentation)
{
    MmcGlobalAllocatorPtr allocator = MmcMakeRef<MmcGlobalAllocator>();
    MmcLocation loc{0, MEDIA_DRAM};
    const uint64_t capacity = SIZE_32K * 10;
    MmcLocalMemlInitInfo info{0, capacity};
    allocator->Mount(loc, info);
    allocator->Start(loc);

    EXPECT_EQ(allocator->GetMaxContiguousFreeSize(MEDIA_DRAM), capacity);

    // 分配 9 个 32K blob 填充 [0, 288K)，尾部空闲 [288K, 320K) = 32K
    std::vector<MmcMemBlobPtr> blobs;
    for (int i = 0; i < 9; ++i) {
        AllocOptions allocReq{SIZE_32K, 1, MEDIA_DRAM, {0}, ALLOC_FORCE_BY_RANK};
        std::vector<MmcMemBlobPtr> oneBlob;
        ASSERT_EQ(allocator->Alloc(allocReq, oneBlob), MMC_OK);
        ASSERT_EQ(oneBlob.size(), 1U);
        blobs.push_back(oneBlob[0]);
    }
    EXPECT_EQ(allocator->GetMaxContiguousFreeSize(MEDIA_DRAM), SIZE_32K);

    // 释放中间 blob 制造空洞 [128K, 160K)：总空闲 64K，最大连续空闲仍为 32K
    ASSERT_EQ(allocator->Free(blobs[4]), MMC_OK);
    EXPECT_EQ(allocator->GetMaxContiguousFreeSize(MEDIA_DRAM), SIZE_32K);

    // 碎片判定：总空闲 64K 足够，申请 32K 不属于碎片，申请 64K 属于碎片；
    // 申请 128K 超过总空闲属于容量耗尽而非碎片，不触发强制淘汰
    EXPECT_FALSE(allocator->IsFragmentation(MEDIA_DRAM, SIZE_32K));
    EXPECT_TRUE(allocator->IsFragmentation(MEDIA_DRAM, SIZE_32K * 2));
    EXPECT_FALSE(allocator->IsFragmentation(MEDIA_DRAM, SIZE_32K * 4));
    EXPECT_FALSE(allocator->IsFragmentation(MEDIA_SSD, SIZE_32K));

    // 释放相邻 blob 后空洞合并为 [96K, 160K) = 64K
    ASSERT_EQ(allocator->Free(blobs[3]), MMC_OK);
    EXPECT_EQ(allocator->GetMaxContiguousFreeSize(MEDIA_DRAM), SIZE_32K * 2);
    EXPECT_FALSE(allocator->IsFragmentation(MEDIA_DRAM, SIZE_32K * 2));
}

// MmcBlobAllocator 直接测试：最大连续空闲块查询
TEST_F(TestMmcGlobalAllocator, BlobAllocatorMaxContiguousFreeSize)
{
    // 非 max 模式：切块粒度取对齐值，maxAlignedSize 传本地计数以满足构造契约
    std::atomic<uint64_t> maxAlignedSize{0};
    MmcBlobAllocator blobAllocator(0, MEDIA_DRAM, 0, SIZE_32K * 4, false, &maxAlignedSize);
    blobAllocator.Start();
    auto blob = blobAllocator.Alloc(SIZE_32K);
    ASSERT_NE(blob, nullptr);
    // blob 位于 [0, 32K)，尾部空闲 [32K, 128K) 为最大连续空闲块
    EXPECT_EQ(blobAllocator.GetMaxContiguousFreeSize(), SIZE_32K * 3);
    EXPECT_EQ(blobAllocator.Release(blob), MMC_OK);
    EXPECT_EQ(blobAllocator.GetMaxContiguousFreeSize(), SIZE_32K * 4);
}
