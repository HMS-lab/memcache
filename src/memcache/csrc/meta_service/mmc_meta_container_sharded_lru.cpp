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

#ifndef MF_HYBRID_MMC_META_CONTAINER_SHARDED_LRU_H
#define MF_HYBRID_MMC_META_CONTAINER_SHARDED_LRU_H

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <queue>
#include <thread>
#include <vector>

#include "mmc_meta_container_lru.cpp"

namespace ock {
namespace mmc {

constexpr uint64_t kShardHashBits = 64;

template<typename Key, typename Value>
class MmcMetaContainerShardedLRU : public MmcMetaContainer<Key, Value> {
public:
    explicit MmcMetaContainerShardedLRU(std::function<MediaType(const Value &)> getTypeFunc, uint32_t lruShardCount)
        : shardSelectShift_(kShardHashBits - GetShardSelectBits(lruShardCount))
    {
        shards_.reserve(lruShardCount);
        for (uint32_t i = 0; i < lruShardCount; ++i) {
            shards_.emplace_back(std::make_unique<MmcMetaContainerLRU<Key, Value>>(getTypeFunc));
        }
    }

    Result Insert(const Key &key, const Value &value) override
    {
        return ShardOf(key).Insert(key, value);
    }

    Result Get(const Key &key, Value &value) override
    {
        return ShardOf(key).Get(key, value);
    }

    Result Erase(const Key &key) override
    {
        return ShardOf(key).Erase(key);
    }

    Result Erase(const Key &key, Value &value) override
    {
        return ShardOf(key).Erase(key, value);
    }

    Result EraseIfEmpty(const Key &key, std::function<bool(const Value &)> isEmptyFunc) override
    {
        return ShardOf(key).EraseIfEmpty(key, isEmptyFunc);
    }

    Result EraseAll(std::function<void(const Key &, const Value &)> removeFunc) override
    {
        for (size_t i = 0; i < shards_.size(); ++i) {
            shards_[i]->EraseAll(removeFunc);
        }
        return MMC_OK;
    }

    void EraseIf(std::function<bool(const Key &, const Value &)> matchFunc) override
    {
        for (size_t i = 0; i < shards_.size(); ++i) {
            shards_[i]->EraseIf(matchFunc);
        }
    }

    void IterateIf(std::function<bool(const Key &, const Value &)> matchFunc,
                   std::map<Key, Value> &matchedValues) override
    {
        for (size_t i = 0; i < shards_.size(); ++i) {
            shards_[i]->IterateIf(matchFunc, matchedValues);
        }
    }

    void GetAllKeys(std::vector<Key> &keys) override
    {
        for (size_t i = 0; i < shards_.size(); ++i) {
            shards_[i]->GetAllKeys(keys);
        }
    }

    size_t GetKeyCount() override
    {
        size_t keyCount = 0;
        for (size_t i = 0; i < shards_.size(); ++i) {
            keyCount += shards_[i]->GetKeyCount();
        }
        return keyCount;
    }

    size_t GetKeyCount(MediaType mediaType) override
    {
        if (mediaType >= MEDIA_NONE || mediaType == MEDIA_SSD) {
            return 0;
        }

        size_t keyCount = 0;
        for (const auto &shard : shards_) {
            keyCount += shard->GetKeyCount(mediaType);
        }
        return keyCount;
    }

    std::vector<Key> GetEvictCandidateKeys(MediaType mediaType, size_t count) override
    {
        std::vector<Key> keys;
        if (mediaType >= MEDIA_NONE || mediaType == MEDIA_SSD || count == 0 || shards_.empty()) {
            return keys;
        }

        constexpr size_t kCensusThreadCount = 4;
        const size_t shardCount = shards_.size();
        const size_t threadCount = std::min(kCensusThreadCount, shardCount);
        const size_t shardsPerThread = (shardCount + threadCount - 1) / threadCount;
        std::vector<size_t> shardCandidateCounts(shardCount);
        size_t candidateCount = 0;
        for (size_t shard = 0; shard < shardCount; ++shard) {
            shardCandidateCounts[shard] = shards_[shard]->GetEvictionCandidateCount(mediaType);
            candidateCount += shardCandidateCounts[shard];
        }
        if (candidateCount == 0) {
            return keys;
        }

        const size_t targetCount = std::min(count, candidateCount);
        const auto coldSequences =
            CollectGlobalColdSequences(mediaType, targetCount, shardCandidateCounts, threadCount, shardsPerThread);
        if (coldSequences.empty()) {
            return keys;
        }

        auto candidates = CollectCandidates(mediaType, coldSequences.back(), true, threadCount, shardsPerThread);
        std::sort(candidates.begin(), candidates.end(), [](const ShardCandidate &left, const ShardCandidate &right) {
            return left.candidate.accessSequence < right.candidate.accessSequence;
        });

        keys.reserve(std::min(targetCount, candidates.size()));
        for (const auto &candidate : candidates) {
            if (keys.size() >= targetCount) {
                break;
            }
            keys.push_back(candidate.candidate.key);
        }
        return keys;
    }

    Result Promote(const Key &key) override
    {
        return ShardOf(key).Promote(key);
    }

    MediaType GetMediaType(const Key &key) override
    {
        return ShardOf(key).GetMediaType(key);
    }

    Result InsertLru(const Key &key, MediaType type) override
    {
        return ShardOf(key).InsertLru(key, type);
    }

    Result RemoveFromLru(const Key &key, MediaType type) override
    {
        return ShardOf(key).RemoveFromLru(key, type);
    }

    void MultiLevelElimination(const std::vector<std::pair<uint16_t, uint16_t>> &evictWatermarks,
                               const std::vector<MediaType> &needEvictList,
                               const std::vector<uint16_t> &nowMemoryThresholds,
                               std::function<EvictResult(const Key &, const Value &, MediaType)> moveFunc,
                               uint32_t yieldInterval = 0, uint32_t yieldDurationUs = 0) override
    {
        for (size_t i = 0; i < needEvictList.size(); ++i) {
            const MediaType mediaType = needEvictList[i];
            const uint16_t nowThreshold = nowMemoryThresholds[i];
            if (mediaType >= MEDIA_NONE || mediaType == MEDIA_SSD) {
                MMC_LOG_ERROR("Invalid mediaType: " << mediaType);
                continue;
            }

            const uint16_t high = evictWatermarks[mediaType].first;
            const uint16_t low = evictWatermarks[mediaType].second;
            if (high == 0 || nowThreshold <= low) {
                continue;
            }

            constexpr size_t kCensusThreadCount = 4;
            const size_t shardCount = shards_.size();
            const size_t threadCount = std::min(kCensusThreadCount, shardCount);
            const size_t shardsPerThread = (shardCount + threadCount - 1) / threadCount;
            size_t candidateCount = 0;
            std::vector<size_t> shardCandidateCounts(shardCount);

            // 统计所有 shard 中这个介质层的 LRU 对象数
            for (size_t shard = 0; shard < shardCount; ++shard) {
                shardCandidateCounts[shard] = shards_[shard]->GetEvictionCandidateCount(mediaType);
                candidateCount += shardCandidateCounts[shard];
            }
            if (candidateCount == 0) {
                continue;
            }

            const size_t numEvictObjs =
                std::max(std::min(candidateCount * static_cast<size_t>(nowThreshold - low) / high, candidateCount),
                         static_cast<size_t>(1));

            constexpr size_t kMinReserveSlack = 64;
            constexpr size_t kReserveSlackDivisor = 10;
            // 多拿一些候选，应对候选在执行前失效；
            const size_t reserveSlack =
                std::max(kMinReserveSlack, (numEvictObjs + kReserveSlackDivisor - 1) / kReserveSlackDivisor);
            const size_t reserveCount = std::min(candidateCount, numEvictObjs + reserveSlack);

            const auto coldSequences =
                CollectGlobalColdSequences(mediaType, reserveCount, shardCandidateCounts, threadCount, shardsPerThread);

            // 确定全局淘汰边界后，再把边界以内的真实 key 候选收集出来
            std::vector<ShardCandidate> candidates;
            uint64_t targetCutoff = 0;
            uint64_t reserveCutoff = 0;
            bool recoveredFullPopulation = false;
            if (coldSequences.size() >= numEvictObjs) {
                targetCutoff = coldSequences[numEvictObjs - 1];
                reserveCutoff = coldSequences.back();
                candidates = CollectCandidates(mediaType, reserveCutoff, true, threadCount, shardsPerThread);
            }

            // 正常候选不够目标数，说明两个阶段之间发生了较严重的并发变化， 重新收集candidate
            if (candidates.size() < numEvictObjs) {
                candidates = CollectCandidates(mediaType, std::numeric_limits<uint64_t>::max(), true, threadCount,
                                               shardsPerThread);
                recoveredFullPopulation = true;
            }
            if (candidates.empty()) {
                continue;
            }

            const size_t targetCount = std::min(numEvictObjs, candidates.size());
            if (recoveredFullPopulation) {
                // 这是一个很少触发的路径：在 key 物化之前，有界快照已经发生了变化。进入全量重收集之后，candidates 是 当前所有可淘汰对象，要重新计算 targetCutoff
                auto targetIter = candidates.begin() + (targetCount - 1);
                std::nth_element(candidates.begin(), targetIter, candidates.end(),
                                 [](const ShardCandidate &left, const ShardCandidate &right) {
                                     return left.candidate.accessSequence < right.candidate.accessSequence;
                                 });
                targetCutoff = targetIter->candidate.accessSequence;
            }

            // 两个候选组内部都不要求有序。
            // 所有 primary candidate 都属于全局目标集合；
            // 只有当并发变化导致部分 primary candidate 失效时，才会使用 reserve candidate。
            std::vector<ShardCandidate> primaryCandidates;
            std::vector<ShardCandidate> reserveCandidates;
            primaryCandidates.reserve(targetCount);
            reserveCandidates.reserve(candidates.size() - targetCount);
            for (auto &candidate : candidates) {
                if (candidate.candidate.accessSequence <= targetCutoff) {
                    primaryCandidates.push_back(std::move(candidate));
                } else {
                    reserveCandidates.push_back(std::move(candidate));
                }
            }

            // 阶段 3：在候选项所属 shard 的锁保护下重新校验。
            // 从候选收集到真正执行之间，候选项可能已经被提升、删除，或移动到了其他位置。
            // 如果发生这种情况，就使用 reserve candidate 补足目标数量。
            size_t evicted = 0;
            size_t attempted = 0;
            EvictCandidates(primaryCandidates, targetCount, evicted, attempted, moveFunc, mediaType, yieldInterval,
                            yieldDurationUs);
            if (evicted < targetCount) {
                EvictCandidates(reserveCandidates, targetCount, evicted, attempted, moveFunc, mediaType, yieldInterval,
                                yieldDurationUs);
            }
            if (evicted < targetCount && !recoveredFullPopulation && reserveCount < candidateCount) {
                // 因为并发 Get/Promote、删除、换介质等，很多候选在真正淘汰时已失效，EvictCandidate 返回 FAIL。主候选和备用候选都试完后，实际淘汰数还没达到 targetCount
                // 在这条少见的恢复路径中，仍然会扫描剩余候选， 继续尝试淘汰，直到达到 targetCount 或候选耗尽
                // 但扫描过程会并行执行。
                auto refillCandidates =
                    CollectCandidates(mediaType, reserveCutoff, false, threadCount, shardsPerThread);
                EvictCandidates(refillCandidates, targetCount, evicted, attempted, moveFunc, mediaType, yieldInterval,
                                yieldDurationUs);
            }
            MMC_LOG_INFO("Evicted " << evicted << " keys from " << mediaType << ". Candidate count: " << candidateCount
                                    << ".");
        }
    }

    size_t EvictDefragTargets(const std::vector<Key> &keys,
                              std::function<EvictResult(const Key &, const Value &, MediaType)> moveFunc,
                              MediaType mediaType, uint32_t yieldInterval = 0, uint32_t yieldDurationUs = 0) override
    {
        std::vector<std::vector<Key>> shardKeys(shards_.size());
        for (const auto &key : keys) {
            shardKeys[ShardIndex(key)].push_back(key);
        }

        size_t evicted = 0;
        for (size_t shard = 0; shard < shards_.size(); ++shard) {
            if (shardKeys[shard].empty()) {
                continue;
            }
            const size_t shardEvicted = shards_[shard]->EvictDefragTargets(shardKeys[shard], moveFunc, mediaType,
                                                                           yieldInterval, yieldDurationUs);
            evicted += shardEvicted;
            MMC_LOG_DEBUG("EvictDefragTargets shard " << shard << ", media: " << mediaType << ", keys: "
                                                      << shardKeys[shard].size() << ", evicted: " << shardEvicted);
        }

        MMC_LOG_INFO("EvictDefragTargets done, media: " << mediaType << ", shards: " << shards_.size()
                                                        << ", keys: " << keys.size() << ", evicted: " << evicted);
        return evicted;
    }

private:
    using Candidate = typename MmcMetaContainerLRU<Key, Value>::EvictionCandidate;

    struct ShardCandidate {
        size_t shard;
        Candidate candidate;
    };

    template<typename Func>
    void ForEachShardRangeInParallel(size_t threadCount, size_t shardsPerThread, Func func)
    {
        std::vector<std::thread> threads;
        threads.reserve(threadCount);
        for (size_t thread = 0; thread < threadCount; ++thread) {
            threads.emplace_back([&, thread] {
                const size_t firstShard = thread * shardsPerThread;
                const size_t lastShard = std::min(firstShard + shardsPerThread, shards_.size());
                func(thread, firstShard, lastShard);
            });
        }
        for (auto &thread : threads) {
            thread.join();
        }
    }

    std::vector<uint64_t> CollectGlobalColdSequences(MediaType mediaType, size_t reserveCount,
                                                     const std::vector<size_t> &shardCandidateCounts,
                                                     size_t threadCount, size_t shardsPerThread)
    {
        std::vector<std::vector<uint64_t>> shardFrontiers(shards_.size());
        ForEachShardRangeInParallel(threadCount, shardsPerThread, [&](size_t, size_t firstShard, size_t lastShard) {
            for (size_t shard = firstShard; shard < lastShard; ++shard) {
                shards_[shard]->CollectColdestEvictionSequences(
                    mediaType, std::min(reserveCount, shardCandidateCounts[shard]), shardFrontiers[shard]);
            }
        });

        // 把各 shard 的“最冷访问序号”做一次全局归并，选出全局最老的前 reserveCount 个候选。
        struct HeapEntry {
            size_t shard;
            size_t index;
        };
        auto older = [&shardFrontiers](const HeapEntry &left, const HeapEntry &right) {
            return shardFrontiers[left.shard][left.index] > shardFrontiers[right.shard][right.index];
        };
        std::priority_queue<HeapEntry, std::vector<HeapEntry>, decltype(older)> coldHeap(older);
        for (size_t shard = 0; shard < shards_.size(); ++shard) {
            if (!shardFrontiers[shard].empty()) {
                coldHeap.push({shard, 0});
            }
        }

        std::vector<uint64_t> coldSequences;
        coldSequences.reserve(reserveCount);
        while (!coldHeap.empty() && coldSequences.size() < reserveCount) {
            const auto entry = coldHeap.top();
            coldHeap.pop();
            coldSequences.push_back(shardFrontiers[entry.shard][entry.index]);
            if (entry.index + 1 < shardFrontiers[entry.shard].size()) {
                coldHeap.push({entry.shard, entry.index + 1});
            }
        }
        return coldSequences;
    }

    std::vector<ShardCandidate> CollectCandidates(MediaType mediaType, uint64_t cutoff, bool olderOrEqual,
                                                  size_t threadCount, size_t shardsPerThread)
    {
        std::vector<std::vector<ShardCandidate>> localCandidates(threadCount);
        ForEachShardRangeInParallel(
            threadCount, shardsPerThread, [&](size_t thread, size_t firstShard, size_t lastShard) {
                for (size_t shard = firstShard; shard < lastShard; ++shard) {
                    std::vector<Candidate> shardCandidates;
                    shards_[shard]->CollectEvictionCandidates(mediaType, cutoff, olderOrEqual, shardCandidates);
                    for (auto &candidate : shardCandidates) {
                        localCandidates[thread].push_back({shard, std::move(candidate)});
                    }
                }
            });

        std::vector<ShardCandidate> candidates;
        for (auto &local : localCandidates) {
            candidates.insert(candidates.end(), std::make_move_iterator(local.begin()),
                              std::make_move_iterator(local.end()));
        }
        return candidates;
    }

    void EvictCandidates(const std::vector<ShardCandidate> &batch, size_t targetCount, size_t &evicted,
                         size_t &attempted,
                         const std::function<EvictResult(const Key &, const Value &, MediaType)> &moveFunc,
                         MediaType mediaType, uint32_t yieldInterval, uint32_t yieldDurationUs)
    {
        for (const auto &candidate : batch) {
            if (evicted >= targetCount)
                break;
            const auto result = shards_[candidate.shard]->EvictCandidate(candidate.candidate, moveFunc, mediaType);
            if (result != EvictResult::FAIL) {
                ++evicted;
            }
            ++attempted;
            if (yieldInterval > 0 && yieldDurationUs > 0 && attempted % yieldInterval == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(yieldDurationUs));
            }
        }
    }

    static uint64_t GetShardSelectBits(uint32_t shardCount)
    {
        uint64_t shardSelectBits = 0;
        while (shardCount > 1) {
            ++shardSelectBits;
            shardCount >>= 1;
        }
        return shardSelectBits;
    }

    MmcMetaContainerLRU<Key, Value> &ShardOf(const Key &key)
    {
        return *shards_[ShardIndex(key)];
    }

    size_t ShardIndex(const Key &key) const
    {
        if (shards_.size() == 1) {
            return 0;
        }
        const uint64_t hash = static_cast<uint64_t>(std::hash<Key>{}(key));
        return hash >> shardSelectShift_;
    }

    uint64_t shardSelectShift_;
    std::vector<std::unique_ptr<MmcMetaContainerLRU<Key, Value>>> shards_;
};

template class MmcMetaContainerShardedLRU<std::string, MmcMemObjMetaPtr>;

template<typename Key, typename Value>
MmcRef<MmcMetaContainer<Key, Value>>
MmcMetaContainerFactory<Key, Value>::Create(MetaLruType lruType, std::function<MediaType(const Value &)> getTypeFunc,
                                            uint32_t lruShardCount)
{
    switch (lruType) {
        case MetaLruType::LRU:
            return MmcMakeRef<MmcMetaContainerLRU<Key, Value>>(getTypeFunc).Get();
        case MetaLruType::SHARDED_LRU:
            return MmcMakeRef<MmcMetaContainerShardedLRU<Key, Value>>(getTypeFunc, lruShardCount).Get();
        default:
            return nullptr;
    }
}

template class MmcMetaContainerFactory<std::string, MmcMemObjMetaPtr>;

} // namespace mmc
} // namespace ock

#endif // MF_HYBRID_MMC_META_CONTAINER_SHARDED_LRU_H
