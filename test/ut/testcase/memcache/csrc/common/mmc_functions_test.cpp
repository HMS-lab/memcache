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

#include "common/mmc_functions.h"
#include "gtest/gtest.h"
#include <array>
#include <cstdlib>
#include <limits>

using ock::mmc::FormatBytesHuman;
using ock::mmc::FormatUsageHuman;
using ock::mmc::SafeCopy;

namespace {
constexpr uint64_t kOneKiB = 1024;
constexpr uint64_t kOneMiB = kOneKiB * 1024;
constexpr uint64_t kOneGiB = kOneMiB * 1024;
constexpr uint64_t kOneTiB = kOneGiB * 1024;
constexpr uint64_t kOnePiB = kOneTiB * 1024;
constexpr uint64_t kOneEiB = kOnePiB * 1024;

// 线上真实样例：dram_used=28179398656/4509715660800
constexpr uint64_t kDramUsedSampleBytes = 28179398656;
constexpr uint64_t kDramTotalSampleBytes = 4509715660800;
} // namespace

TEST(SafeCopyDeathTest, NullDestinationReturnsNormally)
{
    EXPECT_EXIT(
        {
            SafeCopy("payload", nullptr, sizeof("payload"));
            SafeCopy("", nullptr, sizeof("payload"));
            SafeCopy("payload", nullptr, 0);
            std::_Exit(EXIT_SUCCESS);
        },
        testing::ExitedWithCode(EXIT_SUCCESS), "");
}

TEST(SafeCopyTest, ZeroCapacityLeavesDestinationUnchanged)
{
    constexpr char marker = '#';
    std::array<char, sizeof("payload")> destination{};
    destination.fill(marker);
    const auto expected = destination;

    SafeCopy("payload", destination.data(), 0);
    EXPECT_EQ(destination, expected);

    destination.fill(marker);
    SafeCopy("", destination.data(), 0);
    EXPECT_EQ(destination, expected);
}

TEST(FormatBytesHumanTest, ZeroAndByteRange)
{
    EXPECT_EQ(FormatBytesHuman(0), "0B");
    EXPECT_EQ(FormatBytesHuman(1), "1B");
    EXPECT_EQ(FormatBytesHuman(kOneKiB - 1), "1023B");
}

TEST(FormatBytesHumanTest, KilobyteAndMegabyteRange)
{
    EXPECT_EQ(FormatBytesHuman(kOneKiB), "1.0KiB");
    EXPECT_EQ(FormatBytesHuman(kOneKiB + kOneKiB / 2), "1.5KiB");
    EXPECT_EQ(FormatBytesHuman(10 * kOneKiB), "10KiB");
    EXPECT_EQ(FormatBytesHuman(32 * kOneKiB), "32KiB");
    EXPECT_EQ(FormatBytesHuman(64 * kOneKiB), "64KiB");
    EXPECT_EQ(FormatBytesHuman(kOneMiB / 2), "512KiB");
    EXPECT_EQ(FormatBytesHuman(kOneMiB), "1.0MiB");
}

TEST(FormatBytesHumanTest, GigabyteAndTerabyteRange)
{
    EXPECT_EQ(FormatBytesHuman(kOneGiB), "1.0GiB");
    EXPECT_EQ(FormatBytesHuman(kOneGiB * 19 / 2), "9.5GiB");
    EXPECT_EQ(FormatBytesHuman(5 * kOneGiB), "5.0GiB");
    EXPECT_EQ(FormatBytesHuman(10 * kOneGiB), "10GiB");
    EXPECT_EQ(FormatBytesHuman(kOneTiB), "1.0TiB");
    EXPECT_EQ(FormatBytesHuman(kDramUsedSampleBytes), "26GiB");
    EXPECT_EQ(FormatBytesHuman(kDramTotalSampleBytes), "4.1TiB");
}

TEST(FormatBytesHumanTest, CarryToNextUnitWhenRounding)
{
    // 1023.999KiB 取整会变成 1024，必须进位成 1.0MiB
    EXPECT_EQ(FormatBytesHuman(kOneMiB - 1), "1.0MiB");
    // 1023.5KiB 恰好是进位边界
    EXPECT_EQ(FormatBytesHuman(kOneMiB - kOneKiB / 2), "1.0MiB");
    // 1023.499KiB 不进位，取整为 1023KiB
    EXPECT_EQ(FormatBytesHuman(kOneMiB - kOneKiB / 2 - 1), "1023KiB");
}

TEST(FormatBytesHumanTest, OneDecimalBelowTen)
{
    // <10 的数值保留 1 位小数
    EXPECT_EQ(FormatBytesHuman(kOneGiB * 94 / 10), "9.4GiB");
    // 9.99GiB 四舍五入后显示 10.0GiB（仍属 <10 区间，不进位）
    EXPECT_EQ(FormatBytesHuman(kOneGiB * 999 / 100), "10.0GiB");
}

TEST(FormatBytesHumanTest, PebibyteAndExbibyteRange)
{
    EXPECT_EQ(FormatBytesHuman(kOnePiB), "1.0PiB");
    EXPECT_EQ(FormatBytesHuman(kOneEiB), "1.0EiB");
    EXPECT_EQ(FormatBytesHuman(std::numeric_limits<uint64_t>::max()), "16EiB");
}

TEST(FormatUsageHumanTest, ZeroTotalOmitsPercentage)
{
    // 未配置 SSD 时 total 为 0，不追加百分比
    EXPECT_EQ(FormatUsageHuman(0, 0), "0B/0B");
    // used 为 0 但 total 有效时，百分比为 0.00%
    EXPECT_EQ(FormatUsageHuman(0, kOneGiB), "0B/1.0GiB (0.00%)");
}

TEST(FormatUsageHumanTest, PercentageKeepsTwoDecimals)
{
    EXPECT_EQ(FormatUsageHuman(kOneGiB / 2, kOneGiB), "512MiB/1.0GiB (50.00%)");
    EXPECT_EQ(FormatUsageHuman(kOneGiB, kOneGiB), "1.0GiB/1.0GiB (100.00%)");
    EXPECT_EQ(FormatUsageHuman(kOneGiB / 3, kOneGiB), "341MiB/1.0GiB (33.33%)");
    // /metrics/summary 的实际容量组合：32KiB/64KiB
    EXPECT_EQ(FormatUsageHuman(32 * kOneKiB, 64 * kOneKiB), "32KiB/64KiB (50.00%)");
}

TEST(FormatUsageHumanTest, RealDramSample)
{
    // 线上样例：dram_used=28179398656/4509715660800
    EXPECT_EQ(FormatUsageHuman(kDramUsedSampleBytes, kDramTotalSampleBytes), "26GiB/4.1TiB (0.62%)");
}
