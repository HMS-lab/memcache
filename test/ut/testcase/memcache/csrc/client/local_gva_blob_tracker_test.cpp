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
#include <thread>
#include <gtest/gtest.h>

#include "mmc_client_local_gva_blob_tracker.h"
#include "mmc_montotonic.h"

using namespace testing;
using namespace ock::mmc;

namespace {
constexpr uint64_t testGva = 4096;
constexpr uint64_t testSize = 4096;
constexpr uint32_t testRank = 0;
constexpr uint64_t testLeaseTtlMs = 5000;
constexpr uint64_t testOperateId = 1;
constexpr uint32_t promoteWaitMs = 10;
constexpr uint64_t secondaryGva1 = testGva + testSize;
constexpr uint64_t secondaryGva2 = testGva + testSize * 2;
constexpr uint64_t kSecondOperateId = 2;
constexpr size_t kOnePendingOperation = 1;

uint64_t NowMs()
{
    return ock::dagger::Monotonic::TimeUs() / 1000ULL;
}
} // namespace

class TestLocalGvaBlobTracker : public Test {
protected:
    LocalGvaBlobTracker tracker;
    std::string key = "tracker_test_key";

    void RegisterBlob(const std::string &blobKey, uint64_t gva = testGva)
    {
        MmcMemBlobDesc desc{testRank, gva, testSize, MEDIA_DRAM, ALLOCATED, testLeaseTtlMs};
        ASSERT_EQ(tracker.RegisterFromBatchAlloc(blobKey, desc, testOperateId), MMC_OK);
    }
};

TEST_F(TestLocalGvaBlobTracker, RegisterFromBatchAlloc_SetsLastPromoteMs)
{
    const uint64_t beforeMs = NowMs();
    RegisterBlob(key);
    LocalGvaBlobInfo info;
    EXPECT_EQ(tracker.FindWritable(testGva, testSize, info), MMC_OK);
    EXPECT_GE(info.lastPromoteMs, beforeMs);
}

TEST_F(TestLocalGvaBlobTracker, RegisterFromBatchAlloc_RejectsInvalidAndSsdBlobs)
{
    const std::vector<MmcMemBlobDesc> invalidBlobs{
        {testRank, 0, testSize, MEDIA_DRAM, ALLOCATED, testLeaseTtlMs},
        {testRank, UINT64_MAX, testSize, MEDIA_DRAM, ALLOCATED, testLeaseTtlMs},
        {testRank, testGva, 0, MEDIA_DRAM, ALLOCATED, testLeaseTtlMs},
        {testRank, testGva, testSize, MEDIA_SSD, READABLE, testLeaseTtlMs},
    };
    for (const auto &blob : invalidBlobs) {
        EXPECT_EQ(tracker.RegisterFromBatchAlloc(key, blob, testOperateId), MMC_INVALID_PARAM);
        LocalGvaBlobInfo info;
        EXPECT_EQ(tracker.FindBlobByKey(key, info), MMC_UNMATCHED_KEY);
    }

    RegisterBlob(key);
    LocalGvaBlobInfo info;
    EXPECT_EQ(tracker.FindWritable(testGva, testSize, info), MMC_OK);
}

TEST_F(TestLocalGvaBlobTracker, MarkPromoted_UpdatesLastPromoteMs)
{
    RegisterBlob(key);

    LocalGvaBlobInfo info1;
    ASSERT_EQ(tracker.FindWritable(testGva, testSize, info1), MMC_OK);
    const uint64_t initialMs = info1.lastPromoteMs;

    std::this_thread::sleep_for(std::chrono::milliseconds(promoteWaitMs));

    tracker.MarkPromoted({key});

    LocalGvaBlobInfo info2;
    ASSERT_EQ(tracker.FindWritable(testGva, testSize, info2), MMC_OK);
    EXPECT_GT(info2.lastPromoteMs, initialMs);
}

TEST_F(TestLocalGvaBlobTracker, MarkPromoted_NonExistentKey_NoCrash)
{
    tracker.MarkPromoted({"non_existent_key"});
    SUCCEED();
}

TEST_F(TestLocalGvaBlobTracker, MarkPromoted_EmptyKeys_NoCrash)
{
    tracker.MarkPromoted({});
    SUCCEED();
}

TEST_F(TestLocalGvaBlobTracker, FindReadable_ReturnsLastPromoteMs)
{
    const uint64_t beforeMs = NowMs();
    RegisterBlob(key);
    tracker.MarkWriteSuccess(key);

    LocalGvaBlobInfo info;
    EXPECT_EQ(tracker.FindReadable(testGva, testSize, info), MMC_OK);
    EXPECT_GE(info.lastPromoteMs, beforeMs);
}

TEST_F(TestLocalGvaBlobTracker, FindBlobByKey_ReturnsLastPromoteMs)
{
    const uint64_t beforeMs = NowMs();
    RegisterBlob(key);

    LocalGvaBlobInfo info;
    EXPECT_EQ(tracker.FindBlobByKey(key, info), MMC_OK);
    EXPECT_GE(info.lastPromoteMs, beforeMs);
}

TEST_F(TestLocalGvaBlobTracker, MarkPromoted_MultipleKeys)
{
    const std::string key1 = "multi_key_1";
    const std::string key2 = "multi_key_2";

    RegisterBlob(key1, secondaryGva1);
    RegisterBlob(key2, secondaryGva2);

    LocalGvaBlobInfo info1a;
    ASSERT_EQ(tracker.FindWritable(secondaryGva1, testSize, info1a), MMC_OK);
    const uint64_t initialMs1 = info1a.lastPromoteMs;

    LocalGvaBlobInfo info2a;
    ASSERT_EQ(tracker.FindWritable(secondaryGva2, testSize, info2a), MMC_OK);
    const uint64_t initialMs2 = info2a.lastPromoteMs;

    std::this_thread::sleep_for(std::chrono::milliseconds(promoteWaitMs));

    tracker.MarkPromoted({key1, key2, "non_existent"});

    LocalGvaBlobInfo info1b;
    ASSERT_EQ(tracker.FindWritable(secondaryGva1, testSize, info1b), MMC_OK);
    EXPECT_GT(info1b.lastPromoteMs, initialMs1);

    LocalGvaBlobInfo info2b;
    ASSERT_EQ(tracker.FindWritable(secondaryGva2, testSize, info2b), MMC_OK);
    EXPECT_GT(info2b.lastPromoteMs, initialMs2);
}

TEST_F(TestLocalGvaBlobTracker, ReleaseLease_LastWriteRemovesBothIndexesImmediately)
{
    RegisterBlob(key);

    EXPECT_EQ(tracker.ReleaseLease(key), testOperateId);
    LocalGvaBlobInfo info;
    EXPECT_EQ(tracker.FindBlobByKey(key, info), MMC_UNMATCHED_KEY);
    EXPECT_EQ(tracker.FindWritable(testGva, testSize, info), MMC_UNMATCHED_KEY);
    EXPECT_EQ(tracker.ReleaseLease(key), UINT64_MAX);

    // Both the old address and the old key can be registered again without a cleanup scan.
    RegisterBlob("reused_address", testGva);
    RegisterBlob(key, secondaryGva1);
}

TEST_F(TestLocalGvaBlobTracker, ReleaseLease_LastReadRemovesBothIndexesImmediately)
{
    const MmcMemBlobDesc desc{testRank, testGva, testSize, MEDIA_DRAM, READABLE, testLeaseTtlMs};
    ASSERT_EQ(tracker.UpdateFromQuery(key, desc, testOperateId, NowMs() + testLeaseTtlMs), MMC_OK);

    EXPECT_EQ(tracker.ReleaseLease(key), testOperateId);
    LocalGvaBlobInfo info;
    EXPECT_EQ(tracker.FindBlobByKey(key, info), MMC_UNMATCHED_KEY);
    EXPECT_EQ(tracker.FindReadable(testGva, testSize, info), MMC_UNMATCHED_KEY);
    RegisterBlob("reused_read_address", testGva);
}

TEST_F(TestLocalGvaBlobTracker, ReleaseLease_KeepsIntervalUntilLastOperation)
{
    for (const auto state : {ALLOCATED, READABLE}) {
        const MmcMemBlobDesc desc{testRank, testGva, testSize, MEDIA_DRAM, state, testLeaseTtlMs};
        ASSERT_EQ(tracker.RegisterFromBatchAlloc(key, desc, testOperateId), MMC_OK);
        ASSERT_EQ(tracker.RegisterFromBatchAlloc(key, desc, kSecondOperateId), MMC_OK);

        EXPECT_EQ(tracker.ReleaseLease(key), testOperateId);
        LocalGvaBlobInfo info;
        ASSERT_EQ(tracker.FindBlobByKey(key, info), MMC_OK);
        ASSERT_EQ(info.operateQueue.size(), kOnePendingOperation);
        EXPECT_EQ(info.operateQueue.front(), kSecondOperateId);
        if (state == READABLE) {
            EXPECT_EQ(tracker.FindReadable(testGva, testSize, info), MMC_OK);
        } else {
            EXPECT_EQ(tracker.FindWritable(testGva, testSize, info), MMC_OK);
        }

        EXPECT_EQ(tracker.ReleaseLease(key), kSecondOperateId);
        EXPECT_EQ(tracker.FindBlobByKey(key, info), MMC_UNMATCHED_KEY);
        EXPECT_EQ(tracker.FindReadable(testGva, testSize, info), MMC_UNMATCHED_KEY);
        EXPECT_EQ(tracker.FindWritable(testGva, testSize, info), MMC_UNMATCHED_KEY);
    }
}
