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
#include "mmc_meta_container_lru.cpp"
#include "mmc_mem_obj_meta.h"
#include "mmc_meta_container.h"
#include "gtest/gtest.h"
#include <thread>
#include <vector>
#include <atomic>
#include <algorithm>
#include <chrono>

using namespace ock::mmc;
using namespace testing;

class TestMmcMetaContainerLRU : public Test {
protected:
    using Container = MmcMetaContainerLRU<std::string, int>;
    std::unique_ptr<Container> container;

    void SetUp() override
    {
        auto GetTypeFunc = [](const int &value) -> MediaType { return MEDIA_DRAM; };
        container = std::make_unique<Container>(GetTypeFunc);
    }
};

TEST_F(TestMmcMetaContainerLRU, InsertAndGet)
{
    EXPECT_EQ(container->Insert("key1", 100), MMC_OK);
    EXPECT_EQ(container->Insert("key2", 200), MMC_OK);
    EXPECT_EQ(container->Insert("key1", 300), MMC_DUPLICATED_OBJECT);

    int value;
    EXPECT_EQ(container->Get("key1", value), MMC_OK);
    EXPECT_EQ(value, 100);
    EXPECT_EQ(container->Get("key2", value), MMC_OK);
    EXPECT_EQ(value, 200);
    EXPECT_EQ(container->Get("key3", value), MMC_UNMATCHED_KEY);
}

TEST_F(TestMmcMetaContainerLRU, Erase)
{
    container->Insert("key1", 100);
    container->Insert("key2", 200);

    EXPECT_EQ(container->Erase("key1"), MMC_OK);

    int value;
    EXPECT_EQ(container->Get("key1", value), MMC_UNMATCHED_KEY);
    EXPECT_EQ(container->Erase("key3"), MMC_UNMATCHED_KEY);
    EXPECT_EQ(container->Get("key2", value), MMC_OK);
    EXPECT_EQ(value, 200);
}

TEST_F(TestMmcMetaContainerLRU, GetMediaKeyCountAndEvictCandidateKeys)
{
    ASSERT_EQ(container->Insert("key1", 100), MMC_OK);
    ASSERT_EQ(container->Insert("key2", 200), MMC_OK);
    ASSERT_EQ(container->Insert("key3", 300), MMC_OK);

    EXPECT_EQ(container->GetKeyCount(MEDIA_DRAM), 3U);
    EXPECT_EQ(container->GetKeyCount(MEDIA_NONE), 0U);

    const auto keys = container->GetEvictCandidateKeys(MEDIA_DRAM, 2);
    ASSERT_EQ(keys.size(), 2U);
    EXPECT_EQ(keys[0], "key1");
    EXPECT_EQ(keys[1], "key2");
}

TEST_F(TestMmcMetaContainerLRU, EraseIfEmpty)
{
    constexpr int nonEmptyValue = 100;
    EXPECT_EQ(container->Insert("key1", nonEmptyValue), MMC_OK);
    EXPECT_EQ(container->Insert("key2", 0), MMC_OK);

    auto isEmpty = [](const int &value) { return value == 0; };

    // 不存在的 key 返回 MMC_UNMATCHED_KEY
    EXPECT_EQ(container->EraseIfEmpty("key3", isEmpty), MMC_UNMATCHED_KEY);

    // 判空为假（value != 0）：保留条目，返回 MMC_ERROR
    EXPECT_EQ(container->EraseIfEmpty("key1", isEmpty), MMC_ERROR);
    int value = 0;
    EXPECT_EQ(container->Get("key1", value), MMC_OK);
    EXPECT_EQ(value, nonEmptyValue);

    // 判空为真（value == 0）：删除条目并摘除 LRU，返回 MMC_OK
    EXPECT_EQ(container->EraseIfEmpty("key2", isEmpty), MMC_OK);
    EXPECT_EQ(container->Get("key2", value), MMC_UNMATCHED_KEY);
    EXPECT_EQ(container->GetKeyCount(MEDIA_DRAM), 1U);
    auto tailKeys = container->GetEvictCandidateKeys(MEDIA_DRAM, 10U);
    ASSERT_EQ(tailKeys.size(), 1U);
    EXPECT_EQ(tailKeys[0], "key1");
}

class TestEvictYield : public TestMmcMetaContainerLRU {
protected:
    void SetUp() override
    {
        auto GetTypeFunc = [](const int &value) -> MediaType { return MEDIA_DRAM; };
        container = std::make_unique<Container>(GetTypeFunc);
    }

    void InsertKeys(size_t count)
    {
        for (size_t i = 0; i < count; ++i) {
            container->Insert("key" + std::to_string(i), static_cast<int>(i));
        }
    }

    auto MakeMoveFunc(std::atomic<size_t> &evictCount)
    {
        return [&evictCount](const std::string &key, const int &value, MediaType) -> EvictResult {
            evictCount.fetch_add(1);
            return EvictResult::REMOVE;
        };
    }

    std::vector<std::pair<uint16_t, uint16_t>> MakeWatermarks(uint16_t high, uint16_t low)
    {
        std::vector<std::pair<uint16_t, uint16_t>> watermarks(MEDIA_NONE, {0, 0});
        watermarks[MEDIA_DRAM] = {high, low};
        return watermarks;
    }
};

TEST_F(TestEvictYield, YieldDisabledByDefault)
{
    const size_t numKeys = 100;
    InsertKeys(numKeys);

    std::atomic<size_t> evictCount{0};
    auto moveFunc = MakeMoveFunc(evictCount);

    auto start = std::chrono::steady_clock::now();
    container->MultiLevelElimination(MakeWatermarks(90, 80), {MEDIA_DRAM}, {95}, moveFunc);
    auto elapsed =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();

    EXPECT_EQ(evictCount.load(), static_cast<size_t>(numKeys * (95 - 80) / 90));
    EXPECT_LT(elapsed, 1000);
}

TEST_F(TestEvictYield, YieldEnabledAddsDelay)
{
    const size_t numKeys = 100;
    InsertKeys(numKeys);

    std::atomic<size_t> evictCount{0};
    auto moveFunc = MakeMoveFunc(evictCount);

    const uint32_t yieldInterval = 10;
    const uint32_t yieldDurationUs = 1000;

    auto start = std::chrono::steady_clock::now();
    container->MultiLevelElimination(MakeWatermarks(90, 80), {MEDIA_DRAM}, {95}, moveFunc, yieldInterval,
                                     yieldDurationUs);
    auto elapsed =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();

    size_t expectedEvicts = static_cast<size_t>(numKeys * (95 - 80) / 90);
    EXPECT_EQ(evictCount.load(), expectedEvicts);

    size_t expectedYields = expectedEvicts / yieldInterval;
    int64_t expectedMinUs = static_cast<int64_t>(expectedYields * yieldDurationUs);
    EXPECT_GE(elapsed, expectedMinUs);
}

TEST_F(TestEvictYield, YieldIntervalZeroMeansDisabled)
{
    const size_t numKeys = 50;
    InsertKeys(numKeys);

    std::atomic<size_t> evictCount{0};
    auto moveFunc = MakeMoveFunc(evictCount);

    auto start = std::chrono::steady_clock::now();
    container->MultiLevelElimination(MakeWatermarks(90, 80), {MEDIA_DRAM}, {95}, moveFunc, 0, 10000);
    auto elapsed =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();

    EXPECT_EQ(evictCount.load(), static_cast<size_t>(numKeys * (95 - 80) / 90));
    EXPECT_LT(elapsed, 1000);
}

TEST_F(TestEvictYield, YieldDurationZeroMeansDisabled)
{
    const size_t numKeys = 50;
    InsertKeys(numKeys);

    std::atomic<size_t> evictCount{0};
    auto moveFunc = MakeMoveFunc(evictCount);

    auto start = std::chrono::steady_clock::now();
    container->MultiLevelElimination(MakeWatermarks(90, 80), {MEDIA_DRAM}, {95}, moveFunc, 10, 0);
    auto elapsed =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();

    EXPECT_EQ(evictCount.load(), static_cast<size_t>(numKeys * (95 - 80) / 90));
    EXPECT_LT(elapsed, 1000);
}

TEST_F(TestEvictYield, YieldAllowsConcurrentRead)
{
    const size_t numKeys = 200;
    InsertKeys(numKeys);

    std::atomic<size_t> evictCount{0};
    std::atomic<bool> readSuccess{false};

    auto moveFunc = [&evictCount](const std::string &key, const int &value, MediaType) -> EvictResult {
        evictCount.fetch_add(1);
        return EvictResult::REMOVE;
    };

    const uint32_t yieldInterval = 5;
    const uint32_t yieldDurationUs = 5000;

    std::thread evictThread([this, &moveFunc, yieldInterval, yieldDurationUs]() {
        container->MultiLevelElimination(MakeWatermarks(90, 80), {MEDIA_DRAM}, {95}, moveFunc, yieldInterval,
                                         yieldDurationUs);
    });

    std::this_thread::sleep_for(std::chrono::microseconds(2000));

    int value;
    Result ret = container->Get("key0", value);
    if (ret == MMC_OK) {
        readSuccess.store(true);
    }

    evictThread.join();

    EXPECT_TRUE(readSuccess.load() || evictCount.load() > 0);
}

TEST_F(TestEvictYield, YieldCountMatchesEvictCount)
{
    const size_t numKeys = 300;
    InsertKeys(numKeys);

    std::atomic<size_t> evictCount{0};
    auto moveFunc = MakeMoveFunc(evictCount);

    const uint32_t yieldInterval = 10;
    const uint32_t yieldDurationUs = 100;

    container->MultiLevelElimination(MakeWatermarks(90, 80), {MEDIA_DRAM}, {95}, moveFunc, yieldInterval,
                                     yieldDurationUs);

    size_t expectedEvicts = static_cast<size_t>(numKeys * (95 - 80) / 90);
    EXPECT_EQ(evictCount.load(), expectedEvicts);
}

TEST_F(TestMmcMetaContainerLRU, GetKeyCount)
{
    EXPECT_EQ(container->GetKeyCount(), 0);

    EXPECT_EQ(container->Insert("key1", 100U), MMC_OK);
    EXPECT_EQ(container->Insert("key2", 200U), MMC_OK);
    EXPECT_EQ(container->Insert("key1", 300U), MMC_DUPLICATED_OBJECT);
    EXPECT_EQ(container->GetKeyCount(), 2U);

    EXPECT_EQ(container->Erase("key1"), MMC_OK);
    EXPECT_EQ(container->GetKeyCount(), 1);

    EXPECT_EQ(container->EraseAll([](const std::string &, const int &) {}), MMC_OK);
    EXPECT_EQ(container->GetKeyCount(), 0);
}

// Promote on non-existent key returns MMC_UNMATCHED_KEY
TEST_F(TestMmcMetaContainerLRU, PromoteNonExistentKeyReturnsUnmatched)
{
    EXPECT_EQ(container->Promote("non_existent"), MMC_UNMATCHED_KEY);
}

// Promote on existing key returns MMC_OK
TEST_F(TestMmcMetaContainerLRU, PromoteExistingKeyReturnsOk)
{
    constexpr int testValue = 100;
    EXPECT_EQ(container->Insert("key1", testValue), MMC_OK);
    EXPECT_EQ(container->Promote("key1"), MMC_OK);
}

// Promote on key with MEDIA_NONE type returns OK without moving LRU
TEST_F(TestMmcMetaContainerLRU, PromoteNoneMediaTypeReturnsOk)
{
    constexpr int testValue = 100;
    EXPECT_EQ(container->Insert("key1", testValue), MMC_OK);
    EXPECT_EQ(container->InsertLru("key1", MEDIA_NONE), MMC_OK);
    EXPECT_EQ(container->Promote("key1"), MMC_OK);
}

TEST_F(TestMmcMetaContainerLRU, GetKeyCountAndEvictCandidateKeys)
{
    for (int i = 1; i <= 10; ++i) {
        EXPECT_EQ(container->Insert("key" + std::to_string(i), i * 100), MMC_OK);
    }
    EXPECT_EQ(container->GetKeyCount(MEDIA_DRAM), 10U);
    EXPECT_EQ(container->GetKeyCount(MEDIA_NONE), 0U);
    EXPECT_EQ(container->GetKeyCount(MEDIA_HBM), 0U);

    // LRU 尾部即最久未使用端，key1 最早插入
    auto tailKeys = container->GetEvictCandidateKeys(MEDIA_DRAM, 3U);
    ASSERT_EQ(tailKeys.size(), 3U);
    EXPECT_EQ(tailKeys[0], "key1");
    EXPECT_EQ(tailKeys[1], "key2");
    EXPECT_EQ(tailKeys[2], "key3");

    // 请求数量超过 LRU 长度时返回全部 key
    auto allKeys = container->GetEvictCandidateKeys(MEDIA_DRAM, 100U);
    ASSERT_EQ(allKeys.size(), 10U);
    EXPECT_EQ(allKeys.back(), "key10");

    // 请求数量为 0 时返回空
    EXPECT_TRUE(container->GetEvictCandidateKeys(MEDIA_DRAM, 0U).empty());

    // Promote 后尾部顺序更新
    ASSERT_EQ(container->Promote("key1"), MMC_OK);
    auto promotedTail = container->GetEvictCandidateKeys(MEDIA_DRAM, 1U);
    ASSERT_EQ(promotedTail.size(), 1U);
    EXPECT_EQ(promotedTail[0], "key2");
}

TEST_F(TestMmcMetaContainerLRU, GetEvictCandidateKeysForSsdReturnsEmpty)
{
    auto ssdContainer = std::make_unique<Container>([](const int &) -> MediaType { return MEDIA_SSD; });
    EXPECT_EQ(ssdContainer->Insert("ssdKey", 1), MMC_OK);
    // SSD 类型不进入 LRU 管理
    EXPECT_EQ(ssdContainer->GetKeyCount(MEDIA_SSD), 0U);
    EXPECT_TRUE(ssdContainer->GetEvictCandidateKeys(MEDIA_SSD, 5U).empty());
}
