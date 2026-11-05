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
#include "gtest/gtest.h"
#include "mmc_meta_lease_manager.h"

namespace ock {
namespace mmc {
TEST(TestMmcMetaLeaseManager, LastReleaseClearsDeadline)
{
    MmcMetaLeaseManager manager;
    ASSERT_EQ(manager.Add(1, 2, 300000), MMC_OK);
    ASSERT_GT(manager.RemainingLeaseTtlMs(), 0U);
    ASSERT_EQ(manager.Remove(1, 2), MMC_OK);
    EXPECT_EQ(manager.UseCount(), 0U);
    EXPECT_EQ(manager.RemainingLeaseTtlMs(), 0U);
}

TEST(TestMmcMetaLeaseManager, KeepsDeadlineUntilAllHoldersRelease)
{
    MmcMetaLeaseManager manager;
    ASSERT_EQ(manager.Add(1, 2, 300000), MMC_OK);
    ASSERT_EQ(manager.Add(1, 3, 300000), MMC_OK);
    ASSERT_EQ(manager.Remove(1, 2), MMC_OK);
    EXPECT_EQ(manager.UseCount(), 1U);
    EXPECT_TRUE(manager.HasLease(1, 3));
    EXPECT_GT(manager.RemainingLeaseTtlMs(), 0U);
    ASSERT_EQ(manager.Remove(1, 3), MMC_OK);
    EXPECT_EQ(manager.UseCount(), 0U);
    EXPECT_EQ(manager.RemainingLeaseTtlMs(), 0U);
}

TEST(TestMmcMetaLeaseManager, UnknownOrRepeatedReleasePreservesOtherHolder)
{
    MmcMetaLeaseManager manager;
    ASSERT_EQ(manager.Add(1, 2, 300000), MMC_OK);
    ASSERT_EQ(manager.Add(1, 3, 300000), MMC_OK);
    ASSERT_EQ(manager.Remove(1, 2), MMC_OK);
    ASSERT_EQ(manager.Remove(1, 2), MMC_OK);
    ASSERT_EQ(manager.Remove(2, 3), MMC_OK);
    EXPECT_EQ(manager.UseCount(), 1U);
    EXPECT_TRUE(manager.HasLease(1, 3));
    EXPECT_GT(manager.RemainingLeaseTtlMs(), 0U);
}

TEST(TestMmcMetaLeaseManager, NextAddUsesFreshDeadline)
{
    MmcMetaLeaseManager manager;
    ASSERT_EQ(manager.Add(1, 2, 300000), MMC_OK);
    ASSERT_EQ(manager.Remove(1, 2), MMC_OK);
    ASSERT_EQ(manager.Add(1, 3, 5000), MMC_OK);
    EXPECT_EQ(manager.UseCount(), 1U);
    EXPECT_GT(manager.RemainingLeaseTtlMs(), 0U);
    EXPECT_LE(manager.RemainingLeaseTtlMs(), 5000U);
}
} // namespace mmc
} // namespace ock
