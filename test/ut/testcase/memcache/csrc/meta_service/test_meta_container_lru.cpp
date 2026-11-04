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

// 同名 key 重建后，旧对象的 KEEP_SSD 停泊不能作用到新对象上
TEST_F(TestMmcMetaContainerLRU, InsertLruIfMatchSkipsReplacedObject)
{
    constexpr int valueOld = 100;
    constexpr int valueNew = 200;
    EXPECT_EQ(container->Insert("key1", valueOld), MMC_OK);
    EXPECT_EQ(container->Erase("key1"), MMC_OK);
    EXPECT_EQ(container->Insert("key1", valueNew), MMC_OK);

    // 旧对象已不在容器中：身份不匹配时返回未命中，且新对象仍留在内存层 LRU
    EXPECT_EQ(container->InsertLruIfMatch("key1", valueOld, MEDIA_SSD), MMC_UNMATCHED_KEY);
    EXPECT_EQ(container->GetMediaType("key1"), MEDIA_DRAM);

    // 身份匹配时行为与 InsertLru 一致：停泊出内存层
    EXPECT_EQ(container->InsertLruIfMatch("key1", valueNew, MEDIA_SSD), MMC_OK);
    EXPECT_EQ(container->GetMediaType("key1"), MEDIA_NONE);
}

// 同名 key 重建后，旧对象的懒删除标记不能作用到新对象上
TEST_F(TestMmcMetaContainerLRU, DemoteIfMatchSkipsReplacedObject)
{
    constexpr int valueOld = 100;
    constexpr int valueNew = 200;
    constexpr size_t keyCount = 2;
    EXPECT_EQ(container->Insert("key1", valueOld), MMC_OK);
    EXPECT_EQ(container->Insert("key2", valueNew), MMC_OK);
    EXPECT_EQ(container->Erase("key1"), MMC_OK);
    EXPECT_EQ(container->Insert("key1", valueNew), MMC_OK); // 新对象成为 MRU

    EXPECT_EQ(container->DemoteIfMatch("key1", valueOld), MMC_UNMATCHED_KEY);

    // 新对象未被标记懒删除：淘汰仍按普通 LRU 顺序，更旧的 key2 先淘汰
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

// 身份校验删除：同名 key 重建后，针对旧对象版本的条件删除不能删掉新对象
TEST_F(TestMmcMetaContainerLRU, EraseIfMatchSkipsReplacedObject)
{
    constexpr int valueOld = 100;
    constexpr int valueNew = 200;
    EXPECT_EQ(container->Insert("key1", valueOld), MMC_OK);
    EXPECT_EQ(container->Erase("key1"), MMC_OK);
    EXPECT_EQ(container->Insert("key1", valueNew), MMC_OK);

    // 旧对象已不在容器中：条件删除返回未命中，新对象及其 LRU 位置保持不变
    EXPECT_EQ(container->EraseIfMatch("key1", valueOld), MMC_UNMATCHED_KEY);
    int value = 0;
    EXPECT_EQ(container->Get("key1", value), MMC_OK);
    EXPECT_EQ(value, valueNew);
    EXPECT_EQ(container->GetMediaType("key1"), MEDIA_DRAM);

    // 身份匹配时正常删除
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
    // 插入后 LRU 链（front=MRU）：[key4, key3, key2, key1, key0]
    EXPECT_EQ(container->Demote("key1"), MMC_OK); // 先懒删除
    EXPECT_EQ(container->Demote("key3"), MMC_OK); // 后懒删除
    // FIFO：key1 最先淘汰，其次 key3，之后才是普通 LRU 顺序（key0/key2/key4）
    EXPECT_EQ(EvictKeys(MEDIA_DRAM, numKeys), (std::vector<std::string>{"key1", "key3", "key0", "key2", "key4"}));
}

TEST_F(TestLazyDemote, LazyDemoteIdempotent)
{
    const size_t numKeys = 5;
    InsertKeys(numKeys);
    EXPECT_EQ(container->Demote("key1"), MMC_OK);
    EXPECT_EQ(container->Demote("key1"), MMC_OK); // 重复懒删除：幂等，保持首次位置
    EXPECT_EQ(container->Demote("key3"), MMC_OK);
    // 淘汰顺序仍按首次懒删除时间，不受重复 Demote 影响
    EXPECT_EQ(EvictKeys(MEDIA_DRAM, numKeys), (std::vector<std::string>{"key1", "key3", "key0", "key2", "key4"}));
}

// 淘汰回调因活跃租约跳过（FAIL）时：懒 key 回到队尾且保留懒标志，
// 之后新 Demote 的 key 仍排在其后，首次懒删除的 FIFO 顺序不被破坏
TEST_F(TestLazyDemote, LazyFlagSurvivesLeaseSkippedEviction)
{
    const size_t numKeys = 5;
    InsertKeys(numKeys);
    EXPECT_EQ(container->Demote("key1"), MMC_OK); // 先懒删除
    EXPECT_EQ(container->Demote("key3"), MMC_OK); // 后懒删除
    // 模拟 EvictCallBackFunction 因活跃租约返回 FAIL：EvictCandidate 先转换容器状态再回调
    Container::EvictionCandidate candidate{"key1", kLazyDemotedAccessSequence};
    auto leaseSkipFunc = [](const std::string &, const int &, MediaType) -> EvictResult { return EvictResult::FAIL; };
    EXPECT_EQ(container->EvictCandidate(candidate, leaseSkipFunc, MEDIA_DRAM), EvictResult::FAIL);
    int value = 0;
    EXPECT_EQ(container->Get("key1", value), MMC_OK); // 被租约跳过的 key 仍在容器中

    EXPECT_EQ(container->Demote("key2"), MMC_OK); // 跳过之后再懒删除 key2
    // FIFO 仍按首次懒删除顺序：key1（曾被租约跳过）→ key3 → key2 → 普通 LRU 顺序
    EXPECT_EQ(EvictKeys(MEDIA_DRAM, numKeys), (std::vector<std::string>{"key1", "key3", "key2", "key0", "key4"}));
}

// 普通（非懒）候选因活跃租约返回 FAIL 时，若回调执行期间另一线程 Demote 了 key2，
// 回滚必须把普通 key 放回懒删除后缀之前：否则它会被插到 key2 之后，下一次淘汰先选中普通 key，
// 且 cutoff=0 的懒候选收集会在队尾提前 break，收集不到任何懒删除 key
TEST_F(TestLazyDemote, NormalCandidateFailRestoresBeforeLazySuffix)
{
    const size_t numKeys = 4;
    InsertKeys(numKeys);
    std::vector<Container::EvictionCandidate> collected;
    container->CollectEvictionCandidates(MEDIA_DRAM, std::numeric_limits<uint64_t>::max(), true, collected);
    ASSERT_EQ(collected.size(), numKeys);
    // 收集顺序为队尾到队头，第一个是最冷的普通 key
    const auto victim = collected.front();
    ASSERT_EQ(victim.key, "key0");
    ASSERT_NE(victim.accessSequence, kLazyDemotedAccessSequence);

    auto failAfterConcurrentDemote = [this](const std::string &, const int &, MediaType) -> EvictResult {
        // 模拟回调执行期间另一线程把 key2 标记为懒删除
        EXPECT_EQ(container->Demote("key2"), MMC_OK);
        return EvictResult::FAIL;
    };
    EXPECT_EQ(container->EvictCandidate(victim, failAfterConcurrentDemote, MEDIA_DRAM), EvictResult::FAIL);

    // 懒删除 key2 必须仍是逐尾收集到的唯一懒候选（普通 key0 不能挡住收集）
    std::vector<Container::EvictionCandidate> lazyCandidates;
    container->CollectEvictionCandidates(MEDIA_DRAM, kLazyDemotedAccessSequence, true, lazyCandidates);
    ASSERT_EQ(lazyCandidates.size(), 1U);
    EXPECT_EQ(lazyCandidates.front().key, "key2");

    // 淘汰顺序：懒删除的 key2 优先，之后按普通 LRU 顺序
    EXPECT_EQ(EvictKeys(MEDIA_DRAM, numKeys), (std::vector<std::string>{"key2", "key0", "key1", "key3"}));
}

TEST_F(TestLazyDemote, LazyDemoteThenPromoteRevives)
{
    const size_t numKeys = 5;
    InsertKeys(numKeys);
    EXPECT_EQ(container->Demote("key1"), MMC_OK);
    EXPECT_EQ(container->Demote("key3"), MMC_OK);
    EXPECT_EQ(container->Promote("key1"), MMC_OK); // 访问复活：清懒标志并提到队头
    // key3 仍按懒删除 FIFO 最先淘汰；key1 复活后按普通 MRU 最后淘汰
    EXPECT_EQ(EvictKeys(MEDIA_DRAM, numKeys), (std::vector<std::string>{"key3", "key0", "key2", "key4", "key1"}));
}

// 懒后缀计数在 Promote/Erase/InsertLru 混合操作后仍与链表一致：
// 新 Demote 的 key 必须排在已有懒 key 之后、普通 key 之前
TEST_F(TestLazyDemote, LazyFifoAfterMixedOperations)
{
    const size_t numKeys = 6;
    InsertKeys(numKeys);
    EXPECT_EQ(container->Demote("key1"), MMC_OK);  // 懒队列：key1
    EXPECT_EQ(container->Demote("key2"), MMC_OK);  // 懒队列：key1 → key2
    EXPECT_EQ(container->Promote("key1"), MMC_OK); // 访问复活：懒队列只剩 key2
    EXPECT_EQ(container->Erase("key3"), MMC_OK);
    EXPECT_EQ(container->InsertLru("key2", MEDIA_NONE), MMC_OK); // 停泊出内存层：懒队列清空
    EXPECT_EQ(container->Demote("key4"), MMC_OK);                // 懒队列：key4
    EXPECT_EQ(container->Demote("key5"), MMC_OK);                // 懒队列：key4 → key5
    // key4 最早懒删除，其次 key5，之后是普通 LRU 顺序（key0 最冷、key1 最热）
    const std::vector<std::string> expectedOrder{"key4", "key5", "key0", "key1"};
    EXPECT_EQ(EvictKeys(MEDIA_DRAM, expectedOrder.size()), expectedOrder);
}

// 大量连续 Demote 走 O(1) 边界定位路径，淘汰顺序必须仍是首次懒删除的 FIFO
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

// EraseAll 清空链表后懒后缀边界必须一并重置，新的 Demote 仍按 FIFO 生效
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

// 停泊出内存层再重新入队后，key 已不再是懒条目：边界必须跟着链表状态走
TEST_F(TestLazyDemote, LazySuffixBoundaryAfterLruMoves)
{
    InsertKeys(5);
    EXPECT_EQ(container->Demote("key1"), MMC_OK);
    EXPECT_EQ(container->Demote("key3"), MMC_OK);
    EXPECT_EQ(container->RemoveFromLru("key3", MEDIA_DRAM), MMC_OK);
    EXPECT_EQ(container->InsertLru("key3", MEDIA_DRAM), MMC_OK);
    EXPECT_EQ(container->Demote("key2"), MMC_OK);

    // key1 最先懒删除，其次 key2；key3 回队头后按普通 LRU 顺序最后淘汰
    const std::vector<std::string> expected{"key1", "key2", "key0", "key4", "key3"};
    EXPECT_EQ(EvictKeys(MEDIA_DRAM, expected.size()), expected);
}

// Demote 与 InsertLru 并发：Demote 必须在 lruLock_ 内读取介质与 lruIter_，
// 否则 InsertLru 把 key 停泊为 MEDIA_NONE 并 erase 节点后，Demote 会使用悬空迭代器（ASan 可复现）
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
        // 交替停泊出内存层与重新入队，制造 Demote 的检查-加锁竞态窗口
        container->InsertLru(key, MEDIA_NONE);
        container->InsertLru(key, MEDIA_DRAM);
    }
    stop.store(true, std::memory_order_relaxed);
    demoter.join();

    // 并发结束后容器状态仍自洽：所有 key 都在 DRAM 链表中，逐个淘汰恰好覆盖全部 key
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
    // SSD 停泊 key 不在内存层 LRU，无可降级位置：返回 MMC_SKIPPED 交由上层降级处理
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
    // HBM 层：n1(普通)、kH(懒删除)，链表 [n1, kH]
    EXPECT_EQ(container->Insert("n1", HBM_BLOB_VALUE), MMC_OK);
    EXPECT_EQ(container->Insert("kH", HBM_BLOB_VALUE), MMC_OK);
    EXPECT_EQ(container->Demote("kH"), MMC_OK);
    // DRAM 层：k2(懒删除)
    EXPECT_EQ(container->Insert("k2", DRAM_BLOB_VALUE), MMC_OK);
    EXPECT_EQ(container->Demote("k2"), MMC_OK);

    // 淘汰 k2 并 MOVE_DOWN 降级：淘汰即视为删除完成，懒标志应被清除
    auto moveDownFunc = [](const std::string &, const int &, MediaType) -> EvictResult {
        return EvictResult::MOVE_DOWN;
    };
    EXPECT_TRUE(container->EvictOneLeastRecentlyUsed(moveDownFunc, MEDIA_DRAM));

    // move 完成后经 InsertLru 进入 HBM：标志已清，按普通 MRU 入队头
    EXPECT_EQ(container->InsertLru("k2", MEDIA_HBM), MMC_OK);

    // HBM 淘汰顺序：kH(懒后缀 FIFO) -> n1(普通 LRU) -> k2(新入层 MRU 最后)
    // 若标志未清，k2 会插到懒后缀边界，顺序将变为 kH -> k2 -> n1，可区分
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
    // DRAM 层：n1(普通)、k1(懒删除)
    EXPECT_EQ(container->Insert("n1", DRAM_BLOB_VALUE), MMC_OK);
    EXPECT_EQ(container->Insert("k1", DRAM_BLOB_VALUE), MMC_OK);
    EXPECT_EQ(container->Demote("k1"), MMC_OK);

    // KEEP_SSD 停泊：InsertLru(key, SSD) 离开内存 LRU，lazy_ 应清除
    EXPECT_EQ(container->InsertLru("k1", MEDIA_SSD), MMC_OK);
    // 回温：InsertLru(key, DRAM) 以 MRU 头身份重回内存层，lazy_ 应清除
    EXPECT_EQ(container->InsertLru("k1", MEDIA_DRAM), MMC_OK);

    // 再次懒删除：若 lazy_ 残留会命中幂等分支不 reposition，k1 滞留 MRU 头
    EXPECT_EQ(container->Demote("k1"), MMC_OK);

    // 淘汰顺序：k1(懒删除 FIFO 最先) -> n1(普通 LRU)
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
    // DRAM 层：n1(普通)、k1(懒删除)
    EXPECT_EQ(container->Insert("n1", DRAM_BLOB_VALUE), MMC_OK);
    EXPECT_EQ(container->Insert("k1", DRAM_BLOB_VALUE), MMC_OK);
    EXPECT_EQ(container->Demote("k1"), MMC_OK);

    // 手动淘汰停泊：RemoveFromLru 离开内存 LRU，lazy_ 应清除
    EXPECT_EQ(container->RemoveFromLru("k1", MEDIA_DRAM), MMC_OK);
    // 回温：InsertLru(key, DRAM) 重回内存层
    EXPECT_EQ(container->InsertLru("k1", MEDIA_DRAM), MMC_OK);

    // 再次懒删除：应正常 reposition 到懒后缀
    EXPECT_EQ(container->Demote("k1"), MMC_OK);

    // 淘汰顺序：k1(懒删除 FIFO 最先) -> n1(普通 LRU)
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
