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
#include <cstdint>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"

#include "mmc_mem_blob.h"
#include "mmc_spinlock.h"

namespace ock {
namespace mmc {

#define SIZE_32K (uint64_t)(32 * 1024)

// Parametry bazowe Slab Allocatora.
constexpr uint64_t SLAB_DEFAULT_FRAME_SIZE = 1536ULL * 1024ULL * 1024ULL; // 1.5 GiB
constexpr uint64_t SLAB_MIN_ALIGN_SIZE = 4096ULL;                         // 4 KiB (wyrównanie sprzętowe)
constexpr size_t SLAB_MAX_POOLS = 256;                                    // Maksymalna liczba dynamicznych klas

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
                                           bool enableMaxValueMode, std::atomic<uint64_t> *maxAlignedSize,
                                           bool enableSlabMode = false);

    virtual bool CanAlloc(uint64_t blobSize);
    virtual bool IsFragmentedFor(uint64_t alignedSize)
    {
        return GetMaxContiguousFreeSize() < alignedSize;
    }
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
    virtual void Stop()
    {
        spinlock_.lock();
        started_ = false;
        spinlock_.unlock();
    }
    bool CanUnmount()
    {
        return __atomic_load_n(&allocatedSize_, __ATOMIC_RELAXED) == 0;
    }

    std::pair<uint64_t, uint64_t> GetUsageInfo()
    {
        uint64_t alloc = __atomic_load_n(&allocatedSize_, __ATOMIC_RELAXED);
        return std::make_pair(capacity_, alloc);
    }

    // 实时内部碎片 = 已切块总量 (含对齐/max模式放大) - 活跃blob逻辑请求字节之和
    virtual uint64_t GetFragmentationBytes() const
    {
        spinlock_.lock();
        uint64_t alloc = __atomic_load_n(&allocatedSize_, __ATOMIC_RELAXED);
        uint64_t req = __atomic_load_n(&requestedSize_, __ATOMIC_RELAXED);
        uint64_t internalFragBytes = (alloc >= req) ? (alloc - req) : 0;
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
        info["allocatedSize"] = __atomic_load_n(&allocatedSize_, __ATOMIC_RELAXED);
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

    std::atomic<bool> started_ = false;
    uint64_t allocatedSize_ = 0;
    uint64_t requestedSize_ = 0; // 活跃blob逻辑请求字节之和 (Alloc增/Release减/恢复回填)，用于实时内部碎片统计
    bool enableMaxValueMode_ = false;
    std::atomic<uint64_t> *maxAlignedSize_; // 指向 GlobalAllocator 的全局 maxAlignedSize_

    mutable Spinlock spinlock_; // mutable：const 查询接口 (GetInfo/GetFragmentationBytes) 内部加锁
};

using MmcBestFitAllocator = MmcBlobAllocator;

struct SlabPoolFreeStats {
    uint64_t slotSize = 0;
    size_t activeFrames = 0;     // Ile ramek należy obecnie do tej puli
    size_t allocatedSlots = 0;   // Ile slotów jest zajętych
    size_t freeSlots = 0;        // Ile slotów jest wolnych (ze stosu + jeszcze nienaruszonych)
    uint64_t freeBytes = 0;      // Łączna wolna pamięć w slotach tej klasy
    uint64_t tailWasteBytes = 0; // Niewykorzystane końcówki ramek w tej klasie (frame.size % slotSize)
};

struct SlabFreeSummary {
    uint64_t frameSize = 0;
    size_t freeFullFrames = 0;       // Liczba całkowicie pustych ramek 1.5 GiB w FrameManagerze
    uint64_t freeFullFrameBytes = 0; // Pojemność w pustych ramkach 1.5 GiB
    uint64_t freeTailFrameBytes = 0; // Pojemność w pustej ramce ogonowej (jeśli jest wolna)
    std::vector<SlabPoolFreeStats> pools;
};

class MmcSlabAllocator : public MmcBlobAllocator {
public:
    MmcSlabAllocator(const uint32_t rank, const MediaType mediaType, const uint64_t bmAddr, const uint64_t capacity,
                     std::atomic<uint64_t> *maxAlignedSize = nullptr);
    ~MmcSlabAllocator() override = default;

    bool CanAlloc(uint64_t blobSize) override;
    bool IsFragmentedFor(uint64_t alignedSize) override;
    MmcMemBlobPtr Alloc(uint64_t blobSize) override;
    Result Release(const MmcMemBlobPtr &blob) override;
    void Stop() override;
    Result BuildFromBlobs(std::vector<std::pair<std::string, MmcMemBlobDesc>> &blobList) override;

    uint64_t GetMaxContiguousFreeSize() override;
    std::vector<SpaceRange> GetFreeSegments() override;
    uint64_t GetFragmentationBytes() const override
    {
        return internalFragSize_.load(std::memory_order_relaxed);
    }
    uint64_t GetReservedSize() const
    {
        return __atomic_load_n(&allocatedSize_, __ATOMIC_RELAXED) + tailWasteSize_.load(std::memory_order_relaxed);
    }

    SlabFreeSummary GetSlabFreeStats();

private:
    // Pojedyńcza ramka pamięci
    class SlabFrame {
    public:
        uint64_t NextSlotOffset() const;
        void PrepareSlots();
        uint64_t AllocateSlot();
        void ReleaseSlot(size_t slotIndex);
        bool FindAllocatedSlot(uint64_t blockOffset, uint64_t requestedSize,
                               uint64_t allocatedSize, size_t &slotIndex) const;
        bool IsFull() const { return allocatedCount == numSlots; }
        bool IsEmpty() const { return allocatedCount == 0; }

    private:
        friend class MmcSlabAllocator;
        size_t frameId = 0;
        uint64_t offset = 0;
        uint64_t size = 0;
        int poolIndex = -1; // -1 oznacza, że ramka jest pusta i leży w FrameManagerze
        uint64_t slotSize = 0;
        size_t numSlots = 0;
        size_t nextUninitSlot = 0; // Granica ostatniego jeszcze nie ruszanego (czyli napewno wolnego slotu, gdy nie ma wolnych na stosie)
        size_t allocatedCount = 0;

        // Wskaźniki dla listy ramek
        int32_t prevPartial = -1;
        int32_t nextPartial = -1;
        bool inPartialList = false;

        std::vector<uint32_t> freeSlotStack; // Stos zwolnionych slotów (tylko te które, był zajęte i zostały zwolnione
        std::vector<uint8_t> slotAllocated;  // Leniwie rozwijana mapa zajętości (rozmiar == nextUninitSlot),
                                             // potrzebna np. żeby nie zwolnić fragmentu dwa razy, co dodałoby go dwa razy na stack
    };

    // Pojedyncza klasa rozmiaru (pula).
    class SlabPool {
    public:
        bool HasAvailableFrame() const { return activeFrameId != -1 || partialHead != -1; }
        int32_t GetFrame(std::vector<SlabFrame> &frames);
        void AddFrameFront(std::vector<SlabFrame> &frames, size_t frameId);
        void AddFrameBack(std::vector<SlabFrame> &frames, size_t frameId);
        void RemoveFrame(std::vector<SlabFrame> &frames, size_t frameId);
        // Wywołujący trzyma blokadę puli, a przy zwrocie ostatniego slotu także blokadę zarządcy.
        bool ReleaseBlock(std::vector<SlabFrame> &frames, size_t frameId, size_t slotIndex);

    private:
        friend class MmcSlabAllocator;
        std::atomic<uint64_t> slotSize{0};
        int32_t activeFrameId = -1; // Aktywnie dopychana ramka robocza
        int32_t partialHead = -1;   // Przód listy niepełnych ramek (tu trafiają ramki prawie pełne po zwolnieniu slotu)
        int32_t partialTail = -1;   // Tył listy niepełnych ramek (tu trafiają świeżo pobrane, puste ramki)
        size_t partialCount = 0;
        size_t ownedFrames = 0;     // Liczba ramek aktualnie przypisanych do tej klasy (chroniona przez spinlock_)
        uint64_t lastEmptySeq = 0;  // Znacznik kolejności opustoszenia (do wyboru najstarszej pustej klasy)

        mutable Spinlock poolLock;
    };

    int FindExactClass(uint64_t alignedSize) const;
    int GetOrCreateClassIndex(uint64_t alignedSize);
    int GetOrCreateClassIndexLocked(uint64_t alignedSize);
    void ProbeAllocation(uint64_t alignedSize, bool &outCanAlloc, bool &outFragmented) const;

    MmcMemBlobPtr CommitAllocationLocked(int classIdx, size_t frameId, uint64_t requestedSize);
    MmcMemBlobPtr AllocateFromPool(int &classIdx, uint64_t alignedSize, uint64_t requestedSize);
    void ReleaseSlotLocked(int classIdx, size_t frameId, size_t slotIndex, uint64_t requestedSize);
    bool AcquireFrameForPoolLocked(int classIdx, size_t &outFrameId);
    void ReturnEmptyFrameToManagerLocked(size_t frameId);
    Result ValidateAndAddSlabAllocation(uint64_t offset, uint64_t size, uint64_t allocSize);

private:
    uint64_t frameSize_ = SLAB_DEFAULT_FRAME_SIZE;

    std::vector<SlabFrame> frames_;
    std::vector<size_t> freeFrameStack_; // Stos wyłącznie pełnowymiarowych wolnych ramek (zdejmowanie pop_back w O(1))
    int32_t freeTailFrameId_ = -1;       // Osobny uchwyt na niepełną ramkę z końca pamięci (jeśli capacity % frameSize != 0)

    std::atomic<size_t> poolCount_{0};
    uint64_t emptyClassSeq_{0};
    SlabPool pools_[SLAB_MAX_POOLS];
    std::atomic<uint64_t> internalFragSize_{0};
    std::atomic<uint64_t> tailWasteSize_{0};
};

inline MmcRef<MmcBlobAllocator> MmcBlobAllocator::Create(const MmcLocation &loc, const MmcLocalMemlInitInfo &info,
                                                         bool enableMaxValueMode, std::atomic<uint64_t> *maxAlignedSize,
                                                         bool enableSlabMode)
{
    const char *envSlab = std::getenv("MMC_ENABLE_SLAB_ALLOCATOR");
    bool useSlab = enableSlabMode || (envSlab != nullptr && std::string(envSlab) == "1");
    if (useSlab) {
        return MmcRef<MmcBlobAllocator>(
            new (std::nothrow) MmcSlabAllocator(loc.rank_, loc.mediaType_, info.bmAddr_, info.capacity_, maxAlignedSize));
    }
    return MmcMakeRef<MmcBlobAllocator>(loc.rank_, loc.mediaType_, info.bmAddr_, info.capacity_, enableMaxValueMode,
                                        maxAlignedSize);
}

using MmcBlobAllocatorPtr = MmcRef<MmcBlobAllocator>;

} // namespace mmc
} // namespace ock

#endif // MEM_FABRIC_MMC_BLOB_ALLOCATOR_H
