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

#ifndef MF_HYBRID_MMC_META_CONTAINER_LRU_H
#define MF_HYBRID_MMC_META_CONTAINER_LRU_H

#include <algorithm>
#include <atomic>
#include <chrono>
#include <list>
#include <memory>
#include <thread>
#include <unordered_map>

#include "mmc_mem_obj_meta.h"
#include "mmc_rwlock.h"
#include "mmc_meta_container.h"

namespace ock {
namespace mmc {

template<typename Key, typename Value>
class MmcMetaContainerLRU : public MmcMetaContainer<Key, Value> {
public:
    explicit MmcMetaContainerLRU(std::function<MediaType(const Value &)> getTypeFunc) : getTypeFunc_(getTypeFunc) {}

    struct EvictionCandidate {
        Key key;
        uint64_t accessSequence;
    };

private:
    struct ValueLruItem {
        Value value_;
        // 访问时需持有 metaLock_ 写锁，或同时持有 metaLock_ 读锁和 lruLock_。
	// Access requires holding the write lock for metaLock_, or holding both the read lock for metaLock_ and the lruLock_.
        MediaType mediaType_;
        typename std::list<Key>::iterator lruIter_;
        // 最后一次被访问 的全局顺序号
	// Global sequence number of the last access
        uint64_t accessSequence_;
    };
    std::unordered_map<Key, ValueLruItem> metaMap_;
    std::list<Key> lruLists_[MEDIA_NONE];
    // 保护链表和 LRU 状态；同时加锁时必须先获取 metaLock_，再获取 lruLock_。
    MmcReadWriteLock lruLock_;
    MmcReadWriteLock metaLock_;
    std::function<MediaType(const Value &)> getTypeFunc_;
    // 所有 ShardedLRU 模板实例共用一个全局、单调递增的访问序号
    inline static std::atomic<uint64_t> nextAccessSequence_{0};

    // 在 metaLock_ + lruLock_ 已持有的前提下，对 mapIter 指向的条目执行 moveFunc 并按结果做容器操作：
    bool ApplyEvictResult(const Key &key, typename std::unordered_map<Key, ValueLruItem>::iterator &mapIter,
                          const std::function<EvictResult(const Key &, const Value &, MediaType)> &moveFunc,
                          MediaType mediaType)
    {
        Value &value = mapIter->second.value_;
        MediaType storedType = mapIter->second.mediaType_;
        auto lruIter = mapIter->second.lruIter_;
        EvictResult res = moveFunc(key, value, mediaType);
        if (res == EvictResult::REMOVE) {
            if (storedType != MEDIA_NONE) {
                lruLists_[storedType].erase(lruIter);
            }
            metaMap_.erase(mapIter);
            return true;
        }
        if (res == EvictResult::MOVE_DOWN) {
            if (storedType != MEDIA_NONE) {
                lruLists_[storedType].erase(lruIter);
                mapIter->second.mediaType_ = MEDIA_NONE;
            }
            return true;
        }
        return false;
    }

public:
    Result Insert(const Key &key, const Value &value) override
    {
        MmcWriteGuard lockGuard(metaLock_);
        auto iter = metaMap_.find(key);
        if (iter != metaMap_.end()) {
            return MMC_DUPLICATED_OBJECT;
        }

        // insert into LRU and create lruItem
        MmcWriteGuard lrucLockGuard(lruLock_);
        MediaType blobType = GetBlobType(value);
        if (blobType == MEDIA_NONE) {
            MMC_LOG_ERROR("Failed to Insert, no medium registered. ");
            return MMC_ERROR;
        }
        if (blobType != MEDIA_SSD) {
            lruLists_[blobType].push_front(key);
        }
        ValueLruItem lruItem{value, blobType != MEDIA_SSD ? blobType : MEDIA_NONE,
                             blobType != MEDIA_SSD ? lruLists_[blobType].begin() : lruLists_[MEDIA_NONE].end(),
                             NextAccessSequence()};

        // insert into map
        auto ret = metaMap_.emplace(key, lruItem);
        if (ret.second) {
            return MMC_OK;
        }
        lruLists_[blobType].erase(lruItem.lruIter_);
        MMC_LOG_ERROR("Fail to insert " << key << " into MmcMetaContainer. ErrCode: " << MMC_ERROR);
        return MMC_ERROR;
    }

    Result Get(const Key &key, Value &value) override
    {
        MmcReadGuard lockGuard(metaLock_);
        auto iter = metaMap_.find(key);
        if (iter != metaMap_.end()) {
            value = iter->second.value_;
            return MMC_OK;
        }
        MMC_LOG_DEBUG("Get: Key " << key << " not found in MmcMetaContainer. ErrCode: " << MMC_UNMATCHED_KEY);
        return MMC_UNMATCHED_KEY;
    }

    Result Erase(const Key &key) override
    {
        Value value;
        return Erase(key, value);
    }

    Result Erase(const Key &key, Value &value) override
    {
        MmcWriteGuard lockGuard(metaLock_);
        auto iter = metaMap_.find(key);
        if (iter == metaMap_.end()) {
            MMC_LOG_DEBUG("Key " << key << " not found in MmcMetaContainer. ErrCode: " << MMC_UNMATCHED_KEY);
            return MMC_UNMATCHED_KEY;
        }
        auto valueItem = iter->second;
        value = valueItem.value_;
        if (metaMap_.erase(key) == 0) {
            MMC_LOG_ERROR("Failed to erase " << key << " from MmcMetaContainer map. ErrCode: " << MMC_ERROR);
            return MMC_ERROR;
        }
        if (valueItem.mediaType_ != MEDIA_NONE) {
            MmcWriteGuard lruLockGuard(lruLock_);
            lruLists_[valueItem.mediaType_].erase(valueItem.lruIter_);
        }
        return MMC_OK;
    }

    Result EraseIfEmpty(const Key &key, std::function<bool(const Value &)> isEmptyFunc) override
    {
        MmcWriteGuard lockGuard(metaLock_);
        auto iter = metaMap_.find(key);
        if (iter == metaMap_.end()) {
            MMC_LOG_DEBUG("Key " << key << " not found in MmcMetaContainer. ErrCode: " << MMC_UNMATCHED_KEY);
            return MMC_UNMATCHED_KEY;
        }
        // 判空与删除在容器写锁内原子完成，消除锁外判空的 ABA 窗口
	// Checks for emptiness and deletions are performed atomically while holding the container's write lock, eliminating the ABA window that would otherwise exist if such checks were performed outside the lock.
        if (!isEmptyFunc(iter->second.value_)) {
            return MMC_ERROR;
        }
        auto mediaType = iter->second.mediaType_;
        auto lruIter = iter->second.lruIter_;
        metaMap_.erase(iter);
        if (mediaType != MEDIA_NONE) {
            MmcWriteGuard lruLockGuard(lruLock_);
            lruLists_[mediaType].erase(lruIter);
        }
        return MMC_OK;
    }

    Result EraseAll(std::function<void(const Key &, const Value &)> removeFunc) override
    {
        MmcWriteGuard metaGuard(metaLock_);
        MmcWriteGuard lruGuard(lruLock_);

        for (auto &pair : metaMap_) {
            removeFunc(pair.first, pair.second.value_);
        }

        metaMap_.clear();
        for (MediaType type = MEDIA_HBM; type != MEDIA_NONE; type = MoveDown(type)) {
            lruLists_[type].clear();
        }
        return MMC_OK;
    }

    void EraseIf(std::function<bool(const Key &, const Value &)> matchFunc) override
    {
        MmcWriteGuard lockGuard(metaLock_);
        for (auto iter = metaMap_.begin(); iter != metaMap_.end();) {
            if (matchFunc(iter->first, iter->second.value_)) {
                MmcWriteGuard lruLockGuard(lruLock_);
                if (iter->second.mediaType_ != MEDIA_NONE) {
                    lruLists_[iter->second.mediaType_].erase(iter->second.lruIter_);
                }
                iter = metaMap_.erase(iter);
            } else {
                ++iter;
            }
        }
    }

    void IterateIf(std::function<bool(const Key &, const Value &)> matchFunc,
                   std::map<Key, Value> &matchedValues) override
    {
        MmcReadGuard lockGuard(metaLock_);
        for (auto iter = metaMap_.begin(); iter != metaMap_.end();) {
            if (matchFunc(iter->first, iter->second.value_)) {
                matchedValues.emplace(std::make_pair(iter->first, iter->second.value_));
            }
            ++iter;
        }
    }

    void GetAllKeys(std::vector<Key> &keys) override
    {
        MmcReadGuard lockGuard(metaLock_);
        keys.reserve(metaMap_.size());
        for (const auto &pair : metaMap_) {
            keys.push_back(pair.first);
        }
        MMC_LOG_DEBUG("Retrieved all keys, count=" << keys.size());
    }

    size_t GetKeyCount() override
    {
        MmcReadGuard lockGuard(metaLock_);
        return metaMap_.size();
    }

    size_t GetKeyCount(MediaType mediaType) override
    {
        if (mediaType >= MEDIA_NONE || mediaType == MEDIA_SSD) {
            return 0;
        }
        MmcReadGuard lockGuard(lruLock_);
        return lruLists_[mediaType].size();
    }

    std::vector<Key> GetEvictCandidateKeys(MediaType mediaType, size_t count) override
    {
        std::vector<Key> keys;
        if (mediaType >= MEDIA_NONE || mediaType == MEDIA_SSD || count == 0) {
            return keys;
        }

        MmcReadGuard lockGuard(lruLock_);
        keys.reserve(std::min(count, lruLists_[mediaType].size()));
        for (auto iter = lruLists_[mediaType].rbegin(); iter != lruLists_[mediaType].rend() && keys.size() < count;
             ++iter) {
            keys.push_back(*iter);
        }
        return keys;
    }

    Result Promote(const Key &key)
    {
        MmcReadGuard lockGuard(metaLock_);
        auto iter = metaMap_.find(key);
        if (iter == metaMap_.end()) {
            MMC_LOG_ERROR("Promote: Key " << key << " not found in MmcMetaContainer. ErrCode: " << MMC_UNMATCHED_KEY);
            return MMC_UNMATCHED_KEY;
        }
        UpdateLRU(iter->first, iter->second);
        return MMC_OK;
    }

    MediaType GetMediaType(const Key &key) override
    {
        MmcReadGuard lockGuard(metaLock_);
        auto iter = metaMap_.find(key);
        if (iter == metaMap_.end()) {
            return MEDIA_NONE;
        }
        MmcReadGuard lruLockGuard(lruLock_);
        return iter->second.mediaType_;
    }

    Result InsertLru(const Key &key, MediaType type)
    {
        MmcReadGuard lockGuard(metaLock_);
        auto iter = metaMap_.find(key);
        if (iter != metaMap_.end()) {
            MmcWriteGuard lockGuard(lruLock_);
            if (iter->second.mediaType_ != MEDIA_NONE) {
                lruLists_[iter->second.mediaType_].erase(iter->second.lruIter_);
                iter->second.mediaType_ = MEDIA_NONE;
            }
            if (type != MEDIA_NONE && type != MEDIA_SSD) {
                lruLists_[type].push_front(key);
                iter->second.mediaType_ = type;
                iter->second.lruIter_ = lruLists_[type].begin();
                iter->second.accessSequence_ = NextAccessSequence();
            }
            MMC_LOG_DEBUG("Inserted LRU entry, key=" << key << ", type=" << type);
            return MMC_OK;
        }
        MMC_LOG_ERROR("insert lru: Key " << key << " not found. ErrCode: " << MMC_UNMATCHED_KEY);
        return MMC_UNMATCHED_KEY;
    }

    Result RemoveFromLru(const Key &key, MediaType type)
    {
        MmcReadGuard lockGuard(metaLock_);
        auto iter = metaMap_.find(key);
        if (iter == metaMap_.end()) {
            MMC_LOG_WARN("RemoveFromLru: Key " << key << " not found.");
            return MMC_UNMATCHED_KEY;
        }
        MmcWriteGuard lruLockGuard(lruLock_);
        if (iter->second.mediaType_ != MEDIA_NONE && iter->second.mediaType_ == type) {
            lruLists_[type].erase(iter->second.lruIter_);
            iter->second.mediaType_ = MEDIA_NONE;
            MMC_LOG_DEBUG("Removed LRU entry, key=" << key << ", type=" << type);
        }
        return MMC_OK;
    }

    bool EvictOneLeastRecentlyUsed(std::function<EvictResult(const Key &, const Value &, MediaType)> moveFunc,
                                   const MediaType mediaType)
    {
        if (mediaType == MEDIA_NONE) {
            MMC_LOG_ERROR("Invalid mediaType: " << mediaType);
            return false;
        }
        MmcWriteGuard lockGuard(metaLock_);
        MmcWriteGuard lruLockGuard(lruLock_);
        if (lruLists_[mediaType].empty()) {
            MMC_LOG_ERROR("Unable to evict, LRU list is empty");
            return false;
        }
        auto iter = std::prev(lruLists_[mediaType].end());
        Key key = *iter;

        // 1、查找value
	// 1. Find value
        auto mapIter = metaMap_.find(key);
        if (mapIter == metaMap_.end()) {
            lruLists_[mediaType].erase(iter);
            MMC_LOG_INFO("Key " << key << " not found in MmcMetaContainer.");
            return true;
        }

        // 2、回调处理并按结果做容器操作（REMOVE/MOVE_DOWN/SKIP）
	// 2. Handle the callback and perform container operations (REMOVE/MOVE_DOWN/SKIP) based on the result.
        return ApplyEvictResult(key, mapIter, moveFunc, mediaType);
    }

    void CollectEvictionCandidates(const MediaType mediaType, const uint64_t cutoff, const bool olderOrEqual,
                                   std::vector<EvictionCandidate> &candidates)
    {
        if (mediaType == MEDIA_NONE || mediaType == MEDIA_SSD) {
            return;
        }
        MmcReadGuard metaGuard(metaLock_);
        MmcReadGuard lruGuard(lruLock_);
        for (auto keyIter = lruLists_[mediaType].rbegin(); keyIter != lruLists_[mediaType].rend(); ++keyIter) {
            const auto &key = *keyIter;
            auto iter = metaMap_.find(key);
            if (iter == metaMap_.end() || iter->second.mediaType_ != mediaType) {
                continue;
            }
            const bool selected =
                olderOrEqual ? iter->second.accessSequence_ <= cutoff : iter->second.accessSequence_ > cutoff;
            if (selected) {
                candidates.push_back({key, iter->second.accessSequence_});
            } else if (olderOrEqual) {
                // 访问序列号从链表尾部到头部是递增的，因此后面的条目
                // 不可能再满足这个 cutoff 条件。
		// The sequence numbers increase from the tail of the linked list toward the head;
		// therefore, subsequent entries
		// cannot possibly satisfy this cutoff condition.
                break;
            }
        }
    }

    size_t GetEvictionCandidateCount(const MediaType mediaType)
    {
        if (mediaType == MEDIA_NONE || mediaType == MEDIA_SSD) {
            return 0;
        }
        MmcReadGuard lruGuard(lruLock_);
        return lruLists_[mediaType].size();
    }

    void CollectColdestEvictionSequences(const MediaType mediaType, const size_t limit,
                                         std::vector<uint64_t> &sequences)
    {
        if (mediaType == MEDIA_NONE || mediaType == MEDIA_SSD || limit == 0) {
            return;
        }
        MmcReadGuard metaGuard(metaLock_);
        MmcReadGuard lruGuard(lruLock_);
        sequences.reserve(sequences.size() + std::min(limit, lruLists_[mediaType].size()));
        for (auto keyIter = lruLists_[mediaType].rbegin();
             keyIter != lruLists_[mediaType].rend() && sequences.size() < limit; ++keyIter) {
            const auto &key = *keyIter;
            auto iter = metaMap_.find(key);
            if (iter != metaMap_.end() && iter->second.mediaType_ == mediaType) {
                sequences.push_back(iter->second.accessSequence_);
            }
        }
    }

    EvictResult EvictCandidate(const EvictionCandidate &candidate,
                               std::function<EvictResult(const Key &, const Value &, MediaType)> moveFunc,
                               const MediaType mediaType)
    {
        Value value;
        {
            MmcWriteGuard metaGuard(metaLock_);
            MmcWriteGuard lruGuard(lruLock_);
            auto mapIter = metaMap_.find(candidate.key);
            if (mapIter == metaMap_.end() || mapIter->second.mediaType_ != mediaType ||
                mapIter->second.accessSequence_ != candidate.accessSequence) {
                return EvictResult::FAIL;
            }
            value = mapIter->second.value_;
            lruLists_[mediaType].erase(mapIter->second.lruIter_);
            mapIter->second.mediaType_ = MEDIA_NONE;
        }

        EvictResult result = moveFunc(candidate.key, value, mediaType);
        if (result == EvictResult::FAIL) {
            MmcWriteGuard metaGuard(metaLock_);
            MmcWriteGuard lruGuard(lruLock_);
            auto mapIter = metaMap_.find(candidate.key);
            if (mapIter != metaMap_.end() && mapIter->second.mediaType_ == MEDIA_NONE &&
                mapIter->second.accessSequence_ == candidate.accessSequence) {
                lruLists_[mediaType].push_back(candidate.key);
                mapIter->second.lruIter_ = std::prev(lruLists_[mediaType].end());
                mapIter->second.mediaType_ = mediaType;
            }
        } else if (result == EvictResult::REMOVE) {
            MmcWriteGuard metaGuard(metaLock_);
            MmcWriteGuard lruGuard(lruLock_);
            auto mapIter = metaMap_.find(candidate.key);
            if (mapIter != metaMap_.end() && mapIter->second.mediaType_ == MEDIA_NONE &&
                mapIter->second.accessSequence_ == candidate.accessSequence) {
                metaMap_.erase(mapIter);
            }
        }
        return result;
    }

    void MultiLevelElimination(const std::vector<std::pair<uint16_t, uint16_t>> &evictWatermarks,
                               const std::vector<MediaType> &needEvictList,
                               const std::vector<uint16_t> &nowMemoryThresholds,
                               std::function<EvictResult(const Key &, const Value &, MediaType)> moveFunc,
                               uint32_t yieldInterval = 0, uint32_t yieldDurationUs = 0) override
    {
        for (size_t i = 0; i < needEvictList.size(); ++i) {
            auto mediaType = needEvictList[i];
            auto nowThreshold = nowMemoryThresholds[i];
            if (mediaType >= MEDIA_NONE || mediaType == MEDIA_SSD) {
                MMC_LOG_ERROR("Invalid mediaType: " << mediaType);
                continue;
            }

            uint16_t high = evictWatermarks[mediaType].first;
            uint16_t low = evictWatermarks[mediaType].second;
            if (high == 0) {
                MMC_LOG_WARN("Skip eviction for mediaType=" << mediaType << ", evictThresholdHigh is 0");
                continue;
            }

            lruLock_.LockRead();
            size_t oriNum = lruLists_[mediaType].size();
            lruLock_.UnLock();

            const size_t numEvictObjs =
                std::max(std::min(oriNum * (nowThreshold - low) / high, oriNum), static_cast<size_t>(1));

            for (size_t j = 0; j < numEvictObjs; ++j) {
                EvictOneLeastRecentlyUsed(moveFunc, mediaType);
                // Yield: periodically sleep to extend the lock-free window between evictions,
                // giving blocked Put/Get/Exist threads enough time (~5us kernel scheduling)
                // to wake up and acquire metaLock_ during the ~100us sleep window.
                if (yieldInterval > 0 && yieldDurationUs > 0 && (j + 1) % yieldInterval == 0) {
                    std::this_thread::sleep_for(std::chrono::microseconds(yieldDurationUs));
                }
            }

            MMC_LOG_INFO("Evicted " << numEvictObjs << " keys from " << mediaType << ". Number of keys: " << oriNum
                                    << " => " << (oriNum - numEvictObjs) << ".");
        }
    }

    /**
     * @brief 与 EvictOneLeastRecentlyUsed 同构，但按指定 key 列表逐个释放（非 LRU 尾部）。
     */
    size_t EvictDefragTargets(const std::vector<Key> &keys,
                              std::function<EvictResult(const Key &, const Value &, MediaType)> moveFunc,
                              MediaType mediaType, uint32_t yieldInterval = 0, uint32_t yieldDurationUs = 0) override
    {
        size_t evicted = 0;
        for (size_t i = 0; i < keys.size(); ++i) {
            const auto &key = keys[i];
            {
                MmcWriteGuard lockGuard(metaLock_);
                auto mapIter = metaMap_.find(key);
                if (mapIter == metaMap_.end()) {
                    continue; // 已被并发路径删除
                }

                MmcWriteGuard lruLockGuard(lruLock_);
                if (ApplyEvictResult(key, mapIter, moveFunc, mediaType)) {
                    evicted++;
                }
            }
            // Yield: periodically sleep to extend the lock-free window between evictions,
            // giving blocked Put/Get/Exist threads enough time (~5us kernel scheduling)
            // to wake up and acquire metaLock_ during the ~100us sleep window.
            if (yieldInterval > 0 && yieldDurationUs > 0 && (i + 1) % yieldInterval == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(yieldDurationUs));
            }
        }
        return evicted;
    }

private:
    void UpdateLRU(const Key &key, ValueLruItem &lruItem)
    {
        MmcWriteGuard lockGuard(lruLock_);
        if (lruItem.mediaType_ == MEDIA_NONE) {
            MMC_LOG_DEBUG("Skipped promotion for key=" << key << ", mediaType is NONE (key in transition)");
            return;
        }
        // 只支持在同层级内部更新
        // Only supports updates within the same level.
        auto &lruList = lruLists_[lruItem.mediaType_];
        lruList.splice(lruList.begin(), lruList, lruItem.lruIter_);
        lruItem.accessSequence_ = NextAccessSequence();
    }

    static uint64_t NextAccessSequence()
    {
        return nextAccessSequence_.fetch_add(1, std::memory_order_relaxed);
    }

    MediaType GetBlobType(const Value &value)
    {
        if (getTypeFunc_) {
            return getTypeFunc_(value);
        }
        return MEDIA_NONE;
    }
};

} // namespace mmc
} // namespace ock

#endif // MF_HYBRID_MMC_META_CONTAINER_LRU_H
