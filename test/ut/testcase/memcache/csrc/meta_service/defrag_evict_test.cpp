/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemCache_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
*/
#include "mmc_defrag_evict.h"

#include "gtest/gtest.h"

#include <cstdint>
#include <vector>

using namespace ock::mmc;
using namespace testing;

namespace {
/* 测试用统一分配单元：4KB，用于构造连续的段图 */
constexpr uint64_t kBlobSize = 4096;
constexpr uint64_t k1Mb = 1024ULL * 1024;
} // namespace

TEST(DefragWastePercentTest, EmptyOrSingleSegmentReturnsZero)
{
    EXPECT_EQ(GetDefragWastePercent({}), 0U);
    EXPECT_EQ(GetDefragWastePercent({SpaceRange(0, DEFRAG_EVICT_MIN_USABLE_BLOCK_SIZE)}), 0U);
    EXPECT_EQ(GetDefragWastePercent({SpaceRange(0, k1Mb)}), 0U);
}

TEST(DefragWastePercentTest, AllSegmentsUsableReturnsZero)
{
    std::vector<SpaceRange> segments = {
        SpaceRange(0, DEFRAG_EVICT_MIN_USABLE_BLOCK_SIZE),
        SpaceRange(DEFRAG_EVICT_MIN_USABLE_BLOCK_SIZE, DEFRAG_EVICT_MIN_USABLE_BLOCK_SIZE),
        SpaceRange(DEFRAG_EVICT_MIN_USABLE_BLOCK_SIZE * 2, DEFRAG_EVICT_MIN_USABLE_BLOCK_SIZE * 3),
    };
    EXPECT_EQ(GetDefragWastePercent(segments), 0U);
}

TEST(DefragWastePercentTest, AllSegmentsUnusableReturnsHundred)
{
    std::vector<SpaceRange> segments = {
        SpaceRange(0, k1Mb),
        SpaceRange(k1Mb, k1Mb),
        SpaceRange(k1Mb * 2, k1Mb),
    };
    EXPECT_EQ(GetDefragWastePercent(segments), 100U);
}

TEST(DefragWastePercentTest, MixedUsableAndUnusableReturnsRatio)
{
    // 1 个 3MB 可用段 + 1 个 1MB 不可用段 => 1MB / 4MB = 25%
    std::vector<SpaceRange> segments = {
        SpaceRange(0, DEFRAG_EVICT_MIN_USABLE_BLOCK_SIZE),
        SpaceRange(DEFRAG_EVICT_MIN_USABLE_BLOCK_SIZE, k1Mb),
    };
    EXPECT_EQ(GetDefragWastePercent(segments), 25U);
}

TEST(DefragWastePercentTest, BelowMinUsableBlockSizeCountsAsWaste)
{
    // 3MB 整块视为可用；3MB - 1 的段视为不可用，浪费率约为 49.99% -> 49
    const uint64_t belowThreshold = DEFRAG_EVICT_MIN_USABLE_BLOCK_SIZE - 1;
    std::vector<SpaceRange> segments = {
        SpaceRange(0, DEFRAG_EVICT_MIN_USABLE_BLOCK_SIZE),
        SpaceRange(DEFRAG_EVICT_MIN_USABLE_BLOCK_SIZE, belowThreshold),
    };
    EXPECT_EQ(GetDefragWastePercent(segments), 49U);
}

TEST(DefragWastePercentTest, IsDefragWorthyUsesSeventyPercentThreshold)
{
    uint64_t wastePercent = 0;

    // 25% 浪费率低于阈值：不触发整理
    EXPECT_FALSE(
        IsDefragWorthy({SpaceRange(0, k1Mb), SpaceRange(k1Mb, DEFRAG_EVICT_MIN_USABLE_BLOCK_SIZE)}, wastePercent));
    EXPECT_EQ(wastePercent, 25U);

    // 两个 1MB 碎片均为不可用：浪费率 100%，触发整理
    EXPECT_TRUE(IsDefragWorthy({SpaceRange(0, k1Mb), SpaceRange(k1Mb, k1Mb)}, wastePercent));
    EXPECT_EQ(wastePercent, 100U);
}

TEST(PickDefragWindowTest, EmptyInputsReturnEmpty)
{
    EXPECT_TRUE(PickDefragWindow({}, {}, 1U).empty());
    EXPECT_TRUE(PickDefragWindow({SpaceRange(0, kBlobSize), SpaceRange(kBlobSize, kBlobSize)}, {}, 1U).empty());
}

TEST(PickDefragWindowTest, SingleConsecutiveWindowSelectedInGvaOrder)
{
    std::vector<DefragBlobEntry> blobs = {
        {0, kBlobSize, 0, true},
        {kBlobSize, kBlobSize, 1, true},
        {kBlobSize * 2, kBlobSize, 2, true},
    };
    std::vector<uint32_t> expected = {0, 1, 2};
    EXPECT_EQ(PickDefragWindow({}, blobs, 10U), expected);
}

TEST(PickDefragWindowTest, WholeWindowReleasedEvenWhenExceedingTarget)
{
    std::vector<DefragBlobEntry> blobs = {
        {0, kBlobSize, 0, true},
        {kBlobSize, kBlobSize, 1, true},
        {kBlobSize * 2, kBlobSize, 2, true},
        {kBlobSize * 3, kBlobSize, 3, true},
        {kBlobSize * 4, kBlobSize, 4, true},
    };
    // releaseTarget 仅 2，但整窗（5 个连续可释放 key）会一次性全部选中
    std::vector<uint32_t> expected = {0, 1, 2, 3, 4};
    EXPECT_EQ(PickDefragWindow({}, blobs, 2U), expected);
}

TEST(PickDefragWindowTest, LargerMergedWindowSelectedFirst)
{
    // 不可释放的 key1 会把连续段切成两个窗口，更大（更多 key）的窗口优先
    std::vector<DefragBlobEntry> blobs = {
        {0, kBlobSize, 0, true},
        {kBlobSize, kBlobSize, 1, false},
        {kBlobSize * 2, kBlobSize, 2, true},
        {kBlobSize * 3, kBlobSize, 3, true},
    };
    std::vector<uint32_t> expected = {2, 3};
    EXPECT_EQ(PickDefragWindow({}, blobs, 2U), expected);
}

TEST(PickDefragWindowTest, EqualMergedSizeTieBreaksByKeyCount)
{
    // 两个窗口 span 相同，但 key 数分别为 1 和 2，key 数更多的优先
    std::vector<DefragBlobEntry> blobs = {
        {0, kBlobSize * 2, 0, true},
        {kBlobSize * 2, kBlobSize, 1, false},
        {kBlobSize * 3, kBlobSize, 2, true},
        {kBlobSize * 4, kBlobSize, 3, true},
    };
    std::vector<uint32_t> expected = {2, 3, 0};
    EXPECT_EQ(PickDefragWindow({}, blobs, 10U), expected);
}

TEST(PickDefragWindowTest, FreeSegmentsMergeIntoSpan)
{
    // 前导与尾随空闲段会并入窗口 span，但窗口只收集可释放 key
    std::vector<SpaceRange> freeSegments = {
        SpaceRange(0, kBlobSize),
        SpaceRange(kBlobSize * 3, kBlobSize),
    };
    std::vector<DefragBlobEntry> blobs = {
        {kBlobSize, kBlobSize, 0, true},
        {kBlobSize * 2, kBlobSize, 1, true},
    };
    std::vector<uint32_t> expected = {0, 1};
    EXPECT_EQ(PickDefragWindow(freeSegments, blobs, 10U), expected);
}

TEST(PickDefragWindowTest, GapBetweenBlobsSplitsWindows)
{
    // 没有空闲段的地址空洞会打断窗口（start > wEnd 分支），并按 span 降序选取
    std::vector<DefragBlobEntry> blobs = {
        {0, kBlobSize * 4, 0, true},
        {kBlobSize * 5, kBlobSize, 1, true},
        {kBlobSize * 6, kBlobSize, 2, true},
    };
    std::vector<uint32_t> expected = {0, 1, 2};
    EXPECT_EQ(PickDefragWindow({}, blobs, 10U), expected);
}
