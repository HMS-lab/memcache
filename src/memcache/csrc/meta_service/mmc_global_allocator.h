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
#ifndef MEM_FABRIC_MMC_GLOBAL_ALLOCATOR_H
#define MEM_FABRIC_MMC_GLOBAL_ALLOCATOR_H

#include "nlohmann/json.hpp"

#include "mmc_blob_allocator.h"
#include "mmc_locality_strategy.h"
#include "mmc_read_write_lock.h"

namespace ock {
namespace mmc {

constexpr int LEVEL_BASE = 100;

using MmcMemPoolInitInfo = std::map<MmcLocation, MmcLocalMemlInitInfo>;

inline std::ostream &operator<<(std::ostream &os, const std::vector<MmcMemBlobPtr> &vec)
{
    os << ", Rank[";
    for (size_t i = 0; i < vec.size(); ++i) {
        if (i > 0) {
            os << ", ";
        }
        os << vec[i]->Rank();
    }
    os << "]";
    return os;
}

class MmcGlobalAllocator : public MmcReferable {
public:
    MmcGlobalAllocator() = default;

    /**
     * @brief
     * blob分配策略实现
     * @param allocOpt
     * 1. preferredRank为空时（实际上至少有一个local rank），numBlobs可以是 <= MAX_BLOB_COPIES 的任意值
     * 2. preferredRank非空时，rank不能重复，且rank个数 <= numBlobs
     * 3. preferredRank大于1的时候，必定是强制分配场景，例如：
     *    replicaNum=4，preferredLocalServiceIDs=[0,1]场景，需要强制在0,1上分配两个副本，剩下2个副本按默认分配策略处理；且需要排除已分配过的rank列表
     * @param blobs
     * @return Result
     */
    Result Alloc(const AllocOptions &allocOpt, std::vector<MmcMemBlobPtr> &blobs)
    {
        std::unordered_set<uint32_t> uniqueRanks(allocOpt.preferredRank_.begin(), allocOpt.preferredRank_.end());
        if (allocOpt.numBlobs_ < allocOpt.preferredRank_.size() ||
            uniqueRanks.size() != allocOpt.preferredRank_.size()) {
            MMC_LOG_ERROR("Invalid alloc option: " << allocOpt);
            return MMC_ERROR;
        }

        AllocOptions allocReq = allocOpt;
        if (allocReq.mediaType_ == MEDIA_NONE) {
            allocReq.mediaType_ = static_cast<uint16_t>(GetTopLayerMediumType());
        }
        std::unordered_set<uint32_t> excludeRanks;
        globalAllocLock_.LockRead();
        if (allocReq.preferredRank_.size() <= 1u) {
            auto ret = InnerAlloc(allocReq, blobs, excludeRanks);
            globalAllocLock_.UnlockRead();
            if (ret != MMC_OK) {
                Free(blobs);
                MMC_LOG_ERROR("Simple alloc failed, ret: " << ret << ", allocReq: " << allocReq);
            }
            return ret;
        }
        Result result{};
        for (const uint32_t rank : allocReq.preferredRank_) {
            AllocOptions tmpAllocReq = allocReq;
            tmpAllocReq.preferredRank_ = {rank};
            tmpAllocReq.numBlobs_ = 1u;
            tmpAllocReq.flags_ = static_cast<uint32_t>(ALLOC_FORCE_BY_RANK);
            if ((result = InnerAlloc(tmpAllocReq, blobs, excludeRanks)) != MMC_OK) {
                MMC_LOG_ERROR("Force alloc failed, result: " << result << ", allocReq: " << tmpAllocReq);
                globalAllocLock_.UnlockRead();
                Free(blobs);
                return result;
            }
        }
        excludeRanks.insert(allocReq.preferredRank_.begin(), allocReq.preferredRank_.end());
        if (allocReq.numBlobs_ - allocReq.preferredRank_.size() > 0) {
            AllocOptions tmpAllocReq = allocReq;
            tmpAllocReq.numBlobs_ = allocReq.numBlobs_ - allocReq.preferredRank_.size();
            tmpAllocReq.flags_ = static_cast<uint32_t>(ALLOC_RANDOM);
            if ((result = InnerAlloc(tmpAllocReq, blobs, excludeRanks)) != MMC_OK) {
                MMC_LOG_ERROR("Arrange alloc failed, result: " << result << ", allocReq: " << tmpAllocReq);
                globalAllocLock_.UnlockRead();
                Free(blobs);
                return result;
            }
        }
        globalAllocLock_.UnlockRead();
        if (blobs.size() != allocReq.numBlobs_) {
            MMC_LOG_ERROR("More alloc failed, " << allocReq << blobs);
            Free(blobs);
            return MMC_ERROR;
        }
        MMC_LOG_DEBUG("More alloc, " << allocReq << blobs);
        return MMC_OK;
    }

    void Free(std::vector<MmcMemBlobPtr> &blobs) noexcept
    {
        for (const auto &blob : blobs) {
            Free(blob);
        }
        blobs.clear();
    }

    Result Free(const MmcMemBlobPtr &blob)
    {
        if (blob == nullptr) {
            MMC_LOG_ERROR("Free blob failed, blob is nullptr");
            return MMC_INVALID_PARAM;
        }
        if (blob->Type() == MEDIA_SSD) {
            MMC_LOG_DEBUG("Free SSD blob (UBS IO managed), skipping allocator release, blob: " << blob->GetDesc());
            return MMC_OK;
        }
        globalAllocLock_.LockRead();
        const MmcLocation location{blob->Rank(), static_cast<MediaType>(blob->Type())};
        const auto iter = allocators_.find(location);
        if (iter == allocators_.end()) {
            globalAllocLock_.UnlockRead();
            MMC_LOG_ERROR("Free blob failed, location not found, rank: " << location.rank_
                                                                         << ", mediaType: " << location.mediaType_);
            return MMC_INVALID_PARAM;
        }

        const auto &allocator = iter->second;
        if (allocator == nullptr) {
            globalAllocLock_.UnlockRead();
            MMC_LOG_ERROR("Free blob failed, allocator is nullptr");
            return MMC_ERROR;
        }
        Result ret = allocator->Release(blob);
        globalAllocLock_.UnlockRead();
        if (ret != MMC_OK) {
            MMC_LOG_ERROR("Free blob failed, blob: " << blob->GetDesc());
        }
        return ret;
    };

    Result Mount(const MmcLocation &loc, const MmcLocalMemlInitInfo &localMemInitInfo, bool enableMaxValueMode = false)
    {
        globalAllocLock_.LockWrite();
        auto iter = allocators_.find(loc);
        if (iter != allocators_.end()) {
            globalAllocLock_.UnlockWrite();
            MMC_LOG_INFO("not need mount at the existing position, loc: " << loc);
            return MMC_OK;
        }

        allocators_[loc] = MmcBlobAllocator::Create(loc, localMemInitInfo, enableMaxValueMode, &maxAlignedSize_);

        MMC_LOG_TRACE("Mount bm on " << loc << ", capacity:" << localMemInitInfo.capacity_ << "  successfully");
        globalAllocLock_.UnlockWrite();
        return MMC_OK;
    }

    Result Start(const MmcLocation &loc)
    {
        globalAllocLock_.LockRead();
        const auto iter = allocators_.find(loc);
        if (iter == allocators_.end()) {
            globalAllocLock_.UnlockRead();
            MMC_LOG_ERROR("location not found, rank: " << loc.rank_ << ", mediaType: " << loc.mediaType_);
            return MMC_INVALID_PARAM;
        }

        const auto &allocator = iter->second;
        if (allocator == nullptr) {
            globalAllocLock_.UnlockRead();
            MMC_LOG_ERROR("Start failed, allocator is nullptr");
            return MMC_ERROR;
        }
        allocator->Start();
        globalAllocLock_.UnlockRead();
        return MMC_OK;
    }

    Result Stop(const MmcLocation &loc)
    {
        globalAllocLock_.LockRead();
        const auto iter = allocators_.find(loc);
        if (iter == allocators_.end()) {
            globalAllocLock_.UnlockRead();
            MMC_LOG_ERROR("location not found, rank: " << loc.rank_ << ", mediaType: " << loc.mediaType_);
            return MMC_INVALID_PARAM;
        }

        const auto &allocator = iter->second;
        if (allocator == nullptr) {
            globalAllocLock_.UnlockRead();
            MMC_LOG_ERROR("Stop failed, allocator is nullptr");
            return MMC_ERROR;
        }
        allocator->Stop();
        MMC_LOG_TRACE("Stop one bm successfully, loc: " << loc);
        globalAllocLock_.UnlockRead();
        return MMC_OK;
    }

    Result Unmount(const MmcLocation &loc)
    {
        globalAllocLock_.LockWrite();
        auto iter = allocators_.find(loc);
        if (iter == allocators_.end()) {
            globalAllocLock_.UnlockWrite();
            MMC_LOG_ERROR("Cannot find the given {rank:" << loc.rank_ << ", type:" << loc.mediaType_
                                                         << "} in the mem pool");
            return MMC_INVALID_PARAM;
        }
        if (iter->second == nullptr) {
            globalAllocLock_.UnlockWrite();
            MMC_LOG_ERROR("Unmount failed, allocator is nullptr");
            return MMC_ERROR;
        }
        if (!iter->second->CanUnmount()) {
            globalAllocLock_.UnlockWrite();
            MMC_LOG_ERROR("Cannot unmount the given {rank:" << loc.rank_ << ", type:" << loc.mediaType_
                                                            << "}  in the mem pool, space is in use");
            return MMC_INVALID_PARAM;
        }
        allocators_.erase(iter);
        MMC_LOG_DEBUG("Unmount one bm successfully, loc: " << loc);
        globalAllocLock_.UnlockWrite();
        return MMC_OK;
    }

    Result BuildFromBlobs(const MmcLocation &location, std::vector<std::pair<std::string, MmcMemBlobDesc>> &blobList)
    {
        globalAllocLock_.LockRead();
        const auto iter = allocators_.find(location);
        if (iter == allocators_.end()) {
            globalAllocLock_.UnlockRead();
            MMC_LOG_ERROR("Build from blobs failed, location not found, rank: " << location.rank_ << ", mediaType: "
                                                                                << location.mediaType_);
            return MMC_INVALID_PARAM;
        }

        const auto &allocator = iter->second;
        if (allocator == nullptr) {
            globalAllocLock_.UnlockRead();
            MMC_LOG_ERROR("BuildFromBlobs failed, allocator is nullptr");
            return MMC_ERROR;
        }
        Result ret = allocator->BuildFromBlobs(blobList);
        globalAllocLock_.UnlockRead();
        return ret;
    }

    void GetUsedInfo(uint64_t (&totalSize)[MEDIA_NONE], uint64_t (&usedSize)[MEDIA_NONE])
    {
        globalAllocLock_.LockRead();
        for (auto &allocator : allocators_) {
            auto result = allocator.second->GetUsageInfo();
            totalSize[allocator.first.mediaType_] += result.first;
            usedSize[allocator.first.mediaType_] += result.second;
        }
        globalAllocLock_.UnlockRead();
    }

    uint64_t GetFreeSpace(MediaType type)
    {
        if (type == MEDIA_NONE || type == MEDIA_SSD) {
            return 0;
        }
        uint64_t totalSize[MEDIA_NONE] = {0};
        uint64_t usedSize[MEDIA_NONE] = {0};
        GetUsedInfo(totalSize, usedSize);
        return totalSize[type] - usedSize[type];
    }

    /**
     * @brief 查询指定介质在所有 rank 上最大的单个连续空闲块大小
     * @param type    [in] 介质类型，MEDIA_NONE 表示最顶层介质
     * @return 最大连续空闲块大小，无可用分配器时为 0
     */
    uint64_t GetMaxContiguousFreeSize(MediaType type)
    {
        if (type == MEDIA_NONE) {
            type = GetTopLayerMediumType();
        }
        if (type == MEDIA_NONE || type == MEDIA_SSD) {
            return 0;
        }
        globalAllocLock_.LockRead();
        uint64_t maxFreeSize = 0;
        for (const auto &[loc, allocator] : allocators_) {
            if (loc.mediaType_ != type || allocator == nullptr) {
                continue;
            }
            maxFreeSize = std::max(maxFreeSize, allocator->GetMaxContiguousFreeSize());
        }
        globalAllocLock_.UnlockRead();
        return maxFreeSize;
    }

    /**
     * @brief 查询指定 location 的空闲空间概要：总空闲大小与最大连续空闲块大小
     * @param loc    [in] 位置（rank + 介质）
     * @return (总空闲大小, 最大连续空闲块大小)，location 不存在时为 (0, 0)
     */
    std::pair<uint64_t, uint64_t> GetLocFreeSummary(const MmcLocation &loc)
    {
        if (loc.mediaType_ == MEDIA_NONE || loc.mediaType_ == MEDIA_SSD) {
            return {0, 0};
        }
        globalAllocLock_.LockRead();
        std::pair<uint64_t, uint64_t> freeSummary{0, 0};
        const auto iter = allocators_.find(loc);
        if (iter != allocators_.end() && iter->second != nullptr) {
            const auto usage = iter->second->GetUsageInfo();
            freeSummary.first = usage.first - usage.second;
            freeSummary.second = iter->second->GetMaxContiguousFreeSize();
        }
        globalAllocLock_.UnlockRead();
        return freeSummary;
    }

    /**
     * @brief 获取指定 location 分配器 addressTree 中的空闲地址段快照（gva 绝对地址）
     * @param loc    [in] 位置（rank + 介质）
     * @return 空闲地址段列表，location 不存在时为空
     */
    std::vector<SpaceRange> GetFreeSegments(const MmcLocation &loc)
    {
        if (loc.mediaType_ == MEDIA_NONE || loc.mediaType_ == MEDIA_SSD) {
            return {};
        }
        globalAllocLock_.LockRead();
        std::vector<SpaceRange> segments;
        const auto iter = allocators_.find(loc);
        if (iter != allocators_.end() && iter->second != nullptr) {
            segments = iter->second->GetFreeSegments();
        }
        globalAllocLock_.UnlockRead();
        return segments;
    }

    /**
     * @brief 枚举指定介质已挂载的所有 location（rank + 介质），按 map 序（rank 升序）
     * @param type    [in] 介质类型
     * @return location 列表，无挂载分配器时为空
     */
    std::vector<MmcLocation> GetMediaLocations(MediaType type)
    {
        std::vector<MmcLocation> locations;
        if (type == MEDIA_NONE || type == MEDIA_SSD) {
            return locations;
        }
        globalAllocLock_.LockRead();
        for (const auto &[loc, allocator] : allocators_) {
            if (loc.mediaType_ == type && allocator != nullptr) {
                locations.push_back(loc);
            }
        }
        globalAllocLock_.UnlockRead();
        return locations;
    }

    /**
     * @brief 判断一次分配失败是否由内存碎片导致：总空闲足够（非容量耗尽），但申请大小超过系统中最大的单个连续空闲块
     * @param media       [in] 介质类型，MEDIA_NONE 表示最顶层介质
     * @param wantSize    [in] 申请的 blob 数据大小
     * @return true 表示属于碎片场景，可通过强制淘汰整理出满足申请的连续内存
     */
    bool IsFragmentation(MediaType media, uint64_t wantSize)
    {
        if (media == MEDIA_NONE) {
            media = GetTopLayerMediumType();
        }
        if (media == MEDIA_NONE || media == MEDIA_SSD) {
            return false;
        }
        auto alignedSize = MmcBlobAllocator::AllocSizeAlignUp(wantSize);
        // 总空闲不足属于容量耗尽而非碎片：应由常规水位淘汰（可搬移到低层介质）处理，
        // 强制淘汰直接丢弃数据，不应在容量不足时触发
        if (GetFreeSpace(media) < alignedSize) {
            return false;
        }
        return GetMaxContiguousFreeSize(media) < alignedSize;
    }

    /**
     * @brief 获取指定介质上因碎片化无法满足申请大小的 rank 列表，用于限定强制淘汰的目标 rank。
     *        不同 rank 的 gva 地址空间独立，跨 rank 释放无法为失败 rank 整理出连续内存。
     *        Alloc 经 ArrangeLocality/RandomAssign 可能在任意 rank 上失败，须检查所有已挂载 rank
     * @param media          [in] 介质类型，MEDIA_NONE 表示最顶层介质
     * @param wantSize       [in] 申请的 blob 数据大小
     * @return 碎片化 rank 列表（总空闲 ≥ 申请但最大连续空闲块 < 申请），无碎片化 rank 时为空
     */
    std::vector<uint32_t> GetFragmentedRanks(MediaType media, uint64_t wantSize)
    {
        if (media == MEDIA_NONE) {
            media = GetTopLayerMediumType();
        }
        if (media == MEDIA_NONE || media == MEDIA_SSD) {
            return {};
        }
        auto alignedSize = MmcBlobAllocator::AllocSizeAlignUp(wantSize);
        std::vector<uint32_t> ranks;
        globalAllocLock_.LockRead();
        for (const auto &[loc, allocator] : allocators_) {
            if (loc.mediaType_ != media || allocator == nullptr) {
                continue;
            }
            auto [total, used] = allocator->GetUsageInfo();
            uint64_t freeSpace = total - used;
            // 总空闲不足属于容量耗尽：淘汰少量 key 无法补足，跳过
            if (freeSpace >= alignedSize && allocator->GetMaxContiguousFreeSize() < alignedSize) {
                ranks.push_back(loc.rank_);
            }
        }
        globalAllocLock_.UnlockRead();
        return ranks;
    }

    MediaType GetTopLayerMediumType()
    {
        MediaType result = MEDIA_NONE;
        globalAllocLock_.LockRead();
        for (const auto &[location, allocator] : allocators_) {
            result = location.mediaType_;
            break;
        }
        globalAllocLock_.UnlockRead();
        return result;
    }

    bool IsAboveUsageRatio(MediaType type, uint16_t watermark)
    {
        if (type == MEDIA_NONE || type == MEDIA_SSD || watermark == 0) {
            return false;
        }
        globalAllocLock_.LockRead();
        uint64_t total = 0;
        uint64_t used = 0;
        for (const auto &[loc, allocator] : allocators_) {
            if (loc.mediaType_ != type || allocator == nullptr) {
                continue;
            }
            auto [t, u] = allocator->GetUsageInfo();
            total += t;
            used += u;
        }
        globalAllocLock_.UnlockRead();
        if (total == 0) {
            return false;
        }
        if (used > std::numeric_limits<uint64_t>::max() / LEVEL_BASE) {
            MMC_LOG_ERROR("overflow in IsAboveUsageRatio: used=" << used << ", LEVEL_BASE=" << LEVEL_BASE);
            return false;
        }
        return (used * LEVEL_BASE / total) >= watermark;
    }

    std::vector<MediaType> GetNeedEvictList(const std::vector<std::pair<uint16_t, uint16_t>> &evictWatermarks,
                                            std::vector<uint16_t> &nowMemoryThresholds, MediaType media,
                                            uint64_t wantAllocSize)
    {
        if (media == MEDIA_NONE) {
            media = GetTopLayerMediumType();
        }
        std::vector<MediaType> results;
        uint64_t totalSize[MEDIA_NONE] = {0};
        uint64_t usedSize[MEDIA_NONE] = {0};
        GetUsedInfo(totalSize, usedSize);
        for (int i = MEDIA_NONE - 1; i >= 0; i--) {
            if (static_cast<MediaType>(i) == MEDIA_SSD) {
                continue;
            }
            uint16_t level = evictWatermarks[i].first;
            if (usedSize[i] > std::numeric_limits<uint64_t>::max() / LEVEL_BASE ||
                (level != 0 && (totalSize[i] > std::numeric_limits<uint64_t>::max() / level))) {
                MMC_LOG_ERROR("overflow: usedSize: " << usedSize[i] << ", LEVEL_BASE: " << LEVEL_BASE
                                                     << ", totalSize: " << totalSize[i] << ", level: " << level);
                continue;
            }
            if (usedSize[i] * LEVEL_BASE > totalSize[i] * level) {
                uint16_t nowMemoryThreshold = usedSize[i] * LEVEL_BASE / totalSize[i];
                results.push_back(static_cast<MediaType>(i));
                nowMemoryThresholds.push_back(nowMemoryThreshold);
                MMC_LOG_DEBUG("Medium " << static_cast<MediaType>(i) << " need evict, usedSize: " << usedSize[i]
                                        << ", totalSize: " << totalSize[i]);
            } else {
                if (media == static_cast<MediaType>(i) && (totalSize[i] - usedSize[i] < wantAllocSize)) {
                    MMC_LOG_WARN("Medium " << static_cast<MediaType>(i)
                                           << " need evict, but the alloc size of req is greater than remain, and it "
                                              "does not meet the conditions of high evict level , the "
                                              "elimination cannot be triggered, usedSize: "
                                           << usedSize[i] << ", totalSize: " << totalSize[i]
                                           << ", wantAllocSize: " << wantAllocSize);
                }
            }
        }
        return results;
    }

    nlohmann::json GetAllSegmentInfo()
    {
        globalAllocLock_.LockRead();
        nlohmann::json segmentArray = nlohmann::json::array();
        for (const auto &[fst, snd] : allocators_) {
            const auto &allocator = snd;
            if (allocator == nullptr) {
                MMC_LOG_ERROR("allocator is nullptr");
                continue;
            }
            segmentArray.push_back(allocator->GetInfo());
        }
        globalAllocLock_.UnlockRead();
        return segmentArray;
    }

    uint64_t GetMaxAlignedSize() const
    {
        return maxAlignedSize_.load(std::memory_order_relaxed);
    }

private:
    Result InnerAlloc(const AllocOptions &allocReq, std::vector<MmcMemBlobPtr> &blobs,
                      std::unordered_set<uint32_t> &excludeRanks) const
    {
        if (allocators_.empty()) {
            MMC_LOG_ERROR("Alloc allocators_ is empty");
            return MMC_ERROR;
        }
        Result ret;
        switch (allocReq.flags_ & 0xFF) {
            case ALLOC_ARRANGE:
                ret = MmcLocalityStrategy::ArrangeLocality(allocators_, allocReq, blobs, excludeRanks);
                break;

            case ALLOC_FORCE_BY_RANK:
                ret = MmcLocalityStrategy::ForceAssign(allocators_, allocReq, blobs);
                break;

            case ALLOC_RANDOM:
                ret = MmcLocalityStrategy::RandomAssign(allocators_, allocReq, blobs, excludeRanks);
                break;

            default:
                ret = MmcLocalityStrategy::ArrangeLocality(allocators_, allocReq, blobs, excludeRanks);
                break;
        }
        return ret;
    }

    // maxAlignedSize_ 声明在 allocators_ 之前，确保析构时 blob allocator 先销毁（它们持有此指针）
    std::atomic<uint64_t> maxAlignedSize_{0};
    MmcAllocators allocators_;
    ReadWriteLock globalAllocLock_;
};

using MmcGlobalAllocatorPtr = MmcRef<MmcGlobalAllocator>;

} // namespace mmc
} // namespace ock

#endif // MEM_FABRIC_MMC_GLOBAL_ALLOCATOR_H
