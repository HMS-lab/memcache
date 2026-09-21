/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemCache_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
*/
#ifndef MF_HYBRID_MMC_DEFRAG_EVICT_H
#define MF_HYBRID_MMC_DEFRAG_EVICT_H

#include <algorithm>
#include <cstdint>
#include <unordered_set>
#include <vector>

#include "mmc_blob_allocator.h"

namespace ock {
namespace mmc {

/* 可用块阈值：空闲块 >= 该值（3MB）才视为可直接满足大块 put 分配的可用空闲，
   小于该值的空闲块计入碎片浪费（不可用空闲） */
constexpr uint64_t DEFRAG_EVICT_MIN_USABLE_BLOCK_SIZE = 3ULL * 1024 * 1024;
/* 碎片浪费率阈值：不可用空闲占空闲总量的百分比达到该值才触发整理 */
constexpr uint64_t DEFRAG_EVICT_WASTE_PERCENT_THRESHOLD = 70U;
constexpr uint64_t DEFRAG_EVICT_PERCENT_DENOM = 100U;

/* 计算碎片浪费率（不可用空闲占空闲总量的百分比）：空闲段不足两段时无碎片，返回 0。 */
inline uint64_t GetDefragWastePercent(const std::vector<SpaceRange> &freeSegments,
                                      uint64_t minUsableBlockSize = DEFRAG_EVICT_MIN_USABLE_BLOCK_SIZE)
{
    if (freeSegments.size() <= 1) {
        return 0;
    }
    uint64_t totalFree = 0;
    uint64_t usableFree = 0;
    for (const auto &seg : freeSegments) {
        totalFree += seg.size_;
        if (seg.size_ >= minUsableBlockSize) {
            usableFree += seg.size_;
        }
    }
    return (totalFree - usableFree) * DEFRAG_EVICT_PERCENT_DENOM / totalFree;
}

inline bool IsDefragWorthy(const std::vector<SpaceRange> &freeSegments, uint64_t &wastePercent,
                           uint64_t wastePercentThreshold = DEFRAG_EVICT_WASTE_PERCENT_THRESHOLD,
                           uint64_t minUsableBlockSize = DEFRAG_EVICT_MIN_USABLE_BLOCK_SIZE)
{
    wastePercent = GetDefragWastePercent(freeSegments, minUsableBlockSize);
    return wastePercent >= wastePercentThreshold;
}

/* blob 索引条目：已分配 blob 在全量段图中的区间与归属（按 gva 升序排序后参与归并） */
struct DefragBlobEntry {
    uint64_t gva_ = 0;        /* blob 起始 gva */
    uint64_t size_ = 0;       /* 4K 对齐后的占用大小 */
    uint32_t keyIdx_ = 0;     /* 归属 key 的编号（由索引构建方分配） */
    bool releasable_ = false; /* 冷 key 且 READABLE，窗口贪心中可与空闲段合并 */
};

/**
 * @brief 窗口贪心扫描器：归并空闲段与 blob 索引为全量段图，
 *        收集所有"地址连续可释放"窗口，按合并空间降序贪心选取至目标数量。
 *        Process 按 gva 升序调用（归并循环保证），winKeys_ 保持插入顺序即 gva 升序，
 *        Select 中无需对 key 排序，选取复杂度 O(k)
 */
class DefragWindowScanner {
public:
    void Settle()
    {
        if (hasWin_ && !winKeys_.empty()) {
            windows_.push_back({std::move(winKeys_), wEnd_ - wStart_});
        }
        hasWin_ = false;
        winKeys_.clear();
        winSeen_.clear();
    }

    void Process(uint64_t start, uint64_t end, bool isFree, bool releasable, uint32_t key)
    {
        if (!isFree && !releasable) {
            Settle();
            return;
        }
        if (hasWin_ && start > wEnd_) {
            Settle();
        }
        if (!hasWin_) {
            hasWin_ = true;
            wStart_ = start;
            wEnd_ = end;
        } else {
            wEnd_ = std::max(wEnd_, end);
        }
        if (!isFree && winSeen_.insert(key).second) {
            winKeys_.push_back(key); // 插入顺序即 gva 升序
        }
    }

    std::vector<uint32_t> Select(size_t target)
    {
        // 按合并后可获取的空间降序排窗口，同大取 key 数更多者
        std::sort(windows_.begin(), windows_.end(), [](const Window &a, const Window &b) {
            if (a.mergedSize_ != b.mergedSize_) {
                return a.mergedSize_ > b.mergedSize_;
            }
            return a.keys_.size() > b.keys_.size();
        });
        std::vector<uint32_t> selected;
        for (const auto &win : windows_) {
            // keys_ 已按 gva 升序，直接把 win.keys 放入 selected，放够目标即停
            selected.insert(selected.end(), win.keys_.begin(), win.keys_.end());
            if (selected.size() >= target) {
                break;
            }
        }
        return selected;
    }

private:
    struct Window {
        std::vector<uint32_t> keys_;
        uint64_t mergedSize_;
    };
    std::vector<Window> windows_;
    std::vector<uint32_t> winKeys_;        // 当前窗口 key，保持 gva 升序
    std::unordered_set<uint32_t> winSeen_; // 当前窗口 key 去重
    uint64_t wStart_ = 0;
    uint64_t wEnd_ = 0;
    bool hasWin_ = false;
};

/**
 * @brief 归并空闲段与 blob 索引为全量段图，窗口贪心选出地址连续可释放的 key 序列。
 *        契约：freeSegments 与 blobIdx 均须按 gva 升序（由调用方保证）。
 *        freeSegments 来自分配器 addressTree 快照，blobIdx 由调用方收集后排序保证。
 */
inline std::vector<uint32_t> PickDefragWindow(const std::vector<SpaceRange> &freeSegments,
                                              const std::vector<DefragBlobEntry> &blobIdx, size_t releaseTarget)
{
    DefragWindowScanner scanner;
    for (size_t f = 0, b = 0; f < freeSegments.size() || b < blobIdx.size();) {
        bool takeFree = f < freeSegments.size() && (b >= blobIdx.size() || freeSegments[f].offset_ <= blobIdx[b].gva_);
        if (takeFree) {
            const auto &seg = freeSegments[f++];
            scanner.Process(seg.offset_, seg.offset_ + seg.size_, true, false, 0);
        } else {
            const auto &entry = blobIdx[b++];
            scanner.Process(entry.gva_, entry.gva_ + entry.size_, false, entry.releasable_, entry.keyIdx_);
        }
    }
    scanner.Settle();
    return scanner.Select(releaseTarget);
}

} // namespace mmc
} // namespace ock

#endif // MF_HYBRID_MMC_DEFRAG_EVICT_H
