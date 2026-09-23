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

// DS flash 场景内存碎片模型：value 尺寸呈奇数页混合分布（13 页 / 17 页两档）。
// 同一棋盘式分配/释放序列下对照验证：
//   enableMaxValueMode 关闭 => 批量释放小档后仅剩 13 页规格空洞，大档申请失败（外部碎片复现）；
//   enableMaxValueMode 开启 => 分配统一抬升到全局最大档，空洞等尺寸可复用，大档申请成功。
#include "mmc_global_allocator.h"

#include <vector>

#include "gtest/gtest.h"
#include "mmc_blob_allocator.h"
#include "mmc_ref.h"

using namespace std;
using namespace ock::mmc;

namespace {
constexpr uint64_t PAGE_SIZE_BYTES = 4096UL;
constexpr uint64_t SMALL_PAGES = 13; // 奇数页小档（DS flash 混合 value 尺寸建模）
constexpr uint64_t LARGE_PAGES = 17; // 奇数页大档
constexpr uint64_t ARENA_PAGES = 11006;
constexpr uint32_t RANK_ID = 0;
constexpr uint64_t UNALIGNED_SIZE = 100; // 非对齐请求字节，用于触发碎片

constexpr uint64_t SmallBytes()
{
    return SMALL_PAGES * PAGE_SIZE_BYTES;
}
constexpr uint64_t LargeBytes()
{
    return LARGE_PAGES * PAGE_SIZE_BYTES;
}

struct Checkerboard {
    MmcGlobalAllocatorPtr allocator;
    std::vector<MmcMemBlobPtr> smalls;
    std::vector<MmcMemBlobPtr> larges;
    std::vector<MmcMemBlobPtr> tailWall; // 末尾留一个在用小块封住尾部残留洞，保证棋盘封闭
};

bool TryAlloc(const Checkerboard &board, uint64_t blobSize, std::vector<MmcMemBlobPtr> &out)
{
    AllocOptions opt;
    opt.blobSize_ = blobSize;
    opt.numBlobs_ = 1;
    opt.mediaType_ = MEDIA_DRAM;
    opt.preferredRank_.push_back(RANK_ID);
    std::vector<MmcMemBlobPtr> blobs;
    if (board.allocator->Alloc(opt, blobs) != MMC_OK || blobs.empty()) {
        return false;
    }
    out.push_back(blobs[0]);
    return true;
}

uint64_t UsedBytes(const Checkerboard &board)
{
    uint64_t total[MEDIA_NONE] = {0};
    uint64_t used[MEDIA_NONE] = {0};
    board.allocator->GetUsedInfo(total, used);
    return used[MEDIA_DRAM];
}

// 建棋盘：交替分配小/大档直至放满，随后批量释放全部小档
Checkerboard BuildStarvedBoard(bool enableMaxValueMode)
{
    Checkerboard board;
    board.allocator = MmcMakeRef<MmcGlobalAllocator>();
    MmcLocation loc;
    MmcLocalMemlInitInfo info;
    loc.mediaType_ = MEDIA_DRAM;
    loc.rank_ = RANK_ID;
    info.bmAddr_ = 0;
    info.capacity_ = ARENA_PAGES * PAGE_SIZE_BYTES;
    std::vector<std::pair<std::string, MmcMemBlobDesc>> emptyBlobMap;
    if (enableMaxValueMode) {
        EXPECT_EQ(board.allocator->Mount(loc, info, true), MMC_OK);
    } else {
        EXPECT_EQ(board.allocator->Mount(loc, info), MMC_OK);
    }
    board.allocator->BuildFromBlobs(loc, emptyBlobMap);
    board.allocator->Start(loc);

    while (true) {
        if (!TryAlloc(board, SmallBytes(), board.smalls)) {
            break;
        }
        if (!TryAlloc(board, LargeBytes(), board.larges)) {
            board.tailWall.push_back(board.smalls.back());
            board.smalls.pop_back();
            break;
        }
    }
    for (auto &blob : board.smalls) {
        EXPECT_EQ(board.allocator->Free(blob), MMC_OK);
    }
    board.smalls.clear();
    return board;
}

void Drain(const Checkerboard &board)
{
    for (const auto &blob : board.larges) {
        EXPECT_EQ(board.allocator->Free(blob), MMC_OK);
    }
    for (const auto &blob : board.tailWall) {
        EXPECT_EQ(board.allocator->Free(blob), MMC_OK);
    }
}
} // namespace

class TestAllocFragmentation : public testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// 同一棋盘序列的对照实验：关模式 => 大档申请失败；开模式 => 大档申请成功且账本归零
TEST_F(TestAllocFragmentation, MaxValueModeResolvesOddStrideCheckerboard)
{
    // 对照组：enableMaxValueMode 关闭
    {
        Checkerboard board = BuildStarvedBoard(false);
        ASSERT_FALSE(board.larges.empty());
        uint64_t total[MEDIA_NONE] = {0};
        uint64_t used[MEDIA_NONE] = {0};
        board.allocator->GetUsedInfo(total, used);
        const uint64_t freeBytes = total[MEDIA_DRAM] - used[MEDIA_DRAM];
        // 空闲总量远大于一次大档需求，且同尺寸小档仍可分配 => 是碎片而非耗尽
        ASSERT_GT(freeBytes, 100UL * LargeBytes()) << "模型失效：空闲量不足以演示碎片";
        std::vector<MmcMemBlobPtr> probe;
        ASSERT_TRUE(TryAlloc(board, SmallBytes(), probe));
        EXPECT_EQ(board.allocator->Free(probe[0]), MMC_OK);
        std::vector<MmcMemBlobPtr> victim;
        EXPECT_FALSE(TryAlloc(board, LargeBytes(), victim)) << "碎片模型未能复现分配失败";
        Drain(board);
    }

    // 实验组：enableMaxValueMode 开启，同一棋盘序列
    {
        Checkerboard board = BuildStarvedBoard(true);
        ASSERT_FALSE(board.larges.empty());
        std::vector<MmcMemBlobPtr> victim;
        ASSERT_TRUE(TryAlloc(board, LargeBytes(), victim)) << "max-value 模式未能消除棋盘空洞";
        EXPECT_EQ(board.allocator->Free(victim[0]), MMC_OK);
        Drain(board);
        // padded footprint 按记录值精确归还，无泄漏、无多还
        EXPECT_EQ(UsedBytes(board), 0UL);
    }
}

// 碎片率 gauge 守恒：Alloc 增 / Release 减，单块非对齐请求碎片 = 对齐切块 - 请求字节
TEST_F(TestAllocFragmentation, FragmentationGaugeConservation)
{
    constexpr uint64_t expectedFrag = PAGE_SIZE_BYTES - UNALIGNED_SIZE;
    std::atomic<uint64_t> maxAlignedSize{0};
    MmcBlobAllocator allocator(RANK_ID, MEDIA_DRAM, 0, ARENA_PAGES * PAGE_SIZE_BYTES, false, &maxAlignedSize);
    allocator.Start();

    EXPECT_EQ(allocator.GetFragmentationBytes(), 0UL);

    MmcMemBlobPtr blob = allocator.Alloc(UNALIGNED_SIZE);
    ASSERT_NE(blob, nullptr);
    EXPECT_EQ(allocator.GetFragmentationBytes(), expectedFrag);

    EXPECT_EQ(allocator.Release(blob), MMC_OK);
    EXPECT_EQ(allocator.GetFragmentationBytes(), 0UL);
}

// max-value 模式：小请求被放大到全局 max，碎片 = max - 请求字节
TEST_F(TestAllocFragmentation, FragmentationGaugeMaxValueMode)
{
    constexpr uint64_t presetMax = SmallBytes();
    constexpr uint64_t expectedFrag = presetMax - UNALIGNED_SIZE;
    std::atomic<uint64_t> maxAlignedSize{presetMax};
    MmcBlobAllocator allocator(RANK_ID, MEDIA_DRAM, 0, ARENA_PAGES * PAGE_SIZE_BYTES, true, &maxAlignedSize);
    allocator.Start();

    EXPECT_EQ(allocator.GetFragmentationBytes(), 0UL);

    MmcMemBlobPtr blob = allocator.Alloc(UNALIGNED_SIZE);
    ASSERT_NE(blob, nullptr);
    EXPECT_EQ(allocator.GetFragmentationBytes(), expectedFrag);

    EXPECT_EQ(allocator.Release(blob), MMC_OK);
    EXPECT_EQ(allocator.GetFragmentationBytes(), 0UL);
}

// BuildFromBlobs 恢复路径：requestedSize_ 按 size 回填，重启后碎片口径与 Alloc 路径一致
TEST_F(TestAllocFragmentation, FragmentationGaugeBuildFromBlobs)
{
    constexpr uint64_t expectedFrag = PAGE_SIZE_BYTES - UNALIGNED_SIZE;
    std::atomic<uint64_t> maxAlignedSize{0};
    MmcBlobAllocator allocator(RANK_ID, MEDIA_DRAM, 0, ARENA_PAGES * PAGE_SIZE_BYTES, false, &maxAlignedSize);
    allocator.Start();
    MmcMemBlobPtr blob = allocator.Alloc(UNALIGNED_SIZE);
    ASSERT_NE(blob, nullptr);
    EXPECT_EQ(allocator.GetFragmentationBytes(), expectedFrag);

    // 构造恢复用 blobList，allocSize_ 取 Alloc 返回的实际切块值
    MmcMemBlobDesc desc(RANK_ID, blob->Gva(), UNALIGNED_SIZE, MEDIA_DRAM, NONE, 0, blob->AllocSize());
    std::vector<std::pair<std::string, MmcMemBlobDesc>> blobList;
    blobList.emplace_back("key1", desc);

    // 新 allocator (未 Start) 走 BuildFromBlobs 恢复路径
    allocator.Stop();
    std::atomic<uint64_t> maxAlignedSize2{0};
    MmcBlobAllocator allocator2(RANK_ID, MEDIA_DRAM, 0, ARENA_PAGES * PAGE_SIZE_BYTES, false, &maxAlignedSize2);
    ASSERT_EQ(allocator2.BuildFromBlobs(blobList), MMC_OK);

    // 恢复后碎片应与 Alloc 路径一致
    EXPECT_EQ(allocator2.GetFragmentationBytes(), expectedFrag);
}
