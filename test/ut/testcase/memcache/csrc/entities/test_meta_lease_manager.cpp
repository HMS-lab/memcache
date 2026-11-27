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

#include "gtest/gtest.h"
#include "mmc_meta_lease_manager.h"

namespace ock {
namespace mmc {
namespace {
constexpr uint32_t kClientRank = 1;
constexpr uint32_t kFirstRequest = 1;
constexpr uint32_t kSecondRequest = 2;
constexpr uint64_t kShortTtlMs = 200;
constexpr uint64_t kLongTtlMs = 5000;
} // namespace

TEST(TestMmcMetaLeaseManager, WaitReturnsAfterDeadline)
{
    MmcMetaLeaseManager manager;
    EXPECT_EQ(manager.RemainingLeaseTtlMs(), 0U);
    ASSERT_EQ(manager.Add(kClientRank, kFirstRequest, kShortTtlMs), MMC_OK);
    EXPECT_GT(manager.RemainingLeaseTtlMs(), 0U);
    ASSERT_EQ(manager.Wait(), MMC_OK);
    EXPECT_EQ(manager.RemainingLeaseTtlMs(), 0U);
}

TEST(TestMmcMetaLeaseManager, ShorterLeasePreservesDeadline)
{
    MmcMetaLeaseManager manager;
    ASSERT_EQ(manager.Add(kClientRank, kFirstRequest, kLongTtlMs), MMC_OK);
    ASSERT_EQ(manager.Add(kClientRank, kSecondRequest, kShortTtlMs), MMC_OK);
    EXPECT_GT(manager.RemainingLeaseTtlMs(), kShortTtlMs);
}

TEST(TestMmcMetaLeaseManager, LongerLeaseExtendsDeadline)
{
    MmcMetaLeaseManager manager;
    ASSERT_EQ(manager.Add(kClientRank, kFirstRequest, kShortTtlMs), MMC_OK);
    ASSERT_EQ(manager.Add(kClientRank, kSecondRequest, kLongTtlMs), MMC_OK);
    EXPECT_GT(manager.RemainingLeaseTtlMs(), kShortTtlMs);
    EXPECT_LE(manager.RemainingLeaseTtlMs(), kLongTtlMs);
}

TEST(TestMmcMetaLeaseManager, RejectsTtlBeyondWaitDuration)
{
    constexpr auto maxWaitMs = static_cast<uint64_t>(std::chrono::milliseconds::max().count());
    MmcMetaLeaseManager manager;
    ASSERT_EQ(manager.Add(kClientRank, kFirstRequest, kLongTtlMs), MMC_OK);
    EXPECT_EQ(manager.Add(kClientRank, kSecondRequest, maxWaitMs + 1U), MMC_INVALID_PARAM);
    const auto remainingTtlMs = manager.RemainingLeaseTtlMs();
    EXPECT_GT(remainingTtlMs, 0U);
    EXPECT_LE(remainingTtlMs, kLongTtlMs);
}
} // namespace mmc
} // namespace ock
