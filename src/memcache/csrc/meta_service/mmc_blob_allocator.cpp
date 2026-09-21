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

#include "mmc_blob_allocator.h"

#include <cstdint>
#include <limits>

namespace ock {
namespace mmc {
bool MmcBlobAllocator::CanAlloc(uint64_t blobSize)
{
    if (!started_) {
        MMC_LOG_WARN("Allocator rank: " << rank_ << " mediaType: " << mediaType_ << " is stopped");
        return false;
    }
    auto alignedSize = AllocSizeAlignUp(blobSize);

    spinlock_.lock();
    if (!started_) {
        spinlock_.unlock();
        MMC_LOG_WARN("Allocator rank: " << rank_ << " mediaType: " << mediaType_ << " is stopped");
        return false;
    }
    if (enableMaxValueMode_) {
        alignedSize = std::max(maxAlignedSize_->load(std::memory_order_relaxed), alignedSize);
    }
    SpaceRange anchor{0, alignedSize};
    bool exists = (sizeTree_.lower_bound(anchor) != sizeTree_.end());
    spinlock_.unlock();

    return exists;
}

MmcMemBlobPtr MmcBlobAllocator::Alloc(uint64_t blobSize)
{
    if (!started_) {
        MMC_LOG_WARN("Allocator rank: " << rank_ << " mediaType: " << mediaType_ << " is stopped");
        return nullptr;
    }
    auto alignedSize = AllocSizeAlignUp(blobSize);

    spinlock_.lock();
    if (!started_) {
        spinlock_.unlock();
        MMC_LOG_WARN("Allocator rank: " << rank_ << " mediaType: " << mediaType_ << " is stopped");
        return nullptr;
    }
    auto maxAlignedSize = maxAlignedSize_->load(std::memory_order_relaxed);
    uint64_t allocSize = enableMaxValueMode_ ? std::max(maxAlignedSize, alignedSize) : alignedSize;

    SpaceRange anchor{0, allocSize};
    auto sizePos = sizeTree_.lower_bound(anchor);
    if (sizePos == sizeTree_.end()) {
        spinlock_.unlock();
        MMC_LOG_WARN("Allocator rank: " << rank_ << " mediaType: " << mediaType_ << ", cap:" << allocatedSize_ << "/"
                                        << capacity_ << " cannot allocate with size: " << blobSize);
        return nullptr;
    }

    auto targetOffset = sizePos->offset_;
    auto targetSize = sizePos->size_;
    auto addrPos = addressTree_.find(targetOffset);
    if (addrPos == addressTree_.end()) {
        spinlock_.unlock();
        MMC_LOG_ERROR("Allocator rank: " << rank_ << " mediaType: " << mediaType_
                                         << " offset in size tree, not in address tree");
        return nullptr;
    }

    sizeTree_.erase(sizePos);
    addressTree_.erase(addrPos);
    if (targetSize > allocSize) {
        SpaceRange left{targetOffset + allocSize, targetSize - allocSize};
        addressTree_.emplace(left.offset_, left.size_);
        sizeTree_.emplace(left);
    }
    // CAS-max：只在创新高时写入，稳态零写入；并发下只增不减，不丢更新
    uint64_t currentMax = maxAlignedSize_->load(std::memory_order_relaxed);
    while (alignedSize > currentMax &&
           !maxAlignedSize_->compare_exchange_weak(currentMax, alignedSize, std::memory_order_relaxed)) {}
    allocatedSize_ += allocSize;
    spinlock_.unlock();

    MMC_LOG_DEBUG("Alloc rank=" << rank_ << ", type=" << mediaType_ << " allocated=" << blobSize
                                << " total=" << allocatedSize_ << "/" << capacity_);
    return MmcMakeRef<MmcMemBlob>(rank_, bmAddr_ + targetOffset, blobSize, mediaType_, ALLOCATED, allocSize);
}

Result MmcBlobAllocator::Release(const MmcMemBlobPtr &blob)
{
    if (blob == nullptr) {
        MMC_LOG_ERROR("blob is null");
        return MMC_ERROR;
    }
    auto alignedSize = AllocSizeAlignUp(blob->AllocSize());
    MMC_ASSERT_LOG_AND_RETURN(allocatedSize_ >= alignedSize,
                              "allocatedSize_ = " << allocatedSize_ << ", alignedSize = " << alignedSize, MMC_ERROR);
    auto blobAddr = blob->Gva();
    if (blobAddr < bmAddr_ || blobAddr + alignedSize > bmAddr_ + capacity_) {
        MMC_LOG_ERROR("blob address not in allocator");
        return MMC_ERROR;
    }

    auto offset = blobAddr - bmAddr_;
    uint64_t finalOffset = offset;
    uint64_t finalSize = alignedSize;

    spinlock_.lock();
    auto prevAddrPos = addressTree_.lower_bound(offset);
    if (prevAddrPos != addressTree_.end() && prevAddrPos->first == offset) {
        spinlock_.unlock();
        MMC_LOG_ERROR("blob already released");
        return MMC_ERROR;
    }
    if (prevAddrPos != addressTree_.begin()) {
        --prevAddrPos;
        if (prevAddrPos->first + prevAddrPos->second > offset) {
            spinlock_.unlock();
            MMC_LOG_ERROR("blob already released");
            return MMC_ERROR;
        }
        if (prevAddrPos != addressTree_.end() &&
            prevAddrPos->first + prevAddrPos->second == offset) { // 合并前一个range
            finalOffset = prevAddrPos->first;
            finalSize += prevAddrPos->second;
            sizeTree_.erase(SpaceRange{prevAddrPos->first, prevAddrPos->second});
            addressTree_.erase(prevAddrPos);
        }
    }

    auto nextAddrPos = addressTree_.find(offset + alignedSize);
    if (nextAddrPos != addressTree_.end()) {
        finalSize += nextAddrPos->second;
        sizeTree_.erase(SpaceRange{nextAddrPos->first, nextAddrPos->second});
        addressTree_.erase(nextAddrPos);
    }

    addressTree_.emplace(finalOffset, finalSize);
    sizeTree_.emplace(SpaceRange{finalOffset, finalSize});

    allocatedSize_ -= alignedSize;

    spinlock_.unlock();
    MMC_LOG_DEBUG("Release rank=" << rank_ << ", type=" << mediaType_ << " released=" << blob->Size()
                                  << " total=" << allocatedSize_ << "/" << capacity_);
    return MMC_OK;
}

uint64_t MmcBlobAllocator::GetMaxContiguousFreeSize()
{
    spinlock_.lock();
    // sizeTree_ 按空闲块大小升序排列，最后一个元素即最大连续空闲块
    uint64_t maxFreeSize = sizeTree_.empty() ? 0 : sizeTree_.rbegin()->size_;
    spinlock_.unlock();
    return maxFreeSize;
}

std::vector<SpaceRange> MmcBlobAllocator::GetFreeSegments()
{
    std::vector<SpaceRange> segments;
    spinlock_.lock();
    segments.reserve(addressTree_.size());
    // addressTree_ 以 offset 为 key 有序，转换回 gva 绝对地址后仍按地址升序
    for (const auto &[offset, size] : addressTree_) {
        segments.emplace_back(bmAddr_ + offset, size);
    }
    spinlock_.unlock();
    return segments;
}

Result MmcBlobAllocator::BuildFromBlobs(std::vector<std::pair<std::string, MmcMemBlobDesc>> &blobList)
{
    spinlock_.lock();
    if (started_) {
        spinlock_.unlock();
        MMC_LOG_ERROR("rebuild allocator failed, rank: " << rank_ << " mediaType: " << mediaType_
                                                         << ", allocator must not started and empty");
        return MMC_ERROR;
    }

    // 处理每个已分配的blob
    for (auto it = blobList.begin(); it != blobList.end();) {
        if (it->second.rank_ != rank_) {
            MMC_LOG_WARN("rebuild blob not match, allocator rank: " << rank_ << ", blob rank: " << it->second.rank_);
            it = blobList.erase(it);
            continue;
        }

        if (it->second.mediaType_ != mediaType_) {
            MMC_LOG_WARN("rebuild blob not match, allocator mediaType: " << mediaType_ << ", blob mediaType: "
                                                                         << it->second.mediaType_);
            ++it;
            continue;
        }
        uint64_t gva = it->second.gva_;
        uint64_t size = it->second.size_;
        uint64_t offset = gva - bmAddr_; // 转换为分配器内部偏移

        Result res = ValidateAndAddAllocation(offset, size, it->second.allocSize_);
        if (res != MMC_OK) {
            MMC_LOG_ERROR("rebuild allocator failed, rank: " << rank_ << " mediaType: " << mediaType_
                                                             << ", blob off:" << offset << ", size: " << size);
            it = blobList.erase(it);
            continue;
        }
        // 按恢复数据重建历史最大对齐尺寸，避免重启后 max 模式从 0 重新爬升导致切块粒度漂移。
        // recorded 与 ValidateAndAddAllocation 的切块值严格一致，回填给 desc 作为单一真相源：
        // RebuildMeta 重建 Blob、Release 归还、后续 GetDesc 传播均使用该值，旧数据（allocSize_=0）自愈
        uint64_t recorded = std::max(AllocSizeAlignUp(it->second.allocSize_), AllocSizeAlignUp(size));
        it->second.allocSize_ = recorded;
        // CAS-max：恢复时多 rank 可并行（各持各自的锁），CAS 保证全局 max 只增不减
        uint64_t currentMax = maxAlignedSize_->load(std::memory_order_relaxed);
        while (recorded > currentMax &&
               !maxAlignedSize_->compare_exchange_weak(currentMax, recorded, std::memory_order_relaxed)) {}
        MMC_LOG_DEBUG("rebuild block successful, rank: " << it->second);
        ++it;
    }
    spinlock_.unlock();
    return MMC_OK;
}

Result MmcBlobAllocator::ValidateAndAddAllocation(uint64_t offset, uint64_t size, uint64_t allocSize)
{
    // 验证地址范围有效性
    if (offset > std::numeric_limits<uint64_t>::max() - size) {
        MMC_LOG_ERROR("blob range overflow: offset:" << offset << ", size:" << size);
        return MMC_ERROR;
    }
    if (offset >= capacity_ || (offset + size) > capacity_) {
        MMC_LOG_ERROR("Blob out of range: offset: " << offset << ", size: " << size << ", capacity: " << capacity_);
        return MMC_ERROR;
    }

    // 切块大小优先用 Desc 记录的真实值（已对齐，max 模式下大于对齐值）；防御性取较大，
    // 同时覆盖 allocSize 为 0（旧数据回退对齐值）与小于对齐值（异常数据）两种情况
    uint64_t alignedSize = std::max(AllocSizeAlignUp(allocSize), AllocSizeAlignUp(size));
    if (offset > std::numeric_limits<uint64_t>::max() - alignedSize) {
        MMC_LOG_ERROR("offset plus size overflow: offset=" << offset << ", size=" << size);
        return MMC_ERROR;
    }

    // 查找包含该分配的空闲块
    auto it = addressTree_.upper_bound(offset);
    if (it != addressTree_.begin()) {
        --it; // 回退到可能包含offset的块
    }

    if (it == addressTree_.end() || it->first > offset || (it->first + it->second) < (offset + alignedSize)) {
        MMC_LOG_ERROR("No matching free block for blob: offset=" << offset << ", size=" << size);
        return MMC_ERROR;
    }

    // 从空闲树中移除该块
    uint64_t blockOffset = it->first;
    uint64_t blockSize = it->second;
    sizeTree_.erase(SpaceRange{blockOffset, blockSize});
    addressTree_.erase(it);

    // 分割剩余空间（前部）
    if (blockOffset < offset) {
        uint64_t frontSize = offset - blockOffset;
        SpaceRange left{blockOffset, frontSize};
        addressTree_.emplace(left.offset_, left.size_);
        sizeTree_.emplace(left);
    }

    // 分割剩余空间（后部）
    uint64_t endOffset = offset + alignedSize;
    uint64_t remainingSize = (blockOffset + blockSize) - endOffset;
    if (remainingSize > 0) {
        SpaceRange left{endOffset, remainingSize};
        addressTree_.emplace(left.offset_, left.size_);
        sizeTree_.emplace(left);
    }

    // 更新已分配大小
    allocatedSize_ += alignedSize;
    return MMC_OK;
}

uint64_t MmcBlobAllocator::AllocSizeAlignUp(uint64_t size)
{
    constexpr uint64_t alignSize = 4096UL;
    if (size > UINT64_MAX - alignSize + 1UL) {
        MMC_LOG_ERROR("Invalid size: " << size << " will occur overflow");
        return UINT64_MAX;
    }
    constexpr uint64_t alignSizeMask = ~(alignSize - 1UL);
    return (size + alignSize - 1UL) & alignSizeMask;
}
} // namespace mmc
} // namespace ock
