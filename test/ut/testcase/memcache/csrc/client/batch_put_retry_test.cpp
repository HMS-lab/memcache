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

#include <cstddef>
#include <vector>

#include "gtest/gtest.h"

#include "mmc_client_default.h"
#include "mmc_types.h"
#include "smem_bm_def.h"

using namespace ock::mmc;

namespace {
constexpr size_t FIRST_RETRY_INDEX = 1U;
constexpr size_t SECOND_RETRY_INDEX = 2U;
constexpr size_t THIRD_RETRY_INDEX = 3U;
} // namespace

// Only a missing/unmounted GVA registration (-2017) triggers defensive retry.
TEST(BatchPutRetryTest, IsHybmCopyRetryable_AcceptsGvaNotFound)
{
    EXPECT_TRUE(IsHybmCopyRetryable(SMEM_BM_GVA_NOT_FOUND));
}

// MMC error codes are never retryable.
TEST(BatchPutRetryTest, IsHybmCopyRetryable_RejectsMmcErrors)
{
    EXPECT_FALSE(IsHybmCopyRetryable(MMC_OK));
    EXPECT_FALSE(IsHybmCopyRetryable(MMC_ERROR));
    EXPECT_FALSE(IsHybmCopyRetryable(MMC_INVALID_PARAM));
    EXPECT_FALSE(IsHybmCopyRetryable(MMC_UNMATCHED_KEY));
    EXPECT_FALSE(IsHybmCopyRetryable(MMC_EXCEED_CAPACITY));
    EXPECT_FALSE(IsHybmCopyRetryable(MMC_DUPLICATED_OBJECT));
    EXPECT_FALSE(IsHybmCopyRetryable(MMC_CLIENT_NOT_INIT));
}

TEST(BatchPutRetryTest, CollectPutRetryKeys_MixedResults_SelectsOnlyGvaNotFound)
{
    const std::vector<int> batchResult{MMC_OK, SMEM_BM_GVA_NOT_FOUND, MMC_ERROR, SMEM_BM_GVA_NOT_FOUND,
                                       MMC_UNMATCHED_KEY};
    const std::vector<size_t> expected{FIRST_RETRY_INDEX, THIRD_RETRY_INDEX};
    EXPECT_EQ(expected, CollectPutRetryKeys(batchResult));
}

TEST(BatchPutRetryTest, CollectPutRetryKeys_NoRetryable_ReturnsEmpty)
{
    const std::vector<int> batchResult{MMC_OK, MMC_ERROR, MMC_EXCEED_CAPACITY, MMC_UNMATCHED_KEY};
    EXPECT_TRUE(CollectPutRetryKeys(batchResult).empty());
}

TEST(BatchPutRetryTest, CollectPutRetryKeys_AllRetryable_ReturnsAllIndices)
{
    const std::vector<int> batchResult{SMEM_BM_GVA_NOT_FOUND, SMEM_BM_GVA_NOT_FOUND, SMEM_BM_GVA_NOT_FOUND};
    const std::vector<size_t> expected{0U, FIRST_RETRY_INDEX, SECOND_RETRY_INDEX};
    EXPECT_EQ(expected, CollectPutRetryKeys(batchResult));
}

TEST(BatchPutRetryTest, CollectPutRetryKeys_EmptyResult_ReturnsEmpty)
{
    const std::vector<int> batchResult;
    EXPECT_TRUE(CollectPutRetryKeys(batchResult).empty());
}

// Continue defensive retry only if all RPCs in the round succeeded and retryable keys remain.
TEST(BatchPutRetryTest, ShouldContinuePutRetry_ContinuesWhenRoundOkAndKeysRetryable)
{
    EXPECT_TRUE(ShouldContinuePutRetry(MMC_OK, {FIRST_RETRY_INDEX}));
}

TEST(BatchPutRetryTest, ShouldContinuePutRetry_StopsWhenNoRetryableKeys)
{
    EXPECT_FALSE(ShouldContinuePutRetry(MMC_OK, {}));
}

TEST(BatchPutRetryTest, ShouldContinuePutRetry_StopsWhenRoundFailed)
{
    EXPECT_FALSE(ShouldContinuePutRetry(MMC_TIMEOUT, {FIRST_RETRY_INDEX}));
    EXPECT_FALSE(ShouldContinuePutRetry(MMC_ERROR, {FIRST_RETRY_INDEX}));
}

TEST(BatchPutRetryTest, ShouldContinuePutRetry_StopsWhenRoundFailedAndNoRetryableKeys)
{
    EXPECT_FALSE(ShouldContinuePutRetry(MMC_TIMEOUT, {}));
}
