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
#ifndef MF_HYBRID_MMC_META_CONTAINER_H
#define MF_HYBRID_MMC_META_CONTAINER_H

#include <memory>

#include "mmc_blob_allocator.h"
#include "mmc_def.h"
#include "mmc_ref.h"
#include "mmc_thread_pool.h"

namespace ock {
namespace mmc {

enum class MetaLruType : uint32_t {
    LRU = MMC_META_LRU_TYPE_LRU,
    SHARDED_LRU = MMC_META_LRU_TYPE_SHARDED_LRU,
};

template<typename Key, typename Value>
class MmcMetaContainer : public MmcReferable {
public:
    virtual ~MmcMetaContainer() = default;
    virtual Result Insert(const Key &key, const Value &value) = 0;
    virtual Result Get(const Key &key, Value &value) = 0;
    virtual Result Erase(const Key &key) = 0;
    virtual Result Erase(const Key &key, Value &value) = 0;
    virtual Result EraseIfEmpty(const Key &key, std::function<bool(const Value &)> isEmptyFunc) = 0;
    virtual Result EraseAll(std::function<void(const Key &, const Value &)> removeFunc) = 0;
    virtual void EraseIf(std::function<bool(const Key &, const Value &)> matchFunc) = 0;
    // EraseIf 并行执行所用线程池，由 meta manager 在启动时注入；未注入时退化为串行。
    virtual void SetEraseIfPool(MmcThreadPoolPtr pool)
    {
        (void)pool;
    }
    virtual void IterateIf(std::function<bool(const Key &, const Value &)> matchFunc,
                           std::map<Key, Value> &matchedValues) = 0;
    virtual void GetAllKeys(std::vector<Key> &keys) = 0;
    virtual size_t GetKeyCount() = 0;
    virtual size_t GetKeyCount(MediaType mediaType) = 0;
    virtual std::vector<Key> GetEvictCandidateKeys(MediaType mediaType, size_t count) = 0;
    virtual Result Promote(const Key &key) = 0;
    virtual MediaType GetMediaType(const Key &key) = 0;
    virtual Result InsertLru(const Key &key, MediaType type) = 0;
    virtual Result RemoveFromLru(const Key &key, MediaType type) = 0;

    virtual void MultiLevelElimination(const std::vector<std::pair<uint16_t, uint16_t>> &evictWatermarks,
                                       const std::vector<MediaType> &needEvictList,
                                       const std::vector<uint16_t> &nowMemoryThresholds,
                                       std::function<EvictResult(const Key &, const Value &, MediaType)> moveFunc,
                                       uint32_t yieldInterval = 0, uint32_t yieldDurationUs = 0) = 0;
    // yieldInterval: evict this many objects then sleep yieldDurationUs microseconds to release
    //                the write lock, allowing blocked requests to acquire the lock. 0 disables yield.

    /**
     * @brief 碎片整理目标释放：对 DefragEvictForMedia 选中的 key 列表逐个执行 move-down
     *        释放，与 EvictOneLeastRecentlyUsed 同构——持容器写锁调 moveFunc，锁内做
     *        Erase（REMOVE）或 LRU 摘除（MOVE_DOWN），消除 ABA 窗口。每 yieldInterval 个
     *        key 后 yield 一次，让被阻塞的 Get/Put/Exist 有窗口获取 metaLock_
     * @param keys           [in] DefragEvictForMedia 返回的目标 key 列表
     * @param moveFunc       [in] 释放回调（EvictCallBackFunction，执行 move-down 或纯释放）
     * @param mediaType      [in] 源介质类型
     * @param yieldInterval  [in] 每 N 个 key yield 一次（0=禁用）
     * @param yieldDurationUs [in] 每次 yield 睡眠微秒数
     * @return 实际释放（REMOVE + MOVE_DOWN）的 key 个数
     */
    virtual size_t EvictDefragTargets(const std::vector<Key> &keys,
                                      std::function<EvictResult(const Key &, const Value &, MediaType)> moveFunc,
                                      MediaType mediaType, uint32_t yieldInterval = 0,
                                      uint32_t yieldDurationUs = 0) = 0;
};

template<typename Key, typename Value>
class MmcMetaContainerFactory {
public:
    static MmcRef<MmcMetaContainer<Key, Value>>
    Create(MetaLruType lruType, std::function<MediaType(const Value &)> getTypeFunc, uint32_t lruShardCount);
};
} // namespace mmc
} // namespace ock

#endif // MF_HYBRID_MMC_META_CONTAINER_H
