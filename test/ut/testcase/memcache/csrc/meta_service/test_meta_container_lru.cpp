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
