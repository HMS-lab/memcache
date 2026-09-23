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
#ifndef MEM_FABRIC_MMC_BLOB_ALLOCATOR_H
#define MEM_FABRIC_MMC_BLOB_ALLOCATOR_H

#include <atomic>

#include "nlohmann/json.hpp"

#include "mmc_mem_blob.h"
#include "mmc_spinlock.h"

namespace ock {
namespace mmc {

#define SIZE_32K (uint64_t)(32 * 1024)

struct SpaceRange {
    const uint64_t offset_ = 0;
    const uint64_t size_ = 0;

    SpaceRange(const uint64_t offset, const uint64_t size) : offset_(offset), size_(size) {}
};

struct RangeSizeFirst {
    bool operator()(const SpaceRange &sr1, const SpaceRange &sr2) const
    {
        if (sr1.size_ != sr2.size_) {
            return sr1.size_ < sr2.size_;
        }
        return sr1.offset_ < sr2.offset_;
    }
};

class MmcBlobAllocator : public MmcReferable {
public:
    MmcBlobAllocator(const uint32_t rank, const MediaType mediaType, const uint64_t bmAddr, const uint64_t capacity,
                     bool enableMaxValueMode, std::atomic<uint64_t> *maxAlignedSize)
        : rank_(rank), mediaType_(mediaType), bmAddr_(bmAddr), capacity_(capacity),
          enableMaxValueMode_(enableMaxValueMode), maxAlignedSize_(maxAlignedSize)
    {
        started_ = false;
        addressTree_[0] = capacity;
        sizeTree_.insert({0, capacity});
    }
    ~MmcBlobAllocator() override = default;

    static MmcRef<MmcBlobAllocator> Create(const MmcLocation &loc, const MmcLocalMemlInitInfo &info,
                                           bool enableMaxValueMode, std::atomic<uint64_t> *maxAlignedSize);

    virtual bool CanAlloc(uint64_t blobSize);
    virtual MmcMemBlobPtr Alloc(uint64_t blobSize);
    virtual Result Release(const MmcMemBlobPtr &blob);
    virtual Result BuildFromBlobs(std::vector<std::pair<std::string, MmcMemBlobDesc>> &blobList);

    /**
     * @brief 查询当前最大的单个连续空闲块大小
     * @return 最大连续空闲块大小，无空闲时为 0
     */
    virtual uint64_t GetMaxContiguousFreeSize();

    /**
     * @brief 获取 addressTree 中记录的空闲地址段快照（gva 绝对地址）
     * @return 空闲地址段列表，按地址升序排列
     */
    virtual std::vector<SpaceRange> GetFreeSegments();

    static uint64_t AllocSizeAlignUp(uint64_t size);
    void Start()
    {
        spinlock_.lock();
        started_ = true;
        spinlock_.unlock();
    }
    void Stop()
    {
        spinlock_.lock();
        started_ = false;
        spinlock_.unlock();
    }
    bool CanUnmount()
    {
        return allocatedSize_ == 0;
    }

    std::pair<uint64_t, uint64_t> GetUsageInfo()
    {
        return std::make_pair(capacity_, allocatedSize_);
    }

    // 实时内部碎片 = 已切块总量 (含对齐/max模式放大) - 活跃blob逻辑请求字节之和
    uint64_t GetFragmentationBytes() const
    {
        spinlock_.lock();
        uint64_t internalFragBytes = allocatedSize_ - requestedSize_;
        spinlock_.unlock();
        return internalFragBytes;
    }

    nlohmann::json GetInfo() const
    {
        nlohmann::json info;
        info["rank"] = rank_;
        info["medium"] = MediumTypeToString(mediaType_);
        info["bmAddr"] = bmAddr_;
        info["capacity"] = capacity_;
        info["allocatedSize"] = allocatedSize_;
        info["fragmentationBytes"] = GetFragmentationBytes();
        return info;
    }

protected:
    Result ValidateAndAddAllocation(uint64_t offset, uint64_t size, uint64_t allocSize = 0);

protected:
    const uint32_t rank_;       /* rank id of the space */
    const MediaType mediaType_; /* media type of the space */
    const uint64_t bmAddr_;     /* bm address */
    const uint64_t capacity_;   /* capacity of the space */

    std::map<uint64_t, uint64_t> addressTree_;
    std::set<SpaceRange, RangeSizeFirst> sizeTree_;

    volatile bool started_ = false;
    uint64_t allocatedSize_ = 0;
    uint64_t requestedSize_ = 0; // 活跃blob逻辑请求字节之和 (Alloc增/Release减/恢复回填)，用于实时内部碎片统计
    bool enableMaxValueMode_ = false;
    std::atomic<uint64_t> *maxAlignedSize_; // 指向 GlobalAllocator 的全局 maxAlignedSize_

    mutable Spinlock spinlock_; // mutable：const 查询接口 (GetInfo/GetFragmentationBytes) 内部加锁
};

inline MmcRef<MmcBlobAllocator> MmcBlobAllocator::Create(const MmcLocation &loc, const MmcLocalMemlInitInfo &info,
                                                         bool enableMaxValueMode, std::atomic<uint64_t> *maxAlignedSize)
{
    return MmcMakeRef<MmcBlobAllocator>(loc.rank_, loc.mediaType_, info.bmAddr_, info.capacity_, enableMaxValueMode,
                                        maxAlignedSize);
}

using MmcBlobAllocatorPtr = MmcRef<MmcBlobAllocator>;

} // namespace mmc
} // namespace ock

#endif // MEM_FABRIC_MMC_BLOB_ALLOCATOR_H
