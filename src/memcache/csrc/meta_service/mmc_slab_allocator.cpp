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

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>

#include "mmc_ptracer.h"

namespace ock {
namespace mmc {

// Tu zaczyna sie implementacja MmcSlabAllocator

MmcSlabAllocator::MmcSlabAllocator(const uint32_t rank, const MediaType mediaType, const uint64_t bmAddr,
                                   const uint64_t capacity, std::atomic<uint64_t> *maxAlignedSize)
    : MmcBlobAllocator(rank, mediaType, bmAddr, capacity, false, maxAlignedSize)
{
    // Usuwamy drzewa bo w Slabie zarządzamy pamięcią przez ramki.
    addressTree_.clear();
    sizeTree_.clear();
    poolCount_.store(0, std::memory_order_relaxed);

    // Dla dużych przestrzeni conajmniej 15 GiB używamy docelowej ramki 1.5 GiB.
    // Dla mniejszych dzielimy na 16 mniejszych ramek, gdzie rozmiar wyrównanujemy do 4 KiB.
    //(nie jest chyba ważne, bo kod będzie działał dla większych ramek, ale przydało się w testach)
    if (capacity_ >= 10ULL * SLAB_DEFAULT_FRAME_SIZE) {
        frameSize_ = SLAB_DEFAULT_FRAME_SIZE;
    } else if (capacity_ > 0) {
        uint64_t rawChunk = std::max<uint64_t>(capacity_ / 16ULL, SLAB_MIN_ALIGN_SIZE);
        uint64_t alignedChunk = AllocSizeAlignUp(rawChunk);
        frameSize_ = std::min(SLAB_DEFAULT_FRAME_SIZE, std::max<uint64_t>(alignedChunk, SLAB_MIN_ALIGN_SIZE));
    } else {
        frameSize_ = SLAB_MIN_ALIGN_SIZE;
    }

    // Dzielimy całą pamięć na ramki. Przechodząc po naszej całej dostępnej przestrzeni inicjalizujemy ramki
    uint64_t offset = 0;
    size_t frameId = 0;
    while (offset < capacity_) {
        uint64_t fSize = std::min(frameSize_, capacity_ - offset);
        SlabFrame frame;
        frame.frameId = frameId;
        frame.offset = offset;
        frame.size = fSize;
        frame.poolIndex = -1;
        frames_.push_back(std::move(frame));
        offset += fSize;
        ++frameId;
    }

    // Pełnowymiarowe ramki kładziemy na stosie freeFrameStack_ (od końca, żeby nr 0 był na szczycie).
    // Ostatnią nie równą ramkę zapisuje w innym miejscu, bo jest nie ważna
    freeFrameStack_.reserve(frames_.size());
    freeTailFrameId_ = -1;
    for (size_t i = frames_.size(); i > 0; --i) {
        size_t idx = i - 1;
        if (frames_[idx].size == frameSize_) {
            freeFrameStack_.push_back(idx);
        } else {
            freeTailFrameId_ = static_cast<int32_t>(idx);
        }
    }
}

void MmcSlabAllocator::Stop()
{
    // Kolejność blokad (spinlock_ -> poolLock) gwarantuje wygaszenie wszystkich trwających
    // szybkich ścieżek pul przed powrotem ze Stop(), bez narzutu na pojedynczym Alloc().
    std::lock_guard<Spinlock> managerGuard(spinlock_);
    started_ = false;
    const size_t count = poolCount_.load(std::memory_order_relaxed);
    for (size_t i = 0; i < count; ++i) {
        std::lock_guard<Spinlock> poolGuard(pools_[i].poolLock);
    }
}

int MmcSlabAllocator::FindExactClass(uint64_t alignedSize) const
{
    const size_t count = poolCount_.load(std::memory_order_acquire);
    for (size_t i = 0; i < count; ++i) {
        if (pools_[i].slotSize.load(std::memory_order_acquire) == alignedSize) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int MmcSlabAllocator::GetOrCreateClassIndexLocked(uint64_t alignedSize)
{
    int matchedIdx = FindExactClass(alignedSize);
    if (matchedIdx >= 0) {
        return matchedIdx;
    }

    // Przy pełnym regionie nie publikujemy ani nie odzyskujemy klas dla nieudanych żądań.
    // Sprawdzenie pod blokadą zarządcy synchronizuje dostęp do wolnych ramek.
    if (freeFrameStack_.empty() &&
        (freeTailFrameId_ < 0 || frames_[static_cast<size_t>(freeTailFrameId_)].size < alignedSize)) {
        return -1;
    }

    size_t count = poolCount_.load(std::memory_order_relaxed);
    if (count < SLAB_MAX_POOLS) {
        int newIdx = static_cast<int>(count);
        pools_[newIdx].activeFrameId = -1;
        pools_[newIdx].partialHead = -1;
        pools_[newIdx].partialTail = -1;
        pools_[newIdx].partialCount = 0;
        pools_[newIdx].ownedFrames = 0;
        pools_[newIdx].lastEmptySeq = ++emptyClassSeq_;
        pools_[newIdx].slotSize.store(alignedSize, std::memory_order_release);

        poolCount_.store(count + 1, std::memory_order_release);
        return newIdx;
    }

    // Limit SLAB_MAX_POOLS osiągnięty — odzyskujemy najdawniej opróżnioną klasę (ownedFrames == 0).
    int victimIdx = -1;
    uint64_t bestSeq = std::numeric_limits<uint64_t>::max();
    for (size_t i = 0; i < count; ++i) {
        if (pools_[i].ownedFrames == 0 && pools_[i].lastEmptySeq <= bestSeq) {
            bestSeq = pools_[i].lastEmptySeq;
            victimIdx = static_cast<int>(i);
        }
    }
    if (victimIdx < 0) {
        return -1;
    }

    SlabPool &victim = pools_[victimIdx];
    std::lock_guard<Spinlock> poolGuard(victim.poolLock);
    if (victim.ownedFrames != 0) {
        return -1;
    }
    victim.activeFrameId = -1;
    victim.partialHead = -1;
    victim.partialTail = -1;
    victim.partialCount = 0;
    victim.lastEmptySeq = ++emptyClassSeq_;
    victim.slotSize.store(alignedSize, std::memory_order_release);
    return victimIdx;
}

int MmcSlabAllocator::GetOrCreateClassIndex(uint64_t alignedSize)
{
    if (alignedSize == 0) {
        return -1;
    }
    if (alignedSize == UINT64_MAX || alignedSize > frameSize_ || alignedSize > capacity_) {
        return -1;
    }

    // Szybkie wyszukiwanie liniowe bez zapinania blokady.
    int matchedIdx = FindExactClass(alignedSize);
    if (matchedIdx >= 0) {
        return matchedIdx;
    }

    // Nową klasę publikujemy lub odzyskujemy dopiero po inicjalizacji pól pod blokadą zarządcy.
    std::lock_guard<Spinlock> managerGuard(spinlock_);
    return GetOrCreateClassIndexLocked(alignedSize);
}

void MmcSlabAllocator::SlabPool::AddFrameFront(std::vector<SlabFrame> &frames, size_t frameId)
{
    // Ramka, która przestała być pełna, trafia na początek listy.
    // Po zapełnieniu bieżącej aktywnej ramki GetFrame wybierze właśnie ten początek.
    SlabFrame &frame = frames[frameId];
    if (frame.inPartialList) {
        return;
    }
    int32_t fId = static_cast<int32_t>(frameId);
    frame.prevPartial = -1;
    frame.nextPartial = partialHead;
    if (partialHead != -1) {
        frames[static_cast<size_t>(partialHead)].prevPartial = fId;
    } else {
        partialTail = fId;
    }
    partialHead = fId;
    frame.inPartialList = true;
    partialCount++;
}

void MmcSlabAllocator::SlabPool::AddFrameBack(std::vector<SlabFrame> &frames, size_t frameId)
{
    // Nową ramkę dodajemy na koniec, żeby wcześniej wykorzystać wolne sloty
    // w już używanych ramkach. Lista nie jest sortowana według zajętości.
    SlabFrame &frame = frames[frameId];
    if (frame.inPartialList) {
        return;
    }
    int32_t fId = static_cast<int32_t>(frameId);
    frame.prevPartial = partialTail;
    frame.nextPartial = -1;
    if (partialTail != -1) {
        frames[static_cast<size_t>(partialTail)].nextPartial = fId;
    } else {
        partialHead = fId;
    }
    partialTail = fId;
    frame.inPartialList = true;
    partialCount++;
}

void MmcSlabAllocator::SlabPool::RemoveFrame(std::vector<SlabFrame> &frames, size_t frameId)
{
    SlabFrame &frame = frames[frameId];
    if (!frame.inPartialList) {
        return;
    }
    if (frame.prevPartial != -1) {
        frames[static_cast<size_t>(frame.prevPartial)].nextPartial = frame.nextPartial;
    } else {
        partialHead = frame.nextPartial;
    }
    if (frame.nextPartial != -1) {
        frames[static_cast<size_t>(frame.nextPartial)].prevPartial = frame.prevPartial;
    } else {
        partialTail = frame.prevPartial;
    }
    frame.prevPartial = -1;
    frame.nextPartial = -1;
    frame.inPartialList = false;
    if (partialCount > 0) {
        partialCount--;
    }
    if (activeFrameId == static_cast<int32_t>(frameId)) {
        activeFrameId = -1;
    }
}

int32_t MmcSlabAllocator::SlabPool::GetFrame(std::vector<SlabFrame> &frames)
{
    // Dopóki aktywna ramka ma wolne miejsce, alokujemy z niej.
    if (activeFrameId != -1) {
        SlabFrame &active = frames[static_cast<size_t>(activeFrameId)];
        if (active.inPartialList && active.allocatedCount < active.numSlots) {
            return activeFrameId;
        }
        activeFrameId = -1;
    }

    // Gdy aktywna ramka się zapełni, bierzemy głowę listy
    activeFrameId = partialHead;
    return activeFrameId;
}

void MmcSlabAllocator::SlabFrame::PrepareSlots()
{
    // Przygotowujemy oba bufory zanim ramka stanie się dostępna dla alokacji.
    // Jeśli druga rezerwacja rzuci wyjątek, pierwsza może zachować większe capacity,
    // ale żaden slot nie jest jeszcze zajęty i ramkę można bezpiecznie oddać.
    slotAllocated.reserve(numSlots);
    freeSlotStack.reserve(numSlots);
}

uint64_t MmcSlabAllocator::SlabFrame::NextSlotOffset() const
{
    // Pod blokadą puli podglądamy ten sam slot, który za chwilę pobierze AllocateSlot.
    // Pozwala to utworzyć deskryptor przed zmianą mapy zajętości i liczników.
    const size_t slotIndex = freeSlotStack.empty() ? nextUninitSlot : freeSlotStack.back();
    return offset + slotIndex * slotSize;
}

uint64_t MmcSlabAllocator::SlabFrame::AllocateSlot()
{
    size_t slotIdx = 0;

    // Jeśli mamy wcześniej zwolnione sloty na stosie, bierzemy ze stosu.
    // W przeciwnym razie bierzemy z indexu nieużywanego slotu
    if (!freeSlotStack.empty()) {
        slotIdx = freeSlotStack.back();
        freeSlotStack.pop_back();
        slotAllocated[slotIdx] = 1;
    } else {
        slotIdx = nextUninitSlot++;
        if (slotIdx < slotAllocated.size()) {
            slotAllocated[slotIdx] = 1;
        } else {
            slotAllocated.push_back(1);
        }
    }

    allocatedCount++;
    return offset + slotIdx * slotSize;
}

void MmcSlabAllocator::SlabFrame::ReleaseSlot(size_t slotIndex)
{
    freeSlotStack.push_back(static_cast<uint32_t>(slotIndex));
    slotAllocated[slotIndex] = 0;
    --allocatedCount;
}

bool MmcSlabAllocator::SlabFrame::FindAllocatedSlot(uint64_t blockOffset, uint64_t requestedSize,
                                                   uint64_t allocatedSize, size_t &slotIndex) const
{
    // Jedna walidacja dla obu ścieżek Release. Wywołujący najpierw sprawdza właściciela
    // ramki pod jego blokadą, dzięki czemu jej rozmiar i mapa zajętości są stabilne.
    // Odejmujemy offset zamiast dodawać końce zakresów, żeby uniknąć przepełnienia.
    if (slotSize == 0 || allocatedSize != slotSize || requestedSize > slotSize || blockOffset < offset) {
        return false;
    }
    const uint64_t relativeOffset = blockOffset - offset;
    if (relativeOffset % slotSize != 0) {
        return false;
    }
    const uint64_t index = relativeOffset / slotSize;
    if (index >= numSlots || index >= nextUninitSlot || slotAllocated[index] == 0) {
        return false;
    }
    slotIndex = static_cast<size_t>(index);
    return true;
}

MmcMemBlobPtr MmcSlabAllocator::CommitAllocationLocked(int classIdx, size_t frameId, uint64_t requestedSize)
{
    SlabFrame &frame = frames_[frameId];
    // Deskryptor może nie powstać z powodu braku pamięci albo wyjątku konstruktora.
    // W obu przypadkach slot i liczniki pozostają nietknięte. Po utworzeniu deskryptora
    // korzystamy wyłącznie z wcześniej zarezerwowanych buforów ramki.
    auto blob = MmcMakeRef<MmcMemBlob>(rank_, bmAddr_ + frame.NextSlotOffset(), requestedSize,
                                     mediaType_, ALLOCATED, frame.slotSize);
    if (blob == nullptr) {
        return nullptr;
    }
    frame.AllocateSlot();
    if (frame.IsFull()) {
        pools_[classIdx].RemoveFrame(frames_, frameId);
    }
    __atomic_fetch_add(&allocatedSize_, frame.slotSize, __ATOMIC_RELAXED);
    __atomic_fetch_add(&requestedSize_, requestedSize, __ATOMIC_RELAXED);
    if (frame.slotSize >= requestedSize) {
        internalFragSize_.fetch_add(frame.slotSize - requestedSize, std::memory_order_relaxed);
    }
    return blob;
}

bool MmcSlabAllocator::AcquireFrameForPoolLocked(int classIdx, size_t &outFrameId)
{
    uint64_t slotSize = pools_[classIdx].slotSize.load(std::memory_order_relaxed);
    size_t fId = 0;
    bool found = false;

    // Pobieramy wolną ramkę w najpierw próbujemy pełnowymiarową ramkę, jeśli nie ma to bierzemy tą małą
    if (!freeFrameStack_.empty() && frameSize_ >= slotSize) {
        fId = freeFrameStack_.back();
        freeFrameStack_.pop_back();
        found = true;
    } else if (freeTailFrameId_ != -1 && frames_[static_cast<size_t>(freeTailFrameId_)].size >= slotSize) {
        fId = static_cast<size_t>(freeTailFrameId_);
        freeTailFrameId_ = -1;
        found = true;
    }

    if (!found) {
        return false;
    }

    SlabFrame &frame = frames_[fId];
    frame.slotSize = slotSize;
    frame.numSlots = frame.size / slotSize;
    frame.nextUninitSlot = 0;
    frame.allocatedCount = 0;
    frame.prevPartial = -1;
    frame.nextPartial = -1;
    frame.inPartialList = false;

    frame.freeSlotStack.clear();
    frame.slotAllocated.clear();

    const uint64_t tailWaste = frame.size - frame.numSlots * frame.slotSize;
    if (tailWaste > 0) {
        tailWasteSize_.fetch_add(tailWaste, std::memory_order_relaxed);
    }

    // Ramka jest zarezerwowana dla tej puli, ale nie trafia jeszcze na listę dostępnych.
    // Przygotowanie buforów wykonamy bez blokady globalnej, pod blokadą właściciela.
    pools_[classIdx].ownedFrames++;
    __atomic_store_n(&frame.poolIndex, classIdx, __ATOMIC_RELEASE);
    outFrameId = fId;
    return true;
}

void MmcSlabAllocator::ReturnEmptyFrameToManagerLocked(size_t frameId)
{
    SlabFrame &frame = frames_[frameId];
    const int ownerPool = frame.poolIndex;
    if (ownerPool >= 0 && static_cast<size_t>(ownerPool) < SLAB_MAX_POOLS) {
        SlabPool &pool = pools_[static_cast<size_t>(ownerPool)];
        if (pool.ownedFrames > 0) {
            pool.ownedFrames--;
            if (pool.ownedFrames == 0) {
                pool.lastEmptySeq = ++emptyClassSeq_;
            }
        }
    }
    if (frame.poolIndex >= 0 && frame.slotSize > 0) {
        const uint64_t tailWaste = frame.size - frame.numSlots * frame.slotSize;
        if (tailWaste > 0) {
            tailWasteSize_.fetch_sub(tailWaste, std::memory_order_relaxed);
        }
    }
    __atomic_store_n(&frame.poolIndex, -1, __ATOMIC_RELEASE);
    frame.slotSize = 0;
    frame.numSlots = 0;
    frame.nextUninitSlot = 0;
    frame.prevPartial = -1;
    frame.nextPartial = -1;
    frame.inPartialList = false;

    // Zostawiamy wyczyszczone wektory z zachowanym capacity(), żeby przy kolejnym użyciu
    // tej ramki nie alokować sterty od nowa.
    frame.freeSlotStack.clear();
    frame.slotAllocated.clear();

    // Zwrot ramki do FrameManagera
    if (frame.size == frameSize_) {
        freeFrameStack_.push_back(frameId);
    } else {
        freeTailFrameId_ = static_cast<int32_t>(frameId);
    }
}

void MmcSlabAllocator::ProbeAllocation(uint64_t alignedSize, bool &outCanAlloc, bool &outFragmented) const
{
    outCanAlloc = false;
    outFragmented = false;
    if (!started_ || alignedSize == 0 || alignedSize == UINT64_MAX ||
        alignedSize > frameSize_ || alignedSize > capacity_) {
        return;
    }
    int classIdx = FindExactClass(alignedSize);
    if (classIdx >= 0) {
        // Istniejąca pula działa niezależnie od zarządcy ramek i innych pul.
        // Wynik pozostaje wskazówką: inny wątek może zająć slot przed Alloc().
        std::lock_guard<Spinlock> poolGuard(pools_[classIdx].poolLock);
        if (!started_) {
            return;
        }
        if (pools_[classIdx].slotSize.load(std::memory_order_relaxed) == alignedSize &&
            pools_[classIdx].HasAvailableFrame()) {
            outCanAlloc = true;
            return;
        }
    }
    // Nie trzymamy blokady puli podczas oczekiwania na globalną. W wolnej ścieżce
    // powtarzamy wyszukiwanie i kontrolę puli, bo inny wątek mógł już dostarczyć ramkę.
    std::lock_guard<Spinlock> managerGuard(spinlock_);
    if (!started_) {
        return;
    }
    classIdx = FindExactClass(alignedSize);
    if (classIdx >= 0) {
        std::lock_guard<Spinlock> poolGuard(pools_[classIdx].poolLock);
        if (pools_[classIdx].HasAvailableFrame()) {
            outCanAlloc = true;
            return;
        }
    } else if (poolCount_.load(std::memory_order_relaxed) >= SLAB_MAX_POOLS) {
        const size_t count = poolCount_.load(std::memory_order_relaxed);
        bool hasEmptyClass = false;
        for (size_t i = 0; i < count; ++i) {
            if (pools_[i].ownedFrames == 0) {
                hasEmptyClass = true;
                break;
            }
        }
        if (!hasEmptyClass) {
            outFragmented = true;
            return;
        }
    }
    const bool hasFreeFrame = !freeFrameStack_.empty() ||
        (freeTailFrameId_ >= 0 && frames_[static_cast<size_t>(freeTailFrameId_)].size >= alignedSize);
    outCanAlloc = hasFreeFrame;
    outFragmented = !hasFreeFrame;
}

bool MmcSlabAllocator::CanAlloc(uint64_t blobSize)
{
    if (blobSize == 0) {
        return false;
    }
    bool canAlloc = false;
    bool fragmented = false;
    ProbeAllocation(AllocSizeAlignUp(blobSize), canAlloc, fragmented);
    return canAlloc;
}

bool MmcSlabAllocator::IsFragmentedFor(uint64_t alignedSize)
{
    bool canAlloc = false;
    bool fragmented = false;
    ProbeAllocation(alignedSize, canAlloc, fragmented);
    return fragmented;
}

MmcMemBlobPtr MmcSlabAllocator::Alloc(uint64_t blobSize)
{
    if (!started_) {
        MMC_LOG_WARN("Allocator rank: " << rank_ << " mediaType: " << mediaType_ << " is stopped");
        return nullptr;
    }
    const uint64_t alignedSize = AllocSizeAlignUp(blobSize);
    if (blobSize == 0 || alignedSize == UINT64_MAX || alignedSize > frameSize_ || alignedSize > capacity_) {
        MMC_LOG_WARN("Allocator rank: " << rank_ << " blobSize: " << blobSize << " exceeds max slab slot size");
        return nullptr;
    }
    int classIdx = GetOrCreateClassIndex(alignedSize);
    if (classIdx < 0) {
        MMC_LOG_WARN("Allocator rank: " << rank_ << " cannot acquire slab class for alignedSize: " << alignedSize
                                        << " (no free frame or class limit reached)");
        return nullptr;
    }
    MmcMemBlobPtr blob;
    try {
        blob = AllocateFromPool(classIdx, alignedSize, blobSize);
    } catch (const std::bad_alloc &) {
        return nullptr;
    }
    if (blob == nullptr) {
        return nullptr;
    }
    const uint64_t allocSize = blob->AllocSize();

    if (maxAlignedSize_ != nullptr) {
        uint64_t currentMax = maxAlignedSize_->load(std::memory_order_relaxed);
        while (alignedSize > currentMax &&
               !maxAlignedSize_->compare_exchange_weak(currentMax, alignedSize, std::memory_order_relaxed)) {}
    }

    // Przydział jest już zatwierdzony. Brak pamięci przy formatowaniu diagnostyki
    // nie może zgubić poprawnego bloku ani zamienić udanej alokacji w wyciek slotu.
    try {
        uint64_t internalFragBytes = allocSize - blobSize;
        if (internalFragBytes > 0) {
            TP_TRACE_RECORD(TP_MMC_ALLOC_FRAG, internalFragBytes, 0);
        }

        MMC_LOG_DEBUG("SlabAlloc rank=" << rank_ << ", type=" << mediaType_ << " requested=" << blobSize
                                        << " slotSize=" << allocSize << " total="
                                        << __atomic_load_n(&allocatedSize_, __ATOMIC_RELAXED) << "/" << capacity_);
    } catch (const std::bad_alloc &) {
        // Pomijamy diagnostykę, ale przekazujemy właścicielowi istniejący deskryptor.
    }
    return blob;
}

std::vector<MmcMemBlobPtr> MmcSlabAllocator::AllocBatch(const std::vector<uint64_t> &sizes)
{
    std::vector<MmcMemBlobPtr> result(sizes.size());
    if (!started_) { return result; }
    // Grupujemy indeksy, nie wyniki. Dzięki temu odbiorca zachowuje kolejność kluczy,
    // nawet gdy w jednym żądaniu występują różne rozmiary slotów.
    std::map<uint64_t, std::vector<size_t>> groups;
    for (size_t i = 0; i < sizes.size(); ++i) {
        const uint64_t aligned = AllocSizeAlignUp(sizes[i]);
        if (sizes[i] != 0 && aligned != UINT64_MAX && aligned <= frameSize_ && aligned <= capacity_) {
            groups[aligned].push_back(i);
        }
    }
    try {
        for (const auto &group : groups) {
            int classIdx = GetOrCreateClassIndex(group.first);
            if (classIdx < 0) { continue; }
            std::unique_lock<Spinlock> guard(pools_[classIdx].poolLock);
            for (size_t index : group.second) {
                if (!started_) { break; }
                try {
                    SlabPool &pool = pools_[classIdx];
                    const int32_t frameId =
                        (pool.slotSize.load(std::memory_order_relaxed) == group.first) ? pool.GetFrame(frames_) : -1;
                    if (frameId >= 0) {
                        result[index] = CommitAllocationLocked(classIdx, static_cast<size_t>(frameId), sizes[index]);
                    } else {
                        // Nowa ramka lub podmiana klasy wymaga kolejności zarządca -> pula. Zwalniamy
                        // lokalną blokadę, a wspólna wolna ścieżka ponownie sprawdza stan.
                        guard.unlock();
                        result[index] = AllocateFromPool(classIdx, group.first, sizes[index]);
                        guard = std::unique_lock<Spinlock>(pools_[classIdx].poolLock);
                    }
                } catch (const std::bad_alloc &) {
                    if (!guard.owns_lock()) { guard = std::unique_lock<Spinlock>(pools_[classIdx].poolLock); }
                    // Brak deskryptora dotyczy tego elementu; pozostałe można wykonać.
                }
                if (result[index] != nullptr) {
                    if (maxAlignedSize_ != nullptr) {
                        uint64_t current = maxAlignedSize_->load(std::memory_order_relaxed);
                        while (current < group.first && !maxAlignedSize_->compare_exchange_weak(
                            current, group.first, std::memory_order_relaxed)) {}
                    }
                    try {
                        const uint64_t internalFragBytes = result[index]->AllocSize() - sizes[index];
                        if (internalFragBytes > 0) {
                            TP_TRACE_RECORD(TP_MMC_ALLOC_FRAG, internalFragBytes, 0);
                        }
                    } catch (const std::bad_alloc &) {
                        // Pomijamy diagnostykę, ale przekazujemy właścicielowi istniejący deskryptor.
                    }
                }
            }
        }
    } catch (...) {
        for (const auto &blob : result) { if (blob != nullptr) { Release(blob); } }
        throw;
    }
    return result;
}

MmcMemBlobPtr MmcSlabAllocator::AllocateFromPool(int &classIdx, uint64_t alignedSize, uint64_t requestedSize)
{
    if (classIdx >= 0 && static_cast<size_t>(classIdx) < poolCount_.load(std::memory_order_acquire)) {
        SlabPool &fastPool = pools_[classIdx];
        std::lock_guard<Spinlock> poolGuard(fastPool.poolLock);
        if (!started_) {
            return nullptr;
        }
        if (fastPool.slotSize.load(std::memory_order_relaxed) == alignedSize) {
            const int32_t frameId = fastPool.GetFrame(frames_);
            if (frameId >= 0) {
                return CommitAllocationLocked(classIdx, static_cast<size_t>(frameId), requestedSize);
            }
        }
    }

    std::unique_lock<Spinlock> managerGuard(spinlock_);
    if (!started_) {
        return nullptr;
    }
    if (classIdx < 0 || static_cast<size_t>(classIdx) >= poolCount_.load(std::memory_order_relaxed) ||
        pools_[classIdx].slotSize.load(std::memory_order_relaxed) != alignedSize) {
        const int resolvedIdx = GetOrCreateClassIndexLocked(alignedSize);
        if (resolvedIdx < 0) {
            return nullptr;
        }
        classIdx = resolvedIdx;
    }
    SlabPool &pool = pools_[classIdx];
    std::unique_lock<Spinlock> poolGuard(pool.poolLock);
    const int32_t activeId = pool.GetFrame(frames_);
    if (activeId >= 0) {
        managerGuard.unlock();
        return CommitAllocationLocked(classIdx, static_cast<size_t>(activeId), requestedSize);
    }
    size_t frameId = 0;
    if (!AcquireFrameForPoolLocked(classIdx, frameId)) {
        return nullptr;
    }
    managerGuard.unlock();

    // Przygotowanie pamięci i deskryptora nie blokuje innych pul. Zarezerwowana ramka
    // nie znajduje się jeszcze na liście puli, więc inne alokacje nie mogą jej wybrać.
    // Po błędzie oddajemy ją z zachowaniem kolejności: zarządca, następnie pula.
    auto returnUnusedFrame = [&]() {
        poolGuard.unlock();
        managerGuard.lock();
        poolGuard.lock();
        ReturnEmptyFrameToManagerLocked(frameId);
    };
    MmcMemBlobPtr blob;
    try {
        frames_[frameId].PrepareSlots();
        blob = CommitAllocationLocked(classIdx, frameId, requestedSize);
    } catch (...) {
        returnUnusedFrame();
        throw;
    }
    if (blob == nullptr) {
        returnUnusedFrame();
        return nullptr;
    }
    if (!frames_[frameId].IsFull()) {
        pool.AddFrameBack(frames_, frameId);
        if (pool.activeFrameId == -1) {
            pool.activeFrameId = static_cast<int32_t>(frameId);
        }
    }
    return blob;
}

bool MmcSlabAllocator::SlabPool::ReleaseBlock(std::vector<SlabFrame> &frames,
                                            size_t frameId, size_t slotIndex)
{
    SlabFrame &frame = frames[frameId];
    const bool wasFull = frame.IsFull();
    frame.ReleaseSlot(slotIndex);
    if (frame.IsEmpty()) {
        if (!wasFull) {
            RemoveFrame(frames, frameId);
        }
        return true;
    }
    if (wasFull) {
        AddFrameFront(frames, frameId);
        if (activeFrameId == -1) {
            activeFrameId = static_cast<int32_t>(frameId);
        }
    }
    return false;
}

void MmcSlabAllocator::ReleaseSlotLocked(int classIdx, size_t frameId, size_t slotIndex,
                                        uint64_t requestedSize)
{
    // Pula zmienia wyłącznie własne listy i zajętość slotu. Zarządca odbiera pustą ramkę
    // i aktualizuje globalne rozliczenie w jednym miejscu dla obu ścieżek zwalniania.
    // Dla ostatniego slotu wywołujący musi trzymać także blokadę globalną.
    const uint64_t releasedSize = frames_[frameId].slotSize;
    if (pools_[classIdx].ReleaseBlock(frames_, frameId, slotIndex)) {
        ReturnEmptyFrameToManagerLocked(frameId);
    }
    __atomic_fetch_sub(&allocatedSize_, releasedSize, __ATOMIC_RELAXED);
    __atomic_fetch_sub(&requestedSize_, requestedSize, __ATOMIC_RELAXED);
    if (releasedSize >= requestedSize) {
        internalFragSize_.fetch_sub(releasedSize - requestedSize, std::memory_order_relaxed);
    }
}

Result MmcSlabAllocator::Release(const MmcMemBlobPtr &blob)
{
    if (blob == nullptr || frames_.empty()) {
        return MMC_ERROR;
    }
    const uint64_t allocSize = blob->AllocSize();
    if (__atomic_load_n(&allocatedSize_, __ATOMIC_RELAXED) < allocSize ||
        __atomic_load_n(&requestedSize_, __ATOMIC_RELAXED) < blob->Size()) {
        return MMC_ERROR;
    }
    const uint64_t blobAddr = blob->Gva();
    if (allocSize == 0 || blobAddr < bmAddr_) {
        return MMC_ERROR;
    }
    const uint64_t offset = blobAddr - bmAddr_;
    if (offset >= capacity_ || allocSize > capacity_ - offset) {
        return MMC_ERROR;
    }
    const size_t frameId = offset / frameSize_;
    SlabFrame &frame = frames_[frameId];
    size_t slotIndex = 0;

    // Jeśli ramka zachowa inne bloki, zwrot slotu wymaga tylko blokady puli.
    // Właściciela sprawdzamy ponownie po jej zdobyciu: sama wcześniejsza obserwacja
    // poolIndex nie daje prawa do odczytania pozostałych pól ramki.
    const int fastPoolIdx = __atomic_load_n(&frame.poolIndex, __ATOMIC_ACQUIRE);
    if (fastPoolIdx >= 0 && static_cast<size_t>(fastPoolIdx) < poolCount_.load(std::memory_order_acquire)) {
        std::lock_guard<Spinlock> poolGuard(pools_[fastPoolIdx].poolLock);
        if (__atomic_load_n(&frame.poolIndex, __ATOMIC_ACQUIRE) == fastPoolIdx && frame.allocatedCount > 1 &&
            frame.FindAllocatedSlot(offset, blob->Size(), allocSize, slotIndex)) {
            ReleaseSlotLocked(fastPoolIdx, frameId, slotIndex, blob->Size());
            return MMC_OK;
        }
    }

    // Zwrot ostatniego slotu może oddać ramkę zarządcy. Poprzednia blokada puli
    // została już zwolniona; teraz zachowujemy kolejność globalna -> pula.
    // Ponawiamy walidację, bo pomiędzy sekcjami stan ramki mógł się zmienić.
    std::lock_guard<Spinlock> managerGuard(spinlock_);
    const int poolIdx = frame.poolIndex;
    if (poolIdx < 0 || static_cast<size_t>(poolIdx) >= poolCount_.load(std::memory_order_relaxed)) {
        return MMC_ERROR;
    }
    std::lock_guard<Spinlock> poolGuard(pools_[poolIdx].poolLock);
    if (!frame.FindAllocatedSlot(offset, blob->Size(), allocSize, slotIndex)) {
        return MMC_ERROR;
    }
    ReleaseSlotLocked(poolIdx, frameId, slotIndex, blob->Size());
    return MMC_OK;
}

uint64_t MmcSlabAllocator::GetMaxContiguousFreeSize()
{
    uint64_t maxFreeSize = 0;

    // Stała tablica strażników nie wymaga alokacji pamięci. Zarządca jest
    // blokowany przed pulami, a destruktory zwalniają blokady również po wyjątku.
    std::lock_guard<Spinlock> managerGuard(spinlock_);
    std::array<std::unique_lock<Spinlock>, SLAB_MAX_POOLS> poolGuards;
    size_t count = poolCount_.load(std::memory_order_relaxed);
    for (size_t i = 0; i < count; ++i) {
        poolGuards[i] = std::unique_lock<Spinlock>(pools_[i].poolLock);
    }

    // Zwracamy wyłącznie faktycznie używalne wolne ramki oraz wolne sloty.
    for (const auto &frame : frames_) {
        if (frame.poolIndex < 0) {
            if (frame.size >= SLAB_MIN_ALIGN_SIZE) {
                maxFreeSize = std::max(maxFreeSize, frame.size);
            }
        } else if (frame.allocatedCount < frame.numSlots) {
            maxFreeSize = std::max(maxFreeSize, frame.slotSize);
        }
    }


    return maxFreeSize;
}

std::vector<SpaceRange> MmcSlabAllocator::GetFreeSegments()
{
    // Alokator slabowy nie korzysta z drzewa adresowego ani z defragmentacji okien GVA (DefragEvict).
    // Zwrócenie pustego wektora sprawia, że DetectDefragLocs / DefragEvictForRank pomijają ranki slabowe.
    // Szczegółowe statystyki ramek i slotów są dostępne przez GetSlabFreeStats().
    return {};
}

Result MmcSlabAllocator::ValidateAndAddSlabAllocation(uint64_t offset, uint64_t size, uint64_t allocSize)
{
    // 1. Pełna walidacja przed jakąkolwiek modyfikacją stanu ramki lub puli
    if (frames_.empty() || size == 0 || offset > std::numeric_limits<uint64_t>::max() - size || offset >= capacity_ ||
        (offset + size) > capacity_) {
        return MMC_ERROR;
    }

    // Bierzemy większą z wartości (allocSize, size), aby uniknąć przydzielenia slotu mniejszego niż rozmiar logiczny
    uint64_t effectiveSize = std::max(allocSize, size);
    uint64_t slotSize = AllocSizeAlignUp(effectiveSize);
    if (slotSize == UINT64_MAX || slotSize == 0 || slotSize > frameSize_ || slotSize > capacity_) {
        return MMC_ERROR;
    }
    if (offset > std::numeric_limits<uint64_t>::max() - slotSize || (offset + slotSize) > capacity_) {
        return MMC_ERROR;
    }

    size_t frameId = offset / frameSize_;
    if (frameId >= frames_.size()) {
        frameId = frames_.size() - 1;
    }
    SlabFrame &frame = frames_[frameId];

    if (offset < frame.offset || (offset + slotSize) > (frame.offset + frame.size)) {
        return MMC_ERROR;
    }

    if (frame.poolIndex >= 0 && allocSize == 0 && frame.slotSize >= slotSize) {
        slotSize = frame.slotSize;
    }

    if (frame.poolIndex >= 0 && frame.slotSize != slotSize) {
        return MMC_ERROR;
    }

    size_t numSlots = frame.size / slotSize;
    if (numSlots == 0) {
        return MMC_ERROR;
    }

    uint64_t offsetInFrame = offset - frame.offset;
    if (offsetInFrame % slotSize != 0) {
        return MMC_ERROR;
    }

    size_t slotIdx = offsetInFrame / slotSize;
    if (slotIdx >= numSlots) {
        return MMC_ERROR;
    }
    if (frame.poolIndex >= 0 && slotIdx < frame.nextUninitSlot && frame.slotAllocated[slotIdx] != 0) {
        return MMC_ERROR;
    }

    int classIdx = -1;
    size_t count = poolCount_.load(std::memory_order_relaxed);
    for (size_t i = 0; i < count; ++i) {
        if (pools_[i].slotSize.load(std::memory_order_relaxed) == slotSize) {
            classIdx = static_cast<int>(i);
            break;
        }
    }
    if (classIdx < 0 && count >= SLAB_MAX_POOLS) {
        return MMC_ERROR;
    }

    auto freeIt = freeFrameStack_.end();
    if (frame.poolIndex < 0 && freeTailFrameId_ != static_cast<int32_t>(frameId)) {
        freeIt = std::find(freeFrameStack_.begin(), freeFrameStack_.end(), frameId);
        if (freeIt == freeFrameStack_.end()) {
            return MMC_ERROR;
        }
    }

    // 2. Rezerwacja buforów ramki przed modyfikacją stanu puli i zarządcy, aby bad_alloc nie wyciekał ramki
    if (frame.poolIndex < 0) {
        frame.slotAllocated.clear();
        frame.freeSlotStack.clear();
        try {
            if (frame.slotAllocated.capacity() < numSlots) {
                frame.slotAllocated.reserve(numSlots);
            }
            if (frame.freeSlotStack.capacity() < numSlots) {
                frame.freeSlotStack.reserve(numSlots);
            }
        } catch (const std::bad_alloc &) {
            return MMC_MALLOC_FAILED;
        }
    }

    // 3. Wszystkie kontrole i rezerwacje pamięci przeszły - bezpiecznie modyfikujemy stan puli i ramki
    if (classIdx < 0) {
        classIdx = static_cast<int>(count);
        pools_[classIdx].activeFrameId = -1;
        pools_[classIdx].partialHead = -1;
        pools_[classIdx].partialTail = -1;
        pools_[classIdx].partialCount = 0;
        pools_[classIdx].ownedFrames = 0;
        pools_[classIdx].lastEmptySeq = ++emptyClassSeq_;
        pools_[classIdx].slotSize.store(slotSize, std::memory_order_release);
        poolCount_.store(count + 1, std::memory_order_release);
    }

    if (frame.poolIndex < 0) {
        if (freeTailFrameId_ == static_cast<int32_t>(frameId)) {
            freeTailFrameId_ = -1;
        } else {
            freeFrameStack_.erase(freeIt);
        }
        pools_[classIdx].ownedFrames++;
        frame.poolIndex = classIdx;
        frame.slotSize = slotSize;
        frame.numSlots = numSlots;
        frame.nextUninitSlot = 0;
        frame.allocatedCount = 0;
        frame.prevPartial = -1;
        frame.nextPartial = -1;
        frame.inPartialList = false;
        const uint64_t tailWaste = frame.size - numSlots * slotSize;
        if (tailWaste > 0) {
            tailWasteSize_.fetch_add(tailWaste, std::memory_order_relaxed);
        }
    }

    if (slotIdx >= frame.nextUninitSlot) {
        for (size_t s = frame.nextUninitSlot; s < slotIdx; ++s) {
            frame.freeSlotStack.push_back(static_cast<uint32_t>(s));
        }
        frame.slotAllocated.resize(slotIdx + 1, 0);
        frame.nextUninitSlot = slotIdx + 1;
    } else {
        auto stackIt = std::find(frame.freeSlotStack.begin(), frame.freeSlotStack.end(), static_cast<uint32_t>(slotIdx));
        if (stackIt != frame.freeSlotStack.end()) {
            *stackIt = frame.freeSlotStack.back();
            frame.freeSlotStack.pop_back();
        }
    }

    frame.slotAllocated[slotIdx] = 1;
    frame.allocatedCount++;

    if (frame.allocatedCount < frame.numSlots) {
        pools_[classIdx].AddFrameFront(frames_, frameId);
        if (pools_[classIdx].activeFrameId == -1) {
            pools_[classIdx].activeFrameId = static_cast<int32_t>(frameId);
        }
    } else {
        pools_[classIdx].RemoveFrame(frames_, frameId);
    }

    __atomic_fetch_add(&allocatedSize_, frame.slotSize, __ATOMIC_RELAXED);
    return MMC_OK;
}

Result MmcSlabAllocator::BuildFromBlobs(std::vector<std::pair<std::string, MmcMemBlobDesc>> &blobList)
{
    std::lock_guard<Spinlock> managerGuard(spinlock_);
    if (started_) {
        return MMC_ERROR;
    }

    for (auto it = blobList.begin(); it != blobList.end();) {
        if (it->second.rank_ != rank_) {
            it = blobList.erase(it);
            continue;
        }
        if (it->second.mediaType_ != mediaType_) {
            ++it;
            continue;
        }
        if (it->second.gva_ < bmAddr_) {
            it = blobList.erase(it);
            continue;
        }
        uint64_t offset = it->second.gva_ - bmAddr_;
        uint64_t size = it->second.size_;
        Result res = ValidateAndAddSlabAllocation(offset, size, it->second.allocSize_);
        if (res == MMC_MALLOC_FAILED) {
            // Wycofujemy bloki zaakceptowane w tym wywołaniu bez dodatkowych alokacji sterty,
            // aby nieudane BuildFromBlobs zostawiło alokator w spójnym stanie.
            for (auto rollbackIt = blobList.begin(); rollbackIt != it; ++rollbackIt) {
                if (rollbackIt->second.rank_ != rank_ || rollbackIt->second.mediaType_ != mediaType_) {
                    continue;
                }
                const uint64_t rbOffset = rollbackIt->second.gva_ - bmAddr_;
                size_t rbFrameId = rbOffset / frameSize_;
                if (rbFrameId >= frames_.size()) {
                    rbFrameId = frames_.size() - 1;
                }
                SlabFrame &rbFrame = frames_[rbFrameId];
                const int rbPoolIdx = rbFrame.poolIndex;
                if (rbPoolIdx >= 0 && rbFrame.slotSize > 0 && rbOffset >= rbFrame.offset) {
                    const size_t rbSlotIdx = (rbOffset - rbFrame.offset) / rbFrame.slotSize;
                    std::lock_guard<Spinlock> poolGuard(pools_[rbPoolIdx].poolLock);
                    ReleaseSlotLocked(rbPoolIdx, rbFrameId, rbSlotIdx, rollbackIt->second.size_);
                }
            }
            return MMC_MALLOC_FAILED;
        }
        if (res != MMC_OK) {
            it = blobList.erase(it);
            continue;
        }
        __atomic_fetch_add(&requestedSize_, size, __ATOMIC_RELAXED);
        size_t frameId = std::min(static_cast<size_t>(offset / frameSize_), frames_.size() - 1);
        uint64_t recorded = frames_[frameId].slotSize;
        it->second.allocSize_ = recorded;
        if (recorded >= size) {
            internalFragSize_.fetch_add(recorded - size, std::memory_order_relaxed);
        }
        if (maxAlignedSize_ != nullptr) {
            uint64_t currentMax = maxAlignedSize_->load(std::memory_order_relaxed);
            while (recorded > currentMax &&
                   !maxAlignedSize_->compare_exchange_weak(currentMax, recorded, std::memory_order_relaxed)) {}
        }
        ++it;
    }
    return MMC_OK;
}

SlabFreeSummary MmcSlabAllocator::GetSlabFreeStats()
{
    SlabFreeSummary summary;
    summary.frameSize = frameSize_;

    // Stała tablica strażników nie wymaga alokacji pamięci. Zarządca jest
    // blokowany przed pulami, a destruktory zwalniają blokady również po wyjątku.
    std::lock_guard<Spinlock> managerGuard(spinlock_);
    std::array<std::unique_lock<Spinlock>, SLAB_MAX_POOLS> poolGuards;
    size_t count = poolCount_.load(std::memory_order_relaxed);
    for (size_t i = 0; i < count; ++i) {
        poolGuards[i] = std::unique_lock<Spinlock>(pools_[i].poolLock);
    }

    summary.freeFullFrames = freeFrameStack_.size();
    summary.freeFullFrameBytes = freeFrameStack_.size() * frameSize_;
    if (freeTailFrameId_ != -1) {
        summary.freeTailFrameBytes = frames_[static_cast<size_t>(freeTailFrameId_)].size;
    }

    summary.pools.resize(count);
    for (size_t i = 0; i < count; ++i) {
        summary.pools[i].slotSize = pools_[i].slotSize.load(std::memory_order_relaxed);
    }

    for (const auto &frame : frames_) {
        if (frame.poolIndex >= 0 && static_cast<size_t>(frame.poolIndex) < count) {
            auto &ps = summary.pools[static_cast<size_t>(frame.poolIndex)];
            size_t freeInFrame = frame.numSlots - frame.allocatedCount;
            ps.activeFrames++;
            ps.allocatedSlots += frame.allocatedCount;
            ps.freeSlots += freeInFrame;
            ps.freeBytes += freeInFrame * frame.slotSize;
            ps.tailWasteBytes += (frame.size - frame.numSlots * frame.slotSize);
        }
    }

    // Sortujemy klasy rosnąco po rozmiarze slotu dla czytelnego wypisywania
    std::sort(summary.pools.begin(), summary.pools.end(),
              [](const SlabPoolFreeStats &a, const SlabPoolFreeStats &b) { return a.slotSize < b.slotSize; });


    return summary;
}

} // namespace mmc
} // namespace ock
