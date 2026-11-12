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
#include <limits>

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

// After key recreation, KEEP_SSD parking of the old object must not affect the new object.
TEST_F(TestMmcMetaContainerLRU, InsertLruIfMatchSkipsReplacedObject)
{
    constexpr int valueOld = 100;
    constexpr int valueNew = 200;
    EXPECT_EQ(container->Insert("key1", valueOld), MMC_OK);
    EXPECT_EQ(container->Erase("key1"), MMC_OK);
    EXPECT_EQ(container->Insert("key1", valueNew), MMC_OK);

    // Old object is absent: identity mismatch returns a miss; new object stays in memory LRU.
    EXPECT_EQ(container->InsertLruIfMatch("key1", valueOld, MEDIA_SSD), MMC_UNMATCHED_KEY);
    EXPECT_EQ(container->GetMediaType("key1"), MEDIA_DRAM);

    // Matching identity behaves like InsertLru: park outside memory.
    EXPECT_EQ(container->InsertLruIfMatch("key1", valueNew, MEDIA_SSD), MMC_OK);
    EXPECT_EQ(container->GetMediaType("key1"), MEDIA_NONE);
}

// After key recreation, lazy marking of the old object must not affect the new object.
TEST_F(TestMmcMetaContainerLRU, DemoteIfMatchSkipsReplacedObject)
{
    constexpr int valueOld = 100;
    constexpr int valueNew = 200;
    constexpr size_t keyCount = 2;
    EXPECT_EQ(container->Insert("key1", valueOld), MMC_OK);
    EXPECT_EQ(container->Insert("key2", valueNew), MMC_OK);
    EXPECT_EQ(container->Erase("key1"), MMC_OK);
    EXPECT_EQ(container->Insert("key1", valueNew), MMC_OK); // New object becomes MRU.

    EXPECT_EQ(container->DemoteIfMatch("key1", valueOld), MMC_UNMATCHED_KEY);

    // New object is not lazy: ordinary LRU eviction still evicts older key2 first.
    std::vector<std::string> evictedKeys;
    auto moveFunc = [&evictedKeys](const std::string &key, const int &, MediaType) -> EvictResult {
        evictedKeys.push_back(key);
        return EvictResult::REMOVE;
    };
    for (size_t i = 0; i < keyCount; ++i) {
        ASSERT_TRUE(container->EvictOneLeastRecentlyUsed(moveFunc, MEDIA_DRAM));
    }
    EXPECT_EQ(evictedKeys, (std::vector<std::string>{"key2", "key1"}));
}

// Identity-checked deletion: after key recreation, conditional deletion of the old version must not delete the new
// object.
TEST_F(TestMmcMetaContainerLRU, EraseIfMatchSkipsReplacedObject)
{
    constexpr int valueOld = 100;
    constexpr int valueNew = 200;
    EXPECT_EQ(container->Insert("key1", valueOld), MMC_OK);
    EXPECT_EQ(container->Erase("key1"), MMC_OK);
    EXPECT_EQ(container->Insert("key1", valueNew), MMC_OK);

    // Old object is absent: conditional deletion returns a miss; new object and its LRU position are unchanged.
    EXPECT_EQ(container->EraseIfMatch("key1", valueOld), MMC_UNMATCHED_KEY);
    int value = 0;
    EXPECT_EQ(container->Get("key1", value), MMC_OK);
    EXPECT_EQ(value, valueNew);
    EXPECT_EQ(container->GetMediaType("key1"), MEDIA_DRAM);

    // Delete normally when identity matches.
    EXPECT_EQ(container->EraseIfMatch("key1", valueNew), MMC_OK);
    EXPECT_EQ(container->Get("key1", value), MMC_UNMATCHED_KEY);
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

constexpr int DRAM_BLOB_VALUE = 1;
constexpr int HBM_BLOB_VALUE = 2;
constexpr int SSD_BLOB_VALUE = 3;

class TestLazyDemote : public TestMmcMetaContainerLRU {
protected:
    void InsertKeys(size_t count)
    {
        for (size_t i = 0; i < count; ++i) {
            container->Insert("key" + std::to_string(i), static_cast<int>(i));
        }
    }

    std::vector<std::string> EvictKeys(MediaType mediaType, size_t count)
    {
        std::vector<std::string> evictedKeys;
        auto moveFunc = [&evictedKeys](const std::string &key, const int &, MediaType) -> EvictResult {
            evictedKeys.push_back(key);
            return EvictResult::REMOVE;
        };
        for (size_t i = 0; i < count; ++i) {
            EXPECT_TRUE(container->EvictOneLeastRecentlyUsed(moveFunc, mediaType));
        }
        return evictedKeys;
    }
};

TEST_F(TestLazyDemote, LazyDemoteFifoOrder)
{
    const size_t numKeys = 5;
    InsertKeys(numKeys);
    // LRU list after insertion (front=MRU): [key4, key3, key2, key1, key0].
    EXPECT_EQ(container->Demote("key1"), MMC_OK); // Marked lazy first.
    EXPECT_EQ(container->Demote("key3"), MMC_OK); // Marked lazy later.
    // FIFO: key1 first, then key3, then ordinary LRU order (key0/key2/key4).
    EXPECT_EQ(EvictKeys(MEDIA_DRAM, numKeys), (std::vector<std::string>{"key1", "key3", "key0", "key2", "key4"}));
}

TEST_F(TestLazyDemote, LazyDemoteIdempotent)
{
    const size_t numKeys = 5;
    InsertKeys(numKeys);
    EXPECT_EQ(container->Demote("key1"), MMC_OK);
    // Repeated lazy deletion is idempotent and preserves the first position.
    EXPECT_EQ(container->Demote("key1"), MMC_OK);
    EXPECT_EQ(container->Demote("key3"), MMC_OK);
    // Eviction order remains based on the first lazy deletion, unaffected by repeated Demote.
    EXPECT_EQ(EvictKeys(MEDIA_DRAM, numKeys), (std::vector<std::string>{"key1", "key3", "key0", "key2", "key4"}));
}

// If eviction callback skips (FAIL) due to an active lease, the lazy key returns to the tail with its lazy flag;
// subsequently Demoted keys stay behind it in eviction order, preserving first-marked FIFO.
TEST_F(TestLazyDemote, LazyFlagSurvivesLeaseSkippedEviction)
{
    const size_t numKeys = 5;
    InsertKeys(numKeys);
    EXPECT_EQ(container->Demote("key1"), MMC_OK); // Marked lazy first.
    EXPECT_EQ(container->Demote("key3"), MMC_OK); // Marked lazy later.
    // Simulate EvictCallBackFunction returning FAIL for an active lease: EvictCandidate changes container state before
    // callback.
    Container::EvictionCandidate candidate{"key1", kLazyDemotedAccessSequence};
    auto leaseSkipFunc = [](const std::string &, const int &, MediaType) -> EvictResult { return EvictResult::FAIL; };
    EXPECT_EQ(container->EvictCandidate(candidate, leaseSkipFunc, MEDIA_DRAM), EvictResult::FAIL);
    int value = 0;
    EXPECT_EQ(container->Get("key1", value), MMC_OK); // The key skipped due to a lease remains in the container.

    EXPECT_EQ(container->Demote("key2"), MMC_OK); // Mark key2 lazy after the skip.
    // FIFO still follows first lazy deletion: key1 (previously skipped for lease) -> key3 -> key2 -> ordinary LRU
    // order.
    EXPECT_EQ(EvictKeys(MEDIA_DRAM, numKeys), (std::vector<std::string>{"key1", "key3", "key2", "key0", "key4"}));
}

// If a normal candidate returns FAIL for an active lease while another thread Demotes key2 during the callback,
// rollback must place the normal key before the lazy suffix; otherwise it goes after key2 and is evicted first next
// time.
// Also, lazy-candidate collection with cutoff=0 would break early at the tail and collect no lazy keys.
TEST_F(TestLazyDemote, NormalCandidateFailRestoresBeforeLazySuffix)
{
    const size_t numKeys = 4;
    InsertKeys(numKeys);
    std::vector<Container::EvictionCandidate> collected;
    container->CollectEvictionCandidates(MEDIA_DRAM, std::numeric_limits<uint64_t>::max(), true, collected);
    ASSERT_EQ(collected.size(), numKeys);
    // Collect from tail to head; the first candidate is the coldest normal key.
    const auto victim = collected.front();
    ASSERT_EQ(victim.key, "key0");
    ASSERT_NE(victim.accessSequence, kLazyDemotedAccessSequence);

    auto failAfterConcurrentDemote = [this](const std::string &, const int &, MediaType) -> EvictResult {
        // Simulate another thread marking key2 lazy during the callback.
        EXPECT_EQ(container->Demote("key2"), MMC_OK);
        return EvictResult::FAIL;
    };
    EXPECT_EQ(container->EvictCandidate(victim, failAfterConcurrentDemote, MEDIA_DRAM), EvictResult::FAIL);

    // Lazy key2 must remain the only lazy candidate collected from the tail; normal key0 must not block collection.
    std::vector<Container::EvictionCandidate> lazyCandidates;
    container->CollectEvictionCandidates(MEDIA_DRAM, kLazyDemotedAccessSequence, true, lazyCandidates);
    ASSERT_EQ(lazyCandidates.size(), 1U);
    EXPECT_EQ(lazyCandidates.front().key, "key2");

    // Eviction order: lazy key2 first, then ordinary LRU order.
    EXPECT_EQ(EvictKeys(MEDIA_DRAM, numKeys), (std::vector<std::string>{"key2", "key0", "key1", "key3"}));
}

TEST_F(TestLazyDemote, LazyDemoteThenPromoteRevives)
{
    const size_t numKeys = 5;
    InsertKeys(numKeys);
    EXPECT_EQ(container->Demote("key1"), MMC_OK);
    EXPECT_EQ(container->Demote("key3"), MMC_OK);
    EXPECT_EQ(container->Promote("key1"), MMC_OK); // Access revives the key: clear lazy flag and promote to head.
    // key3 remains first in lazy FIFO; revived key1 is ordinary MRU and evicted last.
    EXPECT_EQ(EvictKeys(MEDIA_DRAM, numKeys), (std::vector<std::string>{"key3", "key0", "key2", "key4", "key1"}));
}

// Lazy-suffix count must match the list after mixed Promote/Erase/InsertLru operations:
// newly Demoted keys must follow existing lazy keys in eviction order and precede normal keys.
TEST_F(TestLazyDemote, LazyFifoAfterMixedOperations)
{
    const size_t numKeys = 6;
    InsertKeys(numKeys);
    EXPECT_EQ(container->Demote("key1"), MMC_OK);  // Lazy queue: key1.
    EXPECT_EQ(container->Demote("key2"), MMC_OK);  // Lazy queue: key1 -> key2.
    EXPECT_EQ(container->Promote("key1"), MMC_OK); // Access revival: lazy queue contains only key2.
    EXPECT_EQ(container->Erase("key3"), MMC_OK);
    EXPECT_EQ(container->InsertLru("key2", MEDIA_NONE), MMC_OK); // Parking outside memory clears the lazy queue.
    EXPECT_EQ(container->Demote("key4"), MMC_OK);                // Lazy queue: key4.
    EXPECT_EQ(container->Demote("key5"), MMC_OK);                // Lazy queue: key4 -> key5.
    // key4 was marked first, then key5, then ordinary LRU order (key0 coldest, key1 hottest).
    const std::vector<std::string> expectedOrder{"key4", "key5", "key0", "key1"};
    EXPECT_EQ(EvictKeys(MEDIA_DRAM, expectedOrder.size()), expectedOrder);
}

// Many consecutive Demotes use O(1) boundary positioning; eviction must still follow first-marked FIFO.
TEST_F(TestLazyDemote, BulkLazyDemoteKeepsFifo)
{
    const size_t numKeys = 200;
    InsertKeys(numKeys);
    std::vector<std::string> demoteOrder;
    for (size_t i = 0; i < numKeys; ++i) {
        std::string key = "key" + std::to_string(i);
        ASSERT_EQ(container->Demote(key), MMC_OK);
        demoteOrder.push_back(key);
    }
    EXPECT_EQ(EvictKeys(MEDIA_DRAM, numKeys), demoteOrder);
}

TEST_F(TestLazyDemote, LazyDemoteMissingKey)
{
    EXPECT_EQ(container->Demote("nonexistent"), MMC_UNMATCHED_KEY);
}

// EraseAll must reset lazy-suffix boundaries with the list; subsequent Demotes still follow FIFO.
TEST_F(TestLazyDemote, LazySuffixBoundaryResetAfterEraseAll)
{
    InsertKeys(4);
    EXPECT_EQ(container->Demote("key1"), MMC_OK);
    EXPECT_EQ(container->Demote("key2"), MMC_OK);
    EXPECT_EQ(container->EraseAll([](const std::string &, const int &) {}), MMC_OK);

    EXPECT_EQ(container->Insert("keyA", 1), MMC_OK);
    EXPECT_EQ(container->Insert("keyB", 2), MMC_OK);
    EXPECT_EQ(container->Demote("keyA"), MMC_OK);
    EXPECT_EQ(container->Demote("keyB"), MMC_OK);
    EXPECT_EQ(EvictKeys(MEDIA_DRAM, 2), (std::vector<std::string>{"keyA", "keyB"}));
}

// After parking outside memory and requeueing, the key is no longer lazy; boundaries must follow the list state.
TEST_F(TestLazyDemote, LazySuffixBoundaryAfterLruMoves)
{
    InsertKeys(5);
    EXPECT_EQ(container->Demote("key1"), MMC_OK);
    EXPECT_EQ(container->Demote("key3"), MMC_OK);
    EXPECT_EQ(container->RemoveFromLru("key3", MEDIA_DRAM), MMC_OK);
    EXPECT_EQ(container->InsertLru("key3", MEDIA_DRAM), MMC_OK);
    EXPECT_EQ(container->Demote("key2"), MMC_OK);

    // key1 was marked first, then key2; key3 returned to head and is evicted last in ordinary LRU order.
    const std::vector<std::string> expected{"key1", "key2", "key0", "key4", "key3"};
    EXPECT_EQ(EvictKeys(MEDIA_DRAM, expected.size()), expected);
}

// Concurrent Demote and InsertLru: Demote must read medium and lruIter_ under lruLock_,
// or InsertLru can park as MEDIA_NONE and erase the node, leaving Demote a dangling iterator (reproducible with ASan).
TEST_F(TestLazyDemote, ConcurrentDemoteAndInsertLruKeepsBoundaryValid)
{
    constexpr size_t kKeyCount = 8;
    constexpr int kIterations = 2000;
    InsertKeys(kKeyCount);

    std::atomic<bool> stop{false};
    std::thread demoter([this, &stop]() {
        for (int i = 0; i < kIterations && !stop.load(std::memory_order_relaxed); ++i) {
            container->Demote("key" + std::to_string(static_cast<size_t>(i) % kKeyCount));
        }
    });
    for (int i = 0; i < kIterations; ++i) {
        const std::string key = "key" + std::to_string(static_cast<size_t>(i) % kKeyCount);
        // Alternate parking outside memory and requeueing to expose Demote's check-then-lock race window.
        container->InsertLru(key, MEDIA_NONE);
        container->InsertLru(key, MEDIA_DRAM);
    }
    stop.store(true, std::memory_order_relaxed);
    demoter.join();

    // State remains consistent after concurrency: all keys are in DRAM; successive evictions cover every key exactly
    // once.
    EXPECT_EQ(container->GetKeyCount(MEDIA_DRAM), kKeyCount);
    auto evicted = EvictKeys(MEDIA_DRAM, kKeyCount);
    ASSERT_EQ(evicted.size(), kKeyCount);
    std::sort(evicted.begin(), evicted.end());
    std::vector<std::string> expected;
    for (size_t i = 0; i < kKeyCount; ++i) {
        expected.push_back("key" + std::to_string(i));
    }
    EXPECT_EQ(evicted, expected);
}

class TestLazyDemoteSsd : public Test {
protected:
    using Container = MmcMetaContainerLRU<std::string, int>;
    std::unique_ptr<Container> container;

    void SetUp() override
    {
        auto GetTypeFunc = [](const int &) -> MediaType { return MEDIA_SSD; };
        container = std::make_unique<Container>(GetTypeFunc);
    }
};

TEST_F(TestLazyDemoteSsd, LazyDemoteSsdKeyReturnsSkipped)
{
    EXPECT_EQ(container->Insert("ssdKey", SSD_BLOB_VALUE), MMC_OK);
    // SSD-parked key is outside memory LRU with no position to demote: return MMC_SKIPPED for upper-layer fallback.
    EXPECT_EQ(container->Demote("ssdKey"), MMC_SKIPPED);
    int value = 0;
    EXPECT_EQ(container->Get("ssdKey", value), MMC_OK);
    EXPECT_EQ(value, SSD_BLOB_VALUE);
}

class TestLazyDemoteMultiMedia : public Test {
protected:
    using Container = MmcMetaContainerLRU<std::string, int>;
    std::unique_ptr<Container> container;

    void SetUp() override
    {
        auto GetTypeFunc = [](const int &value) -> MediaType {
            return value == HBM_BLOB_VALUE ? MEDIA_HBM : MEDIA_DRAM;
        };
        container = std::make_unique<Container>(GetTypeFunc);
    }
};

TEST_F(TestLazyDemoteMultiMedia, LazyFlagClearedOnMoveDown)
{
    // HBM: n1 (normal), kH (lazy); list [n1, kH].
    EXPECT_EQ(container->Insert("n1", HBM_BLOB_VALUE), MMC_OK);
    EXPECT_EQ(container->Insert("kH", HBM_BLOB_VALUE), MMC_OK);
    EXPECT_EQ(container->Demote("kH"), MMC_OK);
    // DRAM: k2 (lazy).
    EXPECT_EQ(container->Insert("k2", DRAM_BLOB_VALUE), MMC_OK);
    EXPECT_EQ(container->Demote("k2"), MMC_OK);

    // Evict k2 with MOVE_DOWN: eviction completes deletion, so clear the lazy flag.
    auto moveDownFunc = [](const std::string &, const int &, MediaType) -> EvictResult {
        return EvictResult::MOVE_DOWN;
    };
    EXPECT_TRUE(container->EvictOneLeastRecentlyUsed(moveDownFunc, MEDIA_DRAM));

    // After moving, InsertLru enters HBM with the flag cleared, inserting at the ordinary MRU head.
    EXPECT_EQ(container->InsertLru("k2", MEDIA_HBM), MMC_OK);

    // HBM eviction order: kH (lazy FIFO) -> n1 (ordinary LRU) -> k2 (new-tier MRU, last).
    // If the flag remained, k2 would enter at the lazy boundary, yielding kH -> k2 -> n1; this distinguishes the
    // behavior.
    std::vector<std::string> evictedKeys;
    auto removeFunc = [&evictedKeys](const std::string &key, const int &, MediaType) -> EvictResult {
        evictedKeys.push_back(key);
        return EvictResult::REMOVE;
    };
    EXPECT_TRUE(container->EvictOneLeastRecentlyUsed(removeFunc, MEDIA_HBM));
    EXPECT_TRUE(container->EvictOneLeastRecentlyUsed(removeFunc, MEDIA_HBM));
    EXPECT_TRUE(container->EvictOneLeastRecentlyUsed(removeFunc, MEDIA_HBM));
    EXPECT_EQ(evictedKeys, (std::vector<std::string>{"kH", "n1", "k2"}));
}

TEST_F(TestLazyDemoteMultiMedia, LazyFlagClearedOnParkToSsdAndRewarm)
{
    // DRAM: n1 (normal), k1 (lazy).
    EXPECT_EQ(container->Insert("n1", DRAM_BLOB_VALUE), MMC_OK);
    EXPECT_EQ(container->Insert("k1", DRAM_BLOB_VALUE), MMC_OK);
    EXPECT_EQ(container->Demote("k1"), MMC_OK);

    // KEEP_SSD parking: InsertLru(key, SSD) leaves memory LRU and must clear lazy_.
    EXPECT_EQ(container->InsertLru("k1", MEDIA_SSD), MMC_OK);
    // Rewarming: InsertLru(key, DRAM) returns to memory as MRU head and must clear lazy_.
    EXPECT_EQ(container->InsertLru("k1", MEDIA_DRAM), MMC_OK);

    // Marking lazy again: a stale lazy_ would take the idempotent branch without repositioning, leaving k1 at MRU head.
    EXPECT_EQ(container->Demote("k1"), MMC_OK);

    // Eviction order: k1 (lazy FIFO first) -> n1 (ordinary LRU).
    std::vector<std::string> evictedKeys;
    auto removeFunc = [&evictedKeys](const std::string &key, const int &, MediaType) -> EvictResult {
        evictedKeys.push_back(key);
        return EvictResult::REMOVE;
    };
    EXPECT_TRUE(container->EvictOneLeastRecentlyUsed(removeFunc, MEDIA_DRAM));
    EXPECT_TRUE(container->EvictOneLeastRecentlyUsed(removeFunc, MEDIA_DRAM));
    EXPECT_EQ(evictedKeys, (std::vector<std::string>{"k1", "n1"}));
}

TEST_F(TestLazyDemoteMultiMedia, LazyFlagClearedOnRemoveFromLruAndRewarm)
{
    // DRAM: n1 (normal), k1 (lazy).
    EXPECT_EQ(container->Insert("n1", DRAM_BLOB_VALUE), MMC_OK);
    EXPECT_EQ(container->Insert("k1", DRAM_BLOB_VALUE), MMC_OK);
    EXPECT_EQ(container->Demote("k1"), MMC_OK);

    // Manual eviction parking: RemoveFromLru leaves memory LRU and must clear lazy_.
    EXPECT_EQ(container->RemoveFromLru("k1", MEDIA_DRAM), MMC_OK);
    // Rewarming: InsertLru(key, DRAM) returns to memory.
    EXPECT_EQ(container->InsertLru("k1", MEDIA_DRAM), MMC_OK);

    // Marking lazy again must reposition normally into the lazy suffix.
    EXPECT_EQ(container->Demote("k1"), MMC_OK);

    // Eviction order: k1 (lazy FIFO first) -> n1 (ordinary LRU).
    std::vector<std::string> evictedKeys;
    auto removeFunc = [&evictedKeys](const std::string &key, const int &, MediaType) -> EvictResult {
        evictedKeys.push_back(key);
        return EvictResult::REMOVE;
    };
    EXPECT_TRUE(container->EvictOneLeastRecentlyUsed(removeFunc, MEDIA_DRAM));
    EXPECT_TRUE(container->EvictOneLeastRecentlyUsed(removeFunc, MEDIA_DRAM));
    EXPECT_EQ(evictedKeys, (std::vector<std::string>{"k1", "n1"}));
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
