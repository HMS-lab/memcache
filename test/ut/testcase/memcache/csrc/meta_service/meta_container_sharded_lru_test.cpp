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
#include "mmc_meta_container_sharded_lru.cpp"
#include "mmc_mem_obj_meta.h"
#include "mmc_meta_container.h"
#include "gtest/gtest.h"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

using namespace ock::mmc;
using namespace testing;

class TestMmcMetaContainerShardedLRU : public Test {
protected:
    using Container = MmcMetaContainerShardedLRU<std::string, int>;
    std::unique_ptr<Container> container;

    void SetUp() override
    {
        auto getTypeFunc = [](const int &value) -> MediaType { return MEDIA_DRAM; };
        container = std::make_unique<Container>(getTypeFunc, MMC_DEFAULT_META_LRU_SHARD_COUNT);
    }
};

TEST_F(TestMmcMetaContainerShardedLRU, InsertAndGet)
{
    constexpr int valueFirst = 100;
    constexpr int valueSecond = 200;
    EXPECT_EQ(container->Insert("key1", valueFirst), MMC_OK);
    EXPECT_EQ(container->Insert("key2", valueSecond), MMC_OK);
    EXPECT_EQ(container->Insert("key1", valueSecond), MMC_DUPLICATED_OBJECT);

    int value = 0;
    EXPECT_EQ(container->Get("key1", value), MMC_OK);
    EXPECT_EQ(value, valueFirst);
    EXPECT_EQ(container->Get("key2", value), MMC_OK);
    EXPECT_EQ(value, valueSecond);
    EXPECT_EQ(container->Get("key3", value), MMC_UNMATCHED_KEY);
}

TEST_F(TestMmcMetaContainerShardedLRU, Erase)
{
    constexpr int valueFirst = 100;
    constexpr int valueSecond = 200;
    container->Insert("key1", valueFirst);
    container->Insert("key2", valueSecond);

    EXPECT_EQ(container->Erase("key1"), MMC_OK);
    int value = 0;
    EXPECT_EQ(container->Get("key1", value), MMC_UNMATCHED_KEY);
    EXPECT_EQ(container->Erase("key3"), MMC_UNMATCHED_KEY);
    EXPECT_EQ(container->Get("key2", value), MMC_OK);
    EXPECT_EQ(value, valueSecond);
}

TEST_F(TestMmcMetaContainerShardedLRU, GetAllKeysAndIterateIf)
{
    constexpr int keyCount = 50;
    constexpr int valueStep = 10;
    constexpr int valueThreshold = 200;
    for (int i = 0; i < keyCount; ++i) {
        container->Insert(std::string("key") + std::to_string(i), i * valueStep);
    }

    std::vector<std::string> keys;
    container->GetAllKeys(keys);
    EXPECT_EQ(keys.size(), static_cast<size_t>(keyCount));

    std::map<std::string, int> matched;
    container->IterateIf([valueThreshold](const std::string &, const int &value) { return value >= valueThreshold; },
                         matched);
    EXPECT_EQ(matched.size(), static_cast<size_t>(keyCount - valueThreshold / valueStep));
}

TEST_F(TestMmcMetaContainerShardedLRU, EraseIf)
{
    constexpr int keyCount = 40;
    constexpr int eraseBelow = 20;
    for (int i = 0; i < keyCount; ++i) {
        container->Insert(std::string("k") + std::to_string(i), i);
    }
    container->EraseIf([eraseBelow](const std::string &, const int &value) { return value < eraseBelow; });

    std::vector<std::string> keys;
    container->GetAllKeys(keys);
    EXPECT_EQ(keys.size(), static_cast<size_t>(keyCount - eraseBelow));
}

TEST_F(TestMmcMetaContainerShardedLRU, EraseAll)
{
    constexpr int keyCount = 30;
    for (int i = 0; i < keyCount; ++i) {
        container->Insert(std::string("k") + std::to_string(i), i);
    }

    int removedCount = 0;
    container->EraseAll([&removedCount](const std::string &, const int &) { removedCount++; });
    EXPECT_EQ(removedCount, keyCount);

    std::vector<std::string> keys;
    container->GetAllKeys(keys);
    EXPECT_TRUE(keys.empty());
}

TEST_F(TestMmcMetaContainerShardedLRU, FactoryCreatesConfiguredContainer)
{
    using LruContainer = MmcMetaContainerLRU<std::string, int>;
    using ShardedLruContainer = MmcMetaContainerShardedLRU<std::string, int>;
    auto getTypeFunc = [](const int &value) -> MediaType { return MEDIA_DRAM; };
    auto lru = MmcMetaContainerFactory<std::string, int>::Create(MetaLruType::LRU, getTypeFunc,
                                                                 MMC_DEFAULT_META_LRU_SHARD_COUNT);
    auto shardedLru = MmcMetaContainerFactory<std::string, int>::Create(MetaLruType::SHARDED_LRU, getTypeFunc,
                                                                        MMC_DEFAULT_META_LRU_SHARD_COUNT);

    ASSERT_NE(lru.Get(), nullptr);
    ASSERT_NE(shardedLru.Get(), nullptr);
    EXPECT_NE(dynamic_cast<LruContainer *>(lru.Get()), nullptr);
    EXPECT_NE(dynamic_cast<ShardedLruContainer *>(shardedLru.Get()), nullptr);
}

TEST_F(TestMmcMetaContainerShardedLRU, GetGlobalEvictCandidateKeys)
{
    constexpr size_t keyCount = 24;
    constexpr size_t candidateCount = 6;
    for (size_t i = 0; i < keyCount; ++i) {
        ASSERT_EQ(container->Insert("key" + std::to_string(i), static_cast<int>(i)), MMC_OK);
    }

    EXPECT_EQ(container->GetKeyCount(MEDIA_DRAM), keyCount);
    const auto keys = container->GetEvictCandidateKeys(MEDIA_DRAM, candidateCount);
    ASSERT_EQ(keys.size(), candidateCount);
    for (size_t i = 0; i < candidateCount; ++i) {
        EXPECT_EQ(keys[i], "key" + std::to_string(i));
    }
}

TEST_F(TestMmcMetaContainerShardedLRU, MultiLevelEliminationSelectsGlobalOldestKeys)
{
    constexpr int keyCount = 96;
    for (int i = 0; i < keyCount; ++i) {
        ASSERT_EQ(container->Insert("key" + std::to_string(i), i), MMC_OK);
    }

    std::vector<int> evictedValues;
    auto moveFunc = [&evictedValues](const std::string &, const int &value, MediaType) {
        evictedValues.push_back(value);
        return EvictResult::REMOVE;
    };

    // (95 - 80) / 90 of 96 objects is exactly 16.  Keys were inserted in
    // value order, so the global LRU order is value order even though they
    // are distributed across shards.
    std::vector<std::pair<uint16_t, uint16_t>> watermarks(MEDIA_NONE, {0, 0});
    watermarks[MEDIA_DRAM] = {90, 80};
    container->MultiLevelElimination(watermarks, {MEDIA_DRAM}, {95}, moveFunc);

    ASSERT_EQ(evictedValues.size(), 16U);
    std::sort(evictedValues.begin(), evictedValues.end());
    for (int i = 0; i < 16; ++i) {
        EXPECT_EQ(evictedValues[i], i);
    }
}
