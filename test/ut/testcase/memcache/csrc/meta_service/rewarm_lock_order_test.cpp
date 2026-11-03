/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * MemCache_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */
#include <cstdlib>
#include <future>
#include <limits>
#include <tuple>
#include <unistd.h>

#include "gtest/gtest.h"
#include "mmc_meta_container_sharded_lru.cpp"

// Load dependencies before changing access to the manager under test.
#include "mmc_defrag_evict.h"
#include "mmc_global_allocator.h"
#include "mmc_storage_mode.h"
#include "mmc_meta_backup_mgr.h"
#include "mmc_meta_net_server.h"

#define private public
#include "mmc_meta_manager.h"
#undef private

namespace ock {
namespace mmc {
namespace {
constexpr uint32_t kTestShardCount = 2U;
constexpr uint32_t kTestRank = 0U;
constexpr uint32_t kBlobCount = 1U;
constexpr uint64_t kTestBaseAddress = 0x1000U;
constexpr uint64_t kTestLeaseTtlMs = 100U;
constexpr uint16_t kHighWatermark = 70U;
constexpr uint16_t kLowWatermark = 60U;
constexpr unsigned int kDeadlockTimeoutSeconds = 5U;
const std::string kTestKey = "rewarm_lock_order";

enum class FailurePath { ROLLBACK, WRITE_FINISH, READ_START };

struct EraseHooks {
    std::function<void()> beforeErase;
    std::function<void()> beforeMatch;
    std::function<void()> afterEmptyCheck;
};

template<typename Base>
class RewarmTestContainer : public Base {
public:
    template<typename... Args>
    explicit RewarmTestContainer(const std::shared_ptr<EraseHooks> &hooks, Args &&...args)
        : Base(std::forward<Args>(args)...), hooks_(hooks)
    {}

    Result Erase(const std::string &key) override
    {
        BeforeErase();
        return Base::Erase(key);
    }

    Result EraseIfEmpty(const std::string &key, std::function<bool(const MmcMemObjMetaPtr &)> isEmpty) override
    {
        BeforeErase();
        return Base::EraseIfEmpty(key, [&](const MmcMemObjMetaPtr &object) {
            const bool empty = isEmpty(object);
            auto hook = hooks_->afterEmptyCheck;
            if (hook) {
                hook();
            }
            return empty;
        });
    }

    void IterateIf(std::function<bool(const std::string &, const MmcMemObjMetaPtr &)> match,
                   std::map<std::string, MmcMemObjMetaPtr> &matched) override
    {
        Base::IterateIf(
            [&](const std::string &key, const MmcMemObjMetaPtr &object) {
                if (hooks_->beforeMatch) {
                    hooks_->beforeMatch();
                }
                return match(key, object);
            },
            matched);
    }

private:
    void BeforeErase()
    {
        auto hook = hooks_->beforeErase;
        if (hook) {
            hook();
        }
    }

    std::shared_ptr<EraseHooks> hooks_;
};
} // namespace

class TestMmcRewarmLockOrder : public testing::TestWithParam<std::tuple<MetaLruType, FailurePath>> {
protected:
    void SetUp() override
    {
        manager_ = MmcMakeRef<MmcMetaManager>(kTestLeaseTtlMs, kHighWatermark, kLowWatermark);
        manager_->SetSsdMetadataMode(MMC_SSD_METADATA_SEPARATED);
        hooks_ = std::make_shared<EraseHooks>();
        auto getType = [](const MmcMemObjMetaPtr &) { return MEDIA_DRAM; };
        if (std::get<0>(GetParam()) == MetaLruType::SHARDED_LRU) {
            using Container = RewarmTestContainer<MmcMetaContainerShardedLRU<std::string, MmcMemObjMetaPtr>>;
            manager_->metaContainer_ = MmcMakeRef<Container>(hooks_, getType, kTestShardCount).Get();
        } else {
            using Container = RewarmTestContainer<MmcMetaContainerLRU<std::string, MmcMemObjMetaPtr>>;
            manager_->metaContainer_ = MmcMakeRef<Container>(hooks_, getType).Get();
        }
        manager_->globalAllocator_ = MmcMakeRef<MmcGlobalAllocator>();
        const MmcLocation location{kTestRank, MEDIA_DRAM};
        const MmcLocalMemlInitInfo info{kTestBaseAddress, SIZE_32K};
        ASSERT_EQ(manager_->globalAllocator_->Mount(location, info), MMC_OK);
        ASSERT_EQ(manager_->globalAllocator_->Start(location), MMC_OK);
        std::vector<MmcMemBlobPtr> blobs;
        const AllocOptions options{SIZE_32K, kBlobCount, MEDIA_DRAM, {kTestRank}, ALLOC_FORCE_BY_RANK};
        ASSERT_EQ(manager_->globalAllocator_->Alloc(options, blobs), MMC_OK);
        ASSERT_EQ(blobs.size(), kBlobCount);
        destination_ = blobs.front();
        object_ = MmcMakeRef<MmcMemObjMeta>();
        ASSERT_EQ(object_->AddBlob(destination_), MMC_OK);
        ASSERT_EQ(manager_->metaContainer_->Insert(kTestKey, object_), MMC_OK);
        if (std::get<1>(GetParam()) == FailurePath::WRITE_FINISH) {
            ASSERT_EQ(destination_->UpdateState(MMC_REMOVE_START), MMC_OK);
        } else if (std::get<1>(GetParam()) == FailurePath::READ_START) {
            ASSERT_EQ(destination_->UpdateState(MMC_WRITE_OK), MMC_OK);
            destination_->SetDefaultLeaseTtlMs(std::numeric_limits<uint64_t>::max());
        }
    }

    bool RunFailure()
    {
        std::unique_lock<std::mutex> guard(object_->Mutex());
        Result ret = MMC_ERROR;
        if (std::get<1>(GetParam()) == FailurePath::ROLLBACK) {
            manager_->RollbackEntry(kTestKey, object_, nullptr, destination_, kTestRank, 0);
        } else {
            const MmcMetaManager::RewarmCtx context{kTestRank, 0, MEDIA_SSD, MEDIA_DRAM};
            MmcMemMetaDesc result;
            ret = manager_->ApplyRewarm(kTestKey, object_, nullptr, destination_->GetDesc(), destination_, context,
                                        result, true);
        }
        // The callers of both functions retain ownership of the object mutex.
        auto lockRetained = std::async(std::launch::async, [this]() {
            std::unique_lock<std::mutex> probe(object_->Mutex(), std::try_to_lock);
            return !probe.owns_lock();
        });
        return ret != MMC_OK && lockRetained.get();
    }

    bool ScanAlongsideFailure()
    {
        std::promise<void> shardLocked;
        auto shardReady = shardLocked.get_future();
        std::future<bool> scan;
        hooks_->beforeMatch = [&]() { shardLocked.set_value(); };
        hooks_->beforeErase = [&]() {
            hooks_->beforeErase = nullptr;
            scan = std::async(std::launch::async, [this]() {
                std::vector<DefragBlobEntry> index;
                std::unordered_map<uint32_t, std::string> keys;
                return manager_->BuildDefragBlobIndex(MEDIA_DRAM, kTestRank, {kTestKey}, index, keys);
            });
            // The real defrag callback holds the shard read lock before acquiring the object lock.
            shardReady.wait();
        };
        const bool completed = RunFailure();
        scan.get();
        MmcMemObjMetaPtr found;
        return completed && manager_->metaContainer_->Get(kTestKey, found) == MMC_UNMATCHED_KEY;
    }

    MmcMemBlobPtr SsdBlob()
    {
        return MmcMakeRef<MmcMemBlob>(kTestRank, 0, SIZE_32K, MEDIA_SSD, READABLE, SIZE_32K);
    }

    auto &Container()
    {
        return manager_->metaContainer_;
    }

    MmcMetaManagerPtr manager_;
    MmcMemObjMetaPtr object_;
    MmcMemBlobPtr destination_;
    std::shared_ptr<EraseHooks> hooks_;
};

TEST_P(TestMmcRewarmLockOrder, DefragAndFailureBothComplete)
{
    // Bound a regression to a child process so a lock cycle cannot hang the test suite.
    ASSERT_EXIT(
        {
            alarm(kDeadlockTimeoutSeconds);
            const bool completed = ScanAlongsideFailure();
            _exit(completed ? EXIT_SUCCESS : EXIT_FAILURE);
        },
        testing::ExitedWithCode(EXIT_SUCCESS), "");
}

TEST_P(TestMmcRewarmLockOrder, ReleasesObjectLockBeforeContainerErase)
{
    bool checked = false;
    hooks_->beforeErase = [&]() {
        auto acquired = std::async(std::launch::async, [this]() {
            std::unique_lock<std::mutex> probe(object_->Mutex(), std::try_to_lock);
            return probe.owns_lock();
        });
        EXPECT_TRUE(acquired.get());
        checked = true;
    };
    ASSERT_TRUE(RunFailure());
    EXPECT_TRUE(checked);
    MmcMemObjMetaPtr found;
    EXPECT_EQ(Container()->Get(kTestKey, found), MMC_UNMATCHED_KEY);
}

TEST_P(TestMmcRewarmLockOrder, HoldsObjectLockThroughConditionalErase)
{
    bool checked = false;
    hooks_->afterEmptyCheck = [&]() {
        auto acquired = std::async(std::launch::async, [this]() {
            std::unique_lock<std::mutex> probe(object_->Mutex(), std::try_to_lock);
            return probe.owns_lock();
        });
        EXPECT_FALSE(acquired.get());
        checked = true;
    };
    ASSERT_TRUE(RunFailure());
    EXPECT_TRUE(checked);
    MmcMemObjMetaPtr found;
    EXPECT_EQ(Container()->Get(kTestKey, found), MMC_UNMATCHED_KEY);
}

TEST_P(TestMmcRewarmLockOrder, KeepsReplacementObjectEvenWhenEmpty)
{
    auto replacement = MmcMakeRef<MmcMemObjMeta>();
    hooks_->beforeErase = [&]() {
        hooks_->beforeErase = nullptr;
        EXPECT_EQ(Container()->Erase(kTestKey), MMC_OK);
        EXPECT_EQ(Container()->Insert(kTestKey, replacement), MMC_OK);
    };
    ASSERT_TRUE(RunFailure());
    MmcMemObjMetaPtr found;
    ASSERT_EQ(Container()->Get(kTestKey, found), MMC_OK);
    EXPECT_EQ(found.Get(), replacement.Get());
}

TEST_P(TestMmcRewarmLockOrder, RechecksObjectRepopulatedBeforeErase)
{
    hooks_->beforeErase = [&]() {
        auto repopulated = std::async(std::launch::async, [this]() {
            std::unique_lock<std::mutex> guard(object_->Mutex(), std::try_to_lock);
            return guard.owns_lock() && object_->AddBlob(SsdBlob()) == MMC_OK;
        });
        EXPECT_TRUE(repopulated.get());
    };
    ASSERT_TRUE(RunFailure());
    MmcMemObjMetaPtr found;
    ASSERT_EQ(Container()->Get(kTestKey, found), MMC_OK);
    EXPECT_EQ(found.Get(), object_.Get());
    EXPECT_EQ(found->NumBlobs(), kBlobCount);
}

TEST_P(TestMmcRewarmLockOrder, KeepsRemainingSsdBlob)
{
    ASSERT_EQ(object_->AddBlob(SsdBlob()), MMC_OK);
    ASSERT_TRUE(RunFailure());
    MmcMemObjMetaPtr found;
    ASSERT_EQ(Container()->Get(kTestKey, found), MMC_OK);
    EXPECT_EQ(found.Get(), object_.Get());
    EXPECT_EQ(found->NumBlobs(), kBlobCount);
    EXPECT_EQ(found->GetBlobType(), MEDIA_SSD);
}

INSTANTIATE_TEST_SUITE_P(Containers, TestMmcRewarmLockOrder,
                         testing::Combine(testing::Values(MetaLruType::LRU, MetaLruType::SHARDED_LRU),
                                          testing::Values(FailurePath::ROLLBACK, FailurePath::WRITE_FINISH,
                                                          FailurePath::READ_START)));

} // namespace mmc
} // namespace ock
