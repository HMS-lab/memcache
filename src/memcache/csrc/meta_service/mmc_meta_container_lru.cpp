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

// 懒删除条目的访问序号：置为最小值，使按访问序号选择淘汰候选的路径优先淘汰懒删除 key，
// 同时不破坏「链表尾到头访问序号递增」的候选收集假设。
constexpr uint64_t kLazyDemotedAccessSequence = 0;

template<typename Key, typename Value>
class MmcMetaContainerLRU : public MmcMetaContainer<Key, Value> {
public:
    explicit MmcMetaContainerLRU(std::function<MediaType(const Value &)> getTypeFunc) : getTypeFunc_(getTypeFunc)
    {
        for (size_t i = 0; i < static_cast<size_t>(MEDIA_NONE); ++i) {
            lazySuffixBegin_[i] = lruLists_[i].end();
        }
    }

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
        bool lazy_ = false; // 懒删除标志：true 表示在懒删除后缀中，按 FIFO 等待优先淘汰
    };
    std::unordered_map<Key, ValueLruItem> metaMap_;
    std::list<Key> lruLists_[MEDIA_NONE];
    // 每个内存介质懒删除后缀的条目数，与 lruLists_ 中 lazy_==true 的节点一一对应
    size_t lazySuffixCounts_[MEDIA_NONE] = {};
    // 每个内存介质懒删除后缀的首个节点（最近一次 Demote 的 key）；无懒条目时等于对应链表 end()。
    // 懒后缀在链表内连续，维护该边界后「插入到后缀之前」是 O(1)，避免 std::prev 逐节点步进
    typename std::list<Key>::iterator lazySuffixBegin_[MEDIA_NONE];
    // 保护链表和 LRU 状态；同时加锁时必须先获取 metaLock_，再获取 lruLock_。
    MmcReadWriteLock lruLock_;
    MmcReadWriteLock metaLock_;
    std::function<MediaType(const Value &)> getTypeFunc_;
    // 所有 ShardedLRU 模板实例共用一个全局、单调递增的访问序号；
    // 从 1 开始分配，0 保留给 kLazyDemotedAccessSequence，避免懒删除哨兵值与正常访问序号冲突
    inline static std::atomic<uint64_t> nextAccessSequence_{1};

    bool IsLazySuffixEntry(const Key &key) const
    {
        auto iter = metaMap_.find(key);
        return iter != metaMap_.end() && iter->second.lazy_;
    }

    void IncreaseLazySuffixCount(MediaType mediaType)
    {
        ++lazySuffixCounts_[mediaType];
    }

    // wasLazy 需在清除 lazy_ 之前读取，避免漏减导致后缀边界漂移
    void DecreaseLazySuffixCount(MediaType mediaType, bool wasLazy)
    {
        if (!wasLazy) {
            return;
        }
        if (lazySuffixCounts_[mediaType] == 0) {
            MMC_LOG_ERROR("lazy suffix count underflow, media=" << static_cast<int>(mediaType));
            return;
        }
        --lazySuffixCounts_[mediaType];
    }

    // 边界节点即将离开懒后缀（erase/promote）时把边界下移到下一个节点；
    // 懒后缀在链表内连续，next 要么仍是懒条目，要么是 end()。
    // 调用方需已持 lruLock_ 写锁，且必须在真正 erase/splice 之前调用
    void AdvanceLazySuffixBegin(std::list<Key> &lruList, MediaType mediaType, typename std::list<Key>::iterator iter)
    {
        if (iter != lazySuffixBegin_[mediaType]) {
            return;
        }
        auto next = std::next(iter);
        if (next != lruList.end() && !IsLazySuffixEntry(*next)) {
            // 边界与链表实际状态已不一致，交给后续反扫重建兜底
            next = lruList.end();
        }
        lazySuffixBegin_[mediaType] = next;
    }

    // 兜底：反扫懒后缀并把计数/边界校正为实际状态。仅在内部状态与链表不一致时才打错误日志，
    // 因此也可用于「链表出现无 map 条目的孤儿节点」这类无法判断懒状态、只能整体重算的场景
    typename std::list<Key>::iterator ScanLazySuffixBoundary(std::list<Key> &lruList, MediaType mediaType)
    {
        const size_t previousCount = lazySuffixCounts_[mediaType];
        size_t lazyCount = 0;
        auto insertPos = lruList.end();
        for (auto rit = lruList.rbegin(); rit != lruList.rend() && IsLazySuffixEntry(*rit); ++rit) {
            insertPos = std::next(rit).base();
            ++lazyCount;
        }
        if (previousCount != lazyCount) {
            MMC_LOG_ERROR("lazy suffix count mismatch, media=" << static_cast<int>(mediaType) << ", expected="
                                                               << previousCount << ", recount=" << lazyCount);
        }
        lazySuffixCounts_[mediaType] = lazyCount;
        lazySuffixBegin_[mediaType] = insertPos;
        return insertPos;
    }

    // 删除 lruLists_ 中的节点并同步懒后缀计数与边界；mediaType 为 MEDIA_NONE 表示该条目不在任何链表中
    void EraseLruEntry(MediaType mediaType, bool wasLazy, typename std::list<Key>::iterator iter)
    {
        if (mediaType == MEDIA_NONE) {
            return;
        }
        if (wasLazy) {
            AdvanceLazySuffixBegin(lruLists_[mediaType], mediaType, iter);
        }
        lruLists_[mediaType].erase(iter);
        DecreaseLazySuffixCount(mediaType, wasLazy);
    }

    // 在 metaLock_ + lruLock_ 已持有的前提下，对 mapIter 指向的条目执行 moveFunc 并按结果做容器操作：
    bool ApplyEvictResult(const Key &key, typename std::unordered_map<Key, ValueLruItem>::iterator &mapIter,
                          const std::function<EvictResult(const Key &, const Value &, MediaType)> &moveFunc,
                          MediaType mediaType)
    {
        Value &value = mapIter->second.value_;
        MediaType storedType = mapIter->second.mediaType_;
        auto lruIter = mapIter->second.lruIter_;
        const bool wasLazy = mapIter->second.lazy_;
        EvictResult res = moveFunc(key, value, mediaType);
        if (res == EvictResult::REMOVE) {
            EraseLruEntry(storedType, wasLazy, lruIter);
            metaMap_.erase(mapIter);
            return true;
        }
        if (res == EvictResult::MOVE_DOWN) {
            if (storedType != MEDIA_NONE) {
                EraseLruEntry(storedType, wasLazy, lruIter);
                mapIter->second.mediaType_ = MEDIA_NONE;
                mapIter->second.lazy_ = false; // 淘汰即视为删除完成：到下一层按普通 LRU 数据对待
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
        EraseLruEntry(lruItem.mediaType_, false, lruItem.lruIter_);
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
            EraseLruEntry(valueItem.mediaType_, valueItem.lazy_, valueItem.lruIter_);
        }
        return MMC_OK;
    }

    Result EraseIfMatch(const Key &key, const Value &expected) override
    {
        MmcWriteGuard lockGuard(metaLock_);
        auto iter = metaMap_.find(key);
        if (iter == metaMap_.end()) {
            MMC_LOG_DEBUG("EraseIfMatch: Key " << key << " not found.");
            return MMC_UNMATCHED_KEY;
        }
        if (!(iter->second.value_ == expected)) {
            MMC_LOG_WARN("EraseIfMatch: key=" << key << " was replaced concurrently, skip stale object");
            return MMC_UNMATCHED_KEY;
        }
        auto valueItem = iter->second;
        if (metaMap_.erase(key) == 0) {
            MMC_LOG_ERROR("EraseIfMatch: failed to erase " << key << " from meta map. ErrCode: " << MMC_ERROR);
            return MMC_ERROR;
        }
        if (valueItem.mediaType_ != MEDIA_NONE) {
            MmcWriteGuard lruLockGuard(lruLock_);
            EraseLruEntry(valueItem.mediaType_, valueItem.lazy_, valueItem.lruIter_);
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
        const bool wasLazy = iter->second.lazy_;
        metaMap_.erase(iter);
        if (mediaType != MEDIA_NONE) {
            MmcWriteGuard lruLockGuard(lruLock_);
            EraseLruEntry(mediaType, wasLazy, lruIter);
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
            lazySuffixCounts_[type] = 0;
            lazySuffixBegin_[type] = lruLists_[type].end();
        }
        return MMC_OK;
    }

    void EraseIf(std::function<bool(const Key &, const Value &)> matchFunc) override
    {
        MmcWriteGuard lockGuard(metaLock_);
        for (auto iter = metaMap_.begin(); iter != metaMap_.end();) {
            if (matchFunc(iter->first, iter->second.value_)) {
                MmcWriteGuard lruLockGuard(lruLock_);
                EraseLruEntry(iter->second.mediaType_, iter->second.lazy_, iter->second.lruIter_);
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

    Result Demote(const Key &key) override
    {
        return DemoteInternal(key, nullptr);
    }

    Result DemoteIfMatch(const Key &key, const Value &expected) override
    {
        return DemoteInternal(key, &expected);
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

    Result InsertLru(const Key &key, MediaType type) override
    {
        return InsertLruInternal(key, nullptr, type);
    }

    Result InsertLruIfMatch(const Key &key, const Value &expected, MediaType type) override
    {
        return InsertLruInternal(key, &expected, type);
    }

private:
    // expected 非空时做身份校验：Get 之后 key 可能已被删除并同名重建，
    // 此时旧对象的懒删除/停泊状态不能作用到新对象上
    Result DemoteInternal(const Key &key, const Value *expected)
    {
        MmcReadGuard lockGuard(metaLock_);
        auto iter = metaMap_.find(key);
        if (iter == metaMap_.end()) {
            MMC_LOG_DEBUG("Demote: Key " << key << " not found in MmcMetaContainer.");
            return MMC_UNMATCHED_KEY;
        }
        if (expected != nullptr && !(iter->second.value_ == *expected)) {
            MMC_LOG_WARN("Demote: key=" << key << " was replaced concurrently, skip stale object");
            return MMC_UNMATCHED_KEY;
        }
        MmcWriteGuard lruLockGuard(lruLock_);
        // 介质状态与 lruIter_ 必须在本锁内读取：InsertLru/RemoveFromLru 只持 metaLock_ 读锁，
        // 可能在拿到 lruLock_ 之前把 key 停泊为 MEDIA_NONE 并 erase 掉原链表节点，
        // 在锁外判断介质后再使用 lruIter_ 会访问已释放节点
        if (iter->second.mediaType_ == MEDIA_NONE) {
            MMC_LOG_DEBUG("Skipped demote for key=" << key << ", mediaType is NONE (key in transition or SSD)");
            return MMC_SKIPPED;
        }
        if (iter->second.lazy_) {
            return MMC_OK; // 重复懒删除：幂等，保持首次位置以维持 FIFO
        }
        auto &lruList = lruLists_[iter->second.mediaType_];
        lruList.erase(iter->second.lruIter_);
        iter->second.lruIter_ = InsertBeforeLazySuffix(lruList, key, iter->second.mediaType_);
        iter->second.lazy_ = true;
        iter->second.accessSequence_ = kLazyDemotedAccessSequence;
        return MMC_OK;
    }

    Result InsertLruInternal(const Key &key, const Value *expected, MediaType type)
    {
        MmcReadGuard lockGuard(metaLock_);
        auto iter = metaMap_.find(key);
        if (iter == metaMap_.end()) {
            MMC_LOG_ERROR("insert lru: Key " << key << " not found. ErrCode: " << MMC_UNMATCHED_KEY);
            return MMC_UNMATCHED_KEY;
        }
        if (expected != nullptr && !(iter->second.value_ == *expected)) {
            MMC_LOG_WARN("insert lru: key=" << key << " was replaced concurrently, skip stale object");
            return MMC_UNMATCHED_KEY;
        }
        MmcWriteGuard lruLockGuard(lruLock_);
        if (iter->second.mediaType_ != MEDIA_NONE) {
            EraseLruEntry(iter->second.mediaType_, iter->second.lazy_, iter->second.lruIter_);
            iter->second.mediaType_ = MEDIA_NONE;
        }
        if (type != MEDIA_NONE && type != MEDIA_SSD) {
            lruLists_[type].push_front(key);
            iter->second.mediaType_ = type;
            iter->second.lruIter_ = lruLists_[type].begin();
            iter->second.accessSequence_ = NextAccessSequence();
        }
        // 重新入队或停泊出内存层后均不在懒后缀区，必须清除懒标志：
        // 否则回温后再次 Demote 命中幂等分支不 reposition，懒删除「优先淘汰」失效
        iter->second.lazy_ = false;
        MMC_LOG_DEBUG("Inserted LRU entry, key=" << key << ", type=" << type);
        return MMC_OK;
    }

public:
    Result RemoveFromLru(const Key &key, MediaType type) override
    {
        MmcReadGuard lockGuard(metaLock_);
        auto iter = metaMap_.find(key);
        if (iter == metaMap_.end()) {
            MMC_LOG_WARN("RemoveFromLru: Key " << key << " not found.");
            return MMC_UNMATCHED_KEY;
        }
        MmcWriteGuard lruLockGuard(lruLock_);
        if (iter->second.mediaType_ != MEDIA_NONE && iter->second.mediaType_ == type) {
            EraseLruEntry(type, iter->second.lazy_, iter->second.lruIter_);
            iter->second.mediaType_ = MEDIA_NONE;
            iter->second.lazy_ = false; // 停泊出内存层即离开懒后缀区，清除懒标志
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
            // 链表残留无 map 条目的孤儿节点：无法判断其懒状态，重算后缀长度保持计数一致
            ScanLazySuffixBoundary(lruLists_[mediaType], mediaType);
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
        bool wasLazy = false;
        {
            MmcWriteGuard metaGuard(metaLock_);
            MmcWriteGuard lruGuard(lruLock_);
            auto mapIter = metaMap_.find(candidate.key);
            if (mapIter == metaMap_.end() || mapIter->second.mediaType_ != mediaType ||
                mapIter->second.accessSequence_ != candidate.accessSequence) {
                return EvictResult::FAIL;
            }
            value = mapIter->second.value_;
            wasLazy = mapIter->second.lazy_;
            EraseLruEntry(mediaType, wasLazy, mapIter->second.lruIter_);
            mapIter->second.mediaType_ = MEDIA_NONE;
            mapIter->second.lazy_ = false; // 淘汰即视为删除完成：到下一层按普通 LRU 数据对待
        }

        EvictResult result = moveFunc(candidate.key, value, mediaType);
        if (result == EvictResult::FAIL) {
            // 有活跃租约等原因跳过淘汰：把 key 放回原介质。懒候选回队尾（最早 Demote 的先淘汰）；
            // 普通候选必须回到懒删除后缀之前，否则回调期间并发 Demote 的 key 会排在其后，
            // 破坏「尾部连续为懒条目」和访问序号从尾到头递增的约定，导致后续淘汰先选普通 key，
            // 且 cutoff 候选收集在队尾提前 break、漏掉懒删除 key
            MmcWriteGuard metaGuard(metaLock_);
            MmcWriteGuard lruGuard(lruLock_);
            auto mapIter = metaMap_.find(candidate.key);
            if (mapIter != metaMap_.end() && mapIter->second.mediaType_ == MEDIA_NONE &&
                mapIter->second.accessSequence_ == candidate.accessSequence) {
                auto &lruList = lruLists_[mediaType];
                if (wasLazy) {
                    lruList.push_back(candidate.key);
                    mapIter->second.lruIter_ = std::prev(lruList.end());
                    // 还原的是最早 Demote 的 key，正常应落在队尾；仅当后缀原本为空时它才成为新边界
                    if (lazySuffixBegin_[mediaType] == lruList.end()) {
                        lazySuffixBegin_[mediaType] = mapIter->second.lruIter_;
                    }
                    IncreaseLazySuffixCount(mediaType);
                } else {
                    mapIter->second.lruIter_ = lruList.insert(lazySuffixBegin_[mediaType], candidate.key);
                }
                mapIter->second.mediaType_ = mediaType;
                mapIter->second.lazy_ = wasLazy;
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
        // 懒删除 key 被访问后离开懒后缀：先同步边界与计数，再取消标记
        if (lruItem.lazy_) {
            AdvanceLazySuffixBegin(lruList, lruItem.mediaType_, lruItem.lruIter_);
        }
        DecreaseLazySuffixCount(lruItem.mediaType_, lruItem.lazy_);
        lruList.splice(lruList.begin(), lruList, lruItem.lruIter_);
        lruItem.accessSequence_ = NextAccessSequence();
        lruItem.lazy_ = false; // Get/Exist/Query/AddLease 等任何 Promote 均取消懒删除
    }

    static uint64_t NextAccessSequence()
    {
        return nextAccessSequence_.fetch_add(1, std::memory_order_relaxed);
    }

    // 插入到懒删除后缀的头部一侧（所有已置 lazy 标志的 key 之前），维持"最早 Demote 最靠队尾"的 FIFO 不变式。
    // 空表或全懒时插入位置为表尾/表头，等价 push_back / push_front。
    // 调用方需已持 metaLock_ 读锁与 lruLock_ 写锁；依靠 lazySuffixBegin_ 做到 O(1) 定位，
    // 边界与链表不一致时退化为反扫并校正（最差只退化到逐节点定位的性能，不会错位）
    typename std::list<Key>::iterator InsertBeforeLazySuffix(std::list<Key> &lruList, const Key &key,
                                                             MediaType mediaType)
    {
        auto insertPos = lazySuffixBegin_[mediaType];
        bool boundaryOk = false;
        if (insertPos == lruList.end()) {
            // 无懒条目：链表为空或队尾不是懒条目才算一致
            boundaryOk = lazySuffixCounts_[mediaType] == 0 && (lruList.empty() || !IsLazySuffixEntry(lruList.back()));
        } else {
            // 边界必须是懒条目，且它前一个节点不能是懒条目
            boundaryOk = IsLazySuffixEntry(*insertPos) &&
                         (insertPos == lruList.begin() || !IsLazySuffixEntry(*std::prev(insertPos)));
        }
        if (!boundaryOk) {
            insertPos = ScanLazySuffixBoundary(lruList, mediaType);
        }
        ++lazySuffixCounts_[mediaType];
        auto newIter = lruList.insert(insertPos, key);
        lazySuffixBegin_[mediaType] = newIter;
        return newIter;
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
