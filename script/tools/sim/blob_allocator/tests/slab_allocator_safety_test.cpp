/* Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 * Licensed under Mulan PSL v2. See the repository LICENSE file.
 */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <new>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "mmc_blob_allocator.h"
#include "mmc_global_allocator.h"
#include "ptracer.h"

extern "C" { ptracer g_mmc_tracer{}; }
namespace ock {
namespace mmc {
MmcMetaMetricManager::MmcMetaMetricManager() = default;
bool MmcMetaMetricManager::IsPerRankEnabled() { return false; }
} // namespace mmc
} // namespace ock
namespace {
thread_local int failAfter = -1;
constexpr uint64_t kCapacity = 1024 * 1024;
constexpr uint64_t kSlot = 4096;
constexpr int kWorkers = 4;
constexpr int kIterations = 2000;
constexpr int kSharedClasses = 2;
constexpr auto kDeadline = std::chrono::seconds(5);
void Require(bool condition, const char *message = "slab safety check failed")
{
    if (!condition) { throw std::runtime_error(message); }
}

class SlabEnvGuard {
public:
    explicit SlabEnvGuard(const char *val)
    {
        const char *prev = std::getenv("MMC_ENABLE_SLAB_ALLOCATOR");
        if (prev != nullptr) {
            hadPrev_ = true;
            prev_ = prev;
        }
        setenv("MMC_ENABLE_SLAB_ALLOCATOR", val, 1);
    }
    ~SlabEnvGuard()
    {
        if (hadPrev_) {
            setenv("MMC_ENABLE_SLAB_ALLOCATOR", prev_.c_str(), 1);
        } else {
            unsetenv("MMC_ENABLE_SLAB_ALLOCATOR");
        }
    }

private:
    bool hadPrev_ = false;
    std::string prev_;
};
}
// Błąd dotyczy jednego przydziału w bieżącym wątku. Nie zmieniamy alokatora
// produkcyjnego ani zachowania pozostałych wątków, które badają niezależność pul.
void *operator new(std::size_t size)
{
    if (failAfter >= 0 && failAfter-- == 0) { throw std::bad_alloc(); }
    if (void *ptr = std::malloc(size == 0 ? 1 : size)) { return ptr; }
    throw std::bad_alloc();
}
void operator delete(void *ptr) noexcept { std::free(ptr); }
void operator delete(void *ptr, std::size_t) noexcept { std::free(ptr); }
void *operator new(std::size_t size, const std::nothrow_t &) noexcept
{
    try { return ::operator new(size); } catch (...) { return nullptr; }
}
void operator delete(void *ptr, const std::nothrow_t &) noexcept { std::free(ptr); }

using namespace ock::mmc;
class TestAllocator : public MmcSlabAllocator {
public:
    explicit TestAllocator(uint64_t cap = kCapacity, uint64_t bmAddr = 0)
        : MmcSlabAllocator(0, MEDIA_DRAM, bmAddr, cap)
    {
        Start();
    }
    Spinlock &ManagerLock() { return spinlock_; }
};

void CheckFailures()
{
    // Nowa ramka: kolejno rezerwacja mapy zajętości, stosu i deskryptor.
    constexpr int kAllocationPoints = 3;
    for (int point = 0; point < kAllocationPoints; ++point) {
        TestAllocator allocator;
        const auto before = allocator.GetSlabFreeStats();
        failAfter = point;
        auto failed = allocator.Alloc(kSlot);
        failAfter = -1;
        Require(failed == nullptr);
        Require(allocator.GetUsageInfo().second == 0);
        Require(allocator.GetFragmentationBytes() == 0);
        Require(allocator.GetSlabFreeStats().freeFullFrames == before.freeFullFrames);
        auto retry = allocator.Alloc(kSlot);
        Require(retry != nullptr);
        Require(allocator.Release(retry) == MMC_OK);
        Require(allocator.GetSlabFreeStats().freeFullFrames == before.freeFullFrames);
    }
    TestAllocator allocator;
    auto first = allocator.Alloc(kSlot);
    const auto before = allocator.GetUsageInfo();
    failAfter = 0;
    auto failed = allocator.Alloc(kSlot);
    failAfter = -1;
    Require(failed == nullptr && allocator.GetUsageInfo() == before);
    auto second = allocator.Alloc(kSlot);
    Require(second != nullptr && second->Gva() == first->Gva() + kSlot);
    Require(allocator.Release(first) == MMC_OK);
    Require(allocator.Release(second) == MMC_OK);
}

void CheckInvalidRelease()
{
    TestAllocator allocator;
    auto first = allocator.Alloc(kSlot);
    auto second = allocator.Alloc(kSlot);
    const auto before = allocator.GetUsageInfo();
    // Niepoprawny offset, adres poza regionem i przepełniający się koniec zakresu
    // muszą zostać odrzucone bez zmiany zajętości slotów ani liczników.
    for (uint64_t address : {first->Gva() + 1, kCapacity, UINT64_MAX - kSlot + 1}) {
        auto invalid = MmcMakeRef<MmcMemBlob>(0, address, kSlot, MEDIA_DRAM, ALLOCATED, kSlot);
        Require(allocator.Release(invalid) == MMC_ERROR);
        Require(allocator.GetUsageInfo() == before);
    }
    Require(allocator.Release(first) == MMC_OK);
    Require(allocator.Release(first) == MMC_ERROR);
    Require(allocator.Release(second) == MMC_OK);
    Require(allocator.GetUsageInfo().second == 0);
}

void CheckIndependentFastPaths()
{
    TestAllocator allocator;
    auto first = allocator.Alloc(kSlot);
    std::unique_lock<Spinlock> manager(allocator.ManagerLock());
    auto worker = std::async(std::launch::async, [&]() {
        if (!allocator.CanAlloc(kSlot)) { return false; }
        auto second = allocator.Alloc(kSlot);
        return second != nullptr && allocator.Release(second) == MMC_OK;
    });
    const bool completed = worker.wait_for(kDeadline) == std::future_status::ready;
    manager.unlock();
    const bool result = worker.get();
    Require(completed && result);
    Require(allocator.Release(first) == MMC_OK);
}

void CheckConcurrentPools()
{
    TestAllocator allocator;
    std::vector<std::future<void>> workers;
    for (int index = 0; index < kWorkers; ++index) {
        workers.push_back(std::async(std::launch::async, [&, index]() {
            const uint64_t size = (index % kSharedClasses + 1) * kSlot;
            for (int iteration = 0; iteration < kIterations; ++iteration) {
                auto first = allocator.Alloc(size);
                auto second = allocator.Alloc(size);
                auto third = allocator.Alloc(size);
                Require(first != nullptr && second != nullptr && third != nullptr);
                Require(first->Gva() != second->Gva() && first->Gva() != third->Gva() && second->Gva() != third->Gva());
                Require(allocator.CanAlloc(size));
                Require(allocator.Release(first) == MMC_OK);
                Require(allocator.Release(second) == MMC_OK);
                Require(allocator.Release(third) == MMC_OK);
            }
        }));
    }
    for (auto &worker : workers) { worker.get(); }
    Require(allocator.GetUsageInfo().second == 0);
    Require(allocator.GetFragmentationBytes() == 0);
    const auto stats = allocator.GetSlabFreeStats();
    Require(stats.freeFullFrameBytes + stats.freeTailFrameBytes == kCapacity);
}

void CheckBuildFromBlobsAndFragmentation()
{
    // 1. Odporność BuildFromBlobs na std::bad_alloc (w tym wycofanie wcześniejszych ramek) oraz walidacja gva < bmAddr_
    constexpr uint64_t kBmAddr = 0x10000ULL;
    bool sawMallocFailed = false;
    for (int point = 0; point < 6; ++point) {
        MmcSlabAllocator allocator(0, MEDIA_DRAM, kBmAddr, kCapacity);
        const auto before = allocator.GetSlabFreeStats();
        std::vector<std::pair<std::string, MmcMemBlobDesc>> blobs;
        MmcMemBlobDesc invalidAddr;
        invalidAddr.rank_ = 0;
        invalidAddr.mediaType_ = MEDIA_DRAM;
        invalidAddr.gva_ = kBmAddr - kSlot;
        invalidAddr.size_ = kSlot;
        invalidAddr.allocSize_ = kSlot;
        blobs.emplace_back("invalid_addr", invalidAddr);

        MmcMemBlobDesc valid1;
        valid1.rank_ = 0;
        valid1.mediaType_ = MEDIA_DRAM;
        valid1.gva_ = kBmAddr;
        valid1.size_ = kSlot / 2;
        valid1.allocSize_ = kSlot;
        blobs.emplace_back("valid_blob_1", valid1);

        MmcMemBlobDesc valid2;
        valid2.rank_ = 0;
        valid2.mediaType_ = MEDIA_DRAM;
        valid2.gva_ = kBmAddr + before.frameSize;
        valid2.size_ = kSlot / 2;
        valid2.allocSize_ = kSlot;
        blobs.emplace_back("valid_blob_2", valid2);

        failAfter = point;
        const Result res = allocator.BuildFromBlobs(blobs);
        failAfter = -1;
        // Błąd alokacji pamięci (std::bad_alloc) przerywa odbudowę z MMC_MALLOC_FAILED i cofa wcześniejsze bloki.
        if (res == MMC_MALLOC_FAILED) {
            sawMallocFailed = true;
            Require(blobs.size() == 2 && blobs[0].first == "valid_blob_1" && blobs[1].first == "valid_blob_2");
            Require(allocator.GetUsageInfo().second == 0);
            Require(allocator.GetFragmentationBytes() == 0);
            Require(allocator.GetReservedSize() == 0);
            Require(allocator.GetSlabFreeStats().freeFullFrames == before.freeFullFrames);
        } else {
            Require(res == MMC_OK);
            Require(blobs.size() == 2 && blobs[0].first == "valid_blob_1" && blobs[1].first == "valid_blob_2");
            Require(allocator.GetUsageInfo().second == 2 * kSlot);
            Require(allocator.GetFragmentationBytes() == 2 * (kSlot - (kSlot / 2)));
        }
        Require(allocator.GetFreeSegments().empty());
    }
    Require(sawMallocFailed);

    // 2. Fragmentacja między klasami w MmcGlobalAllocator dla Slab (mniejsza klasa pełna, większa ma wolne sloty)
    const char *prevEnv = std::getenv("MMC_ENABLE_SLAB_ALLOCATOR");
    std::string savedEnv = prevEnv ? prevEnv : "";
    bool hadEnv = (prevEnv != nullptr);
    setenv("MMC_ENABLE_SLAB_ALLOCATOR", "1", 1);

    MmcGlobalAllocator global;
    MmcLocalMemlInitInfo info{};
    info.bmAddr_ = 0;
    info.capacity_ = kCapacity; // 1 MiB -> frameSize_ = 64 KiB (16 ramek)
    Require(global.Mount({0, MEDIA_DRAM}, info) == MMC_OK);
    Require(global.Start({0, MEDIA_DRAM}) == MMC_OK);

    // 1 ramka w klasie 8 KiB (7 zajętych slotów + 1 wolny slot = 8 KiB wolnego miejsca).
    // Pozostałe 15 ramek wypełniamy w 100% klasą 4 KiB (15 * 16 = 240 slotów).
    std::vector<MmcMemBlobPtr> held;
    for (int i = 0; i < 7; ++i) {
        std::vector<MmcMemBlobPtr> one;
        Require(global.Alloc({2 * kSlot, 1, MEDIA_DRAM, {0}, ALLOC_FORCE_BY_RANK}, one) == MMC_OK);
        held.push_back(one[0]);
    }
    for (int i = 0; i < 240; ++i) {
        std::vector<MmcMemBlobPtr> one;
        Require(global.Alloc({kSlot, 1, MEDIA_DRAM, {0}, ALLOC_FORCE_BY_RANK}, one) == MMC_OK);
        held.push_back(one[0]);
    }

    // Teraz maxContiguousFreeSize == 8 KiB (wolny slot w puli 8 KiB), ale klasa 4 KiB jest pełna i brak wolnych ramek!
    Require(global.GetMaxContiguousFreeSize(MEDIA_DRAM) == 2 * kSlot);
    Require(global.IsFragmentation(MEDIA_DRAM, kSlot));
    const auto fragRanks = global.GetFragmentedRanks(MEDIA_DRAM, kSlot);
    Require(fragRanks.size() == 1 && fragRanks[0] == 0);
    // Dla 8 KiB jest wolny slot -> brak fragmentacji; dla > frameSize_ (128 KiB) lub > freeSpace (12 KiB) -> brak fragmentacji.
    Require(!global.IsFragmentation(MEDIA_DRAM, 2 * kSlot));
    Require(!global.IsFragmentation(MEDIA_DRAM, 3 * kSlot));
    Require(!global.IsFragmentation(MEDIA_DRAM, 128 * 1024));

    global.Free(held);
    Require(global.Unmount({0, MEDIA_DRAM}) == MMC_OK);

    if (hadEnv) {
        setenv("MMC_ENABLE_SLAB_ALLOCATOR", savedEnv.c_str(), 1);
    } else {
        unsetenv("MMC_ENABLE_SLAB_ALLOCATOR");
    }
}

// --- Testerka współbieżności (wykrywanie zakleszczeń, zagłodzeń i wyścigów GVA) ---

struct StressConfig {
    bool stressMode = false;
    bool verbose = false;
    int threads = 8;
    int durationMs = 250;
    int starvationMs = 1500;
    uint64_t seed = 20261116ULL;
};

int64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct alignas(64) ThreadProgress {
    std::string role = "worker";
    std::atomic<uint64_t> ops{0};
    std::atomic<uint64_t> allocOk{0};
    std::atomic<uint64_t> allocFail{0};
    std::atomic<uint64_t> releases{0};
    std::atomic<int64_t> lastProgressMs{0};
    std::atomic<uint64_t> maxStallMs{0};
    std::atomic<bool> finished{false};
    std::exception_ptr error;
};

class PageOwnershipTracker {
public:
    PageOwnershipTracker(uint64_t bmAddr, uint64_t capacity, uint64_t pageSize = kSlot)
        : bmAddr_(bmAddr), capacity_(capacity), pageSize_(pageSize),
          pages_(capacity / pageSize)
    {
        for (auto &p : pages_) {
            p.store(0, std::memory_order_relaxed);
        }
    }

    void MarkAllocated(const MmcMemBlobPtr &blob)
    {
        if (blob == nullptr) {
            return;
        }
        const uint64_t gva = blob->Gva();
        const uint64_t allocSize = blob->AllocSize();
        if (gva < bmAddr_ || allocSize == 0 || allocSize % pageSize_ != 0 || (gva - bmAddr_) % pageSize_ != 0) {
            throw std::runtime_error("Invalid blob alignment or base address in concurrency test");
        }
        const uint64_t offset = gva - bmAddr_;
        if (offset >= capacity_ || allocSize > capacity_ - offset) {
            throw std::runtime_error("Blob GVA range out of capacity bounds");
        }
        const size_t first = static_cast<size_t>(offset / pageSize_);
        const size_t count = static_cast<size_t>(allocSize / pageSize_);
        for (size_t i = 0; i < count; ++i) {
            if (pages_[first + i].exchange(1, std::memory_order_acq_rel) != 0) {
                // Cofamy ustawione bity przed rzutem błędu.
                for (size_t j = 0; j < i; ++j) {
                    pages_[first + j].store(0, std::memory_order_release);
                }
                std::ostringstream oss;
                oss << "GVA overlap / double allocation detected at page " << (first + i)
                    << " (gva=" << gva << ", allocSize=" << allocSize << ")";
                throw std::runtime_error(oss.str());
            }
        }
    }

    void MarkReleased(const MmcMemBlobPtr &blob)
    {
        if (blob == nullptr) {
            return;
        }
        const uint64_t offset = blob->Gva() - bmAddr_;
        const size_t first = static_cast<size_t>(offset / pageSize_);
        const size_t count = static_cast<size_t>(blob->AllocSize() / pageSize_);
        for (size_t i = 0; i < count; ++i) {
            if (pages_[first + i].exchange(0, std::memory_order_acq_rel) != 1) {
                std::ostringstream oss;
                oss << "Released page was not owned at page " << (first + i);
                throw std::runtime_error(oss.str());
            }
        }
    }

    void AssertAllFree() const
    {
        for (size_t i = 0; i < pages_.size(); ++i) {
            if (pages_[i].load(std::memory_order_acquire) != 0) {
                std::ostringstream oss;
                oss << "Leaked page ownership after all releases at page " << i;
                throw std::runtime_error(oss.str());
            }
        }
    }

private:
    uint64_t bmAddr_ = 0;
    uint64_t capacity_ = 0;
    uint64_t pageSize_ = kSlot;
    std::vector<std::atomic<uint8_t>> pages_;
};

class SharedBlobMailbox {
public:
    explicit SharedBlobMailbox(size_t slots = 32) : slots_(slots), locks_(slots) {}

    // Jeśli docelowa skrzynka była zajęta, zwraca poprzedni blok do zwolnienia przez wywołującego.
    MmcMemBlobPtr SwapOrDeposit(size_t idx, MmcMemBlobPtr incoming)
    {
        const size_t s = idx % slots_.size();
        std::lock_guard<std::mutex> guard(locks_[s]);
        MmcMemBlobPtr old = std::move(slots_[s]);
        slots_[s] = std::move(incoming);
        return old;
    }

    MmcMemBlobPtr TryTake(size_t idx)
    {
        const size_t s = idx % slots_.size();
        std::lock_guard<std::mutex> guard(locks_[s]);
        MmcMemBlobPtr taken = std::move(slots_[s]);
        slots_[s] = nullptr;
        return taken;
    }

    template <typename ReleaseFn>
    void DrainAll(ReleaseFn &&releaseFn)
    {
        for (size_t i = 0; i < slots_.size(); ++i) {
            std::lock_guard<std::mutex> guard(locks_[i]);
            if (slots_[i] != nullptr) {
                releaseFn(slots_[i]);
                slots_[i] = nullptr;
            }
        }
    }

private:
    std::vector<MmcMemBlobPtr> slots_;
    std::vector<std::mutex> locks_;
};

template <typename WorkerFactory>
void RunConcurrentPhase(const std::string &name, const StressConfig &cfg, int numThreads,
                        const std::vector<std::string> &roles, WorkerFactory &&makeWorker)
{
    std::vector<ThreadProgress> progress(static_cast<size_t>(numThreads));
    const int64_t t0 = NowMs();
    for (int i = 0; i < numThreads; ++i) {
        progress[i].role = (static_cast<size_t>(i) < roles.size()) ? roles[i] : "worker";
        progress[i].lastProgressMs.store(t0, std::memory_order_relaxed);
    }

    std::atomic<bool> startFlag{false};
    std::atomic<bool> stopFlag{false};
    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(numThreads));

    for (int i = 0; i < numThreads; ++i) {
        threads.emplace_back([&, i]() {
            while (!startFlag.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            progress[i].lastProgressMs.store(NowMs(), std::memory_order_relaxed);
            try {
                makeWorker(i, startFlag, stopFlag, progress[i]);
            } catch (...) {
                progress[i].error = std::current_exception();
                stopFlag.store(true, std::memory_order_release);
            }
            progress[i].finished.store(true, std::memory_order_release);
        });
    }

    const int64_t startMs = NowMs();
    startFlag.store(true, std::memory_order_release);

    std::string anomalyMsg;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        const int64_t now = NowMs();
        if (!stopFlag.load(std::memory_order_acquire) && (now - startMs) >= cfg.durationMs) {
            stopFlag.store(true, std::memory_order_release);
        }

        int activeCount = 0;
        int stalledThreads = 0;
        int hardestThread = -1;
        uint64_t hardestStall = 0;
        uint64_t totalOpsElsewhere = 0;

        for (int i = 0; i < numThreads; ++i) {
            if (!progress[i].finished.load(std::memory_order_acquire)) {
                activeCount++;
                const int64_t last = progress[i].lastProgressMs.load(std::memory_order_relaxed);
                const uint64_t stall = (now > last) ? static_cast<uint64_t>(now - last) : 0ULL;
                uint64_t prevMax = progress[i].maxStallMs.load(std::memory_order_relaxed);
                while (stall > prevMax &&
                       !progress[i].maxStallMs.compare_exchange_weak(prevMax, stall, std::memory_order_relaxed)) {}
                if (stall >= static_cast<uint64_t>(cfg.starvationMs)) {
                    stalledThreads++;
                    if (stall > hardestStall) {
                        hardestStall = stall;
                        hardestThread = i;
                    }
                } else {
                    totalOpsElsewhere += progress[i].ops.load(std::memory_order_relaxed);
                }
            } else {
                totalOpsElsewhere += progress[i].ops.load(std::memory_order_relaxed);
            }
        }

        if (stalledThreads > 0) {
            std::ostringstream oss;
            const bool isDeadlock = (stalledThreads == activeCount);
            oss << "[" << name << "] " << (isDeadlock ? "DEADLOCK" : "STARVATION")
                << " suspected! stalledThreads=" << stalledThreads << "/" << activeCount
                << ", worstThread=" << hardestThread << " (" << progress[hardestThread].role
                << ", ops=" << progress[hardestThread].ops.load()
                << ", stalledForMs=" << hardestStall
                << "), otherThreadsOps=" << totalOpsElsewhere;
            anomalyMsg = oss.str();
            stopFlag.store(true, std::memory_order_release);
            // Przy prawdziwym zakleszczeniu join() zablokowałby proces na zawsze; wypisujemy diagnostykę i przerywamy.
            std::cerr << anomalyMsg << '\n';
            std::cerr.flush();
            std::_Exit(2);
        }

        if (activeCount == 0) {
            break;
        }
    }

    for (auto &t : threads) {
        t.join();
    }

    for (int i = 0; i < numThreads; ++i) {
        if (progress[i].error) {
            std::rethrow_exception(progress[i].error);
        }
    }

    if (cfg.verbose) {
        uint64_t totalOps = 0;
        uint64_t minOps = UINT64_MAX;
        uint64_t maxOps = 0;
        uint64_t worstStall = 0;
        for (const auto &p : progress) {
            const uint64_t ops = p.ops.load();
            totalOps += ops;
            minOps = std::min(minOps, ops);
            maxOps = std::max(maxOps, ops);
            worstStall = std::max(worstStall, p.maxStallMs.load());
        }
        std::cout << "[CONCURRENCY] " << name << ": threads=" << numThreads
                  << ", durationMs=" << (NowMs() - startMs)
                  << ", totalOps=" << totalOps
                  << ", minOps/thr=" << minOps
                  << ", maxOps/thr=" << maxOps
                  << ", maxStallMs=" << worstStall << "\n";
    }
}

void CheckConcurrentFrameChurnAndAba(const StressConfig &cfg)
{
    // 2 MiB -> frameSize_ = 128 KiB (16 ramek). Rozmiary 16K..64K dają 2..8 slotów na ramkę,
    // wymuszając częste przejścia pełna <-> niepełna <-> pusta i zmianę właściciela (ABA na poolIndex).
    constexpr uint64_t kCap = 2ULL * 1024 * 1024;
    constexpr uint64_t kBaseAddr = 0x400000ULL;
    TestAllocator allocator(kCap, kBaseAddr);
    PageOwnershipTracker tracker(kBaseAddr, kCap);
    SharedBlobMailbox mailbox(32);

    const int n = std::max(6, cfg.threads);
    std::vector<std::string> roles;
    for (int i = 0; i < n; ++i) {
        if (i == n - 1) {
            roles.push_back("observer");
        } else {
            roles.push_back("churn_worker");
        }
    }

    const std::vector<uint64_t> reqSizes{
        12289,       // wyrówna się do 16 KiB (internalFrag)
        16384,       // 16 KiB -> 8 slotów/ramkę
        20480,       // 20 KiB -> tail waste w ramce 128 KiB (8 KiB ogona)
        32768,       // 32 KiB -> 4 sloty/ramkę
        40960,       // 40 KiB -> tail waste 8 KiB
        65536        // 64 KiB -> 2 sloty/ramkę
    };

    auto safeRelease = [&](const MmcMemBlobPtr &blob, ThreadProgress &tp) {
        if (blob == nullptr) {
            return;
        }
        tracker.MarkReleased(blob);
        Require(allocator.Release(blob) == MMC_OK, "Concurrent Release failed on valid blob");
        tp.releases.fetch_add(1, std::memory_order_relaxed);
    };

    RunConcurrentPhase("FrameChurnAndABA", cfg, n, roles,
        [&](int tid, const std::atomic<bool> &, const std::atomic<bool> &stopFlag, ThreadProgress &tp) {
            std::mt19937_64 rng(cfg.seed + static_cast<uint64_t>(tid) * 1315423911ULL);
            std::vector<MmcMemBlobPtr> local;
            local.reserve(16);

            while (!stopFlag.load(std::memory_order_acquire)) {
                if (roles[tid] == "observer") {
                    const auto usage = allocator.GetUsageInfo();
                    const uint64_t frag = allocator.GetFragmentationBytes();
                    const uint64_t reserved = allocator.GetReservedSize();
                    Require(usage.second <= usage.first, "allocatedSize exceeded capacity");
                    Require(reserved <= usage.first, "reservedSize exceeded capacity");
                    Require(frag <= usage.second, "fragmentationBytes exceeded allocatedSize");
                    (void)allocator.GetMaxContiguousFreeSize();
                    const auto stats = allocator.GetSlabFreeStats();
                    Require(stats.freeFullFrameBytes + stats.freeTailFrameBytes <= kCap,
                            "Slab free frame stats overflow");
                    for (uint64_t sz : reqSizes) {
                        (void)allocator.CanAlloc(sz);
                        (void)allocator.IsFragmentedFor(MmcBlobAllocator::AllocSizeAlignUp(sz));
                    }
                    // Observer również czasem zdejmuje z mailboxa, aby testować cross-thread release.
                    MmcMemBlobPtr cross = mailbox.TryTake(rng());
                    safeRelease(cross, tp);
                } else {
                    const uint64_t sz = reqSizes[rng() % reqSizes.size()];
                    auto blob = allocator.Alloc(sz);
                    if (blob != nullptr) {
                        tracker.MarkAllocated(blob);
                        tp.allocOk.fetch_add(1, std::memory_order_relaxed);
                        if ((rng() % 3ULL) == 0) {
                            MmcMemBlobPtr evicted = mailbox.SwapOrDeposit(rng(), std::move(blob));
                            safeRelease(evicted, tp);
                        } else {
                            local.push_back(std::move(blob));
                        }
                    } else {
                        tp.allocFail.fetch_add(1, std::memory_order_relaxed);
                    }

                    if (local.size() > 6 || (blob == nullptr && !local.empty())) {
                        const size_t idx = rng() % local.size();
                        MmcMemBlobPtr victim = std::move(local[idx]);
                        local[idx] = std::move(local.back());
                        local.pop_back();
                        safeRelease(victim, tp);
                    }
                    if ((rng() & 7ULL) == 0) {
                        MmcMemBlobPtr cross = mailbox.TryTake(rng());
                        safeRelease(cross, tp);
                    }
                }

                tp.ops.fetch_add(1, std::memory_order_relaxed);
                tp.lastProgressMs.store(NowMs(), std::memory_order_relaxed);
            }

            for (const auto &b : local) {
                safeRelease(b, tp);
            }
        });

    ThreadProgress cleanupTp;
    mailbox.DrainAll([&](const MmcMemBlobPtr &b) { safeRelease(b, cleanupTp); });

    tracker.AssertAllFree();
    Require(allocator.GetUsageInfo().second == 0, "FrameChurn leaked allocatedSize");
    Require(allocator.GetFragmentationBytes() == 0, "FrameChurn leaked fragmentationBytes");
    Require(allocator.GetReservedSize() == 0, "FrameChurn leaked reservedSize");
    const auto finalStats = allocator.GetSlabFreeStats();
    Require(finalStats.freeFullFrameBytes + finalStats.freeTailFrameBytes == kCap,
            "FrameChurn did not return all frames to manager");
}

void CheckConcurrentClassCreationAndStarvation(const StressConfig &cfg)
{
    // Celowe obciążenie na spinlock_ oraz poolLock w klasach gorących i rotacji >256 klas,
    // aby sprawdzić czy StatsObserver i ClassExplorer nie ulegają zagłodzeniu lub wyścigom ABA.
    constexpr uint64_t kCap = 64ULL * 1024 * 1024; // frameSize_ = 4 MiB (16 ramek po 1024 * 4 KiB)
    TestAllocator allocator(kCap, 0);
    PageOwnershipTracker tracker(0, kCap);

    const int n = std::max(6, cfg.threads);
    std::vector<std::string> roles;
    for (int i = 0; i < n; ++i) {
        if (i == 0) {
            roles.push_back("stats_observer");
        } else if (i == 1 || i == 2) {
            roles.push_back("class_explorer");
        } else {
            roles.push_back("hot_hammer");
        }
    }

    RunConcurrentPhase("ClassCreationAndStarvation", cfg, n, roles,
        [&](int tid, const std::atomic<bool> &, const std::atomic<bool> &stopFlag, ThreadProgress &tp) {
            std::mt19937_64 rng(cfg.seed + static_cast<uint64_t>(tid) * 2654435761ULL);

            if (roles[tid] == "stats_observer") {
                while (!stopFlag.load(std::memory_order_acquire)) {
                    const auto summary = allocator.GetSlabFreeStats();
                    Require(summary.freeFullFrameBytes + summary.freeTailFrameBytes <= kCap,
                            "Invalid free frame bytes in StatsObserver");
                    (void)allocator.GetMaxContiguousFreeSize();
                    (void)allocator.IsFragmentedFor(kSlot);
                    (void)allocator.CanAlloc(kSlot);
                    tp.ops.fetch_add(1, std::memory_order_relaxed);
                    tp.lastProgressMs.store(NowMs(), std::memory_order_relaxed);
                }
            } else if (roles[tid] == "class_explorer") {
                uint64_t step = static_cast<uint64_t>(tid) * 170ULL;
                while (!stopFlag.load(std::memory_order_acquire)) {
                    // Rotuje do 340 różnych klas (> SLAB_MAX_POOLS = 256), aktywując równoległe odzyskiwanie.
                    const uint64_t mult = (step++ % 340) + 3;
                    const uint64_t sz = mult * kSlot;
                    auto blob = allocator.Alloc(sz);
                    if (blob != nullptr) {
                        tracker.MarkAllocated(blob);
                        tp.allocOk.fetch_add(1, std::memory_order_relaxed);
                        tracker.MarkReleased(blob);
                        Require(allocator.Release(blob) == MMC_OK, "ClassExplorer Release failed");
                        tp.releases.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        tp.allocFail.fetch_add(1, std::memory_order_relaxed);
                    }
                    tp.ops.fetch_add(1, std::memory_order_relaxed);
                    tp.lastProgressMs.store(NowMs(), std::memory_order_relaxed);
                }
            } else {
                const uint64_t hotSize = ((tid % 2) == 0) ? kSlot : (2 * kSlot);
                while (!stopFlag.load(std::memory_order_acquire)) {
                    MmcMemBlobPtr held[3];
                    for (int k = 0; k < 3; ++k) {
                        held[k] = allocator.Alloc(hotSize);
                        if (held[k] != nullptr) {
                            tracker.MarkAllocated(held[k]);
                            tp.allocOk.fetch_add(1, std::memory_order_relaxed);
                        } else {
                            tp.allocFail.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                    for (const auto &b : held) {
                        if (b != nullptr) {
                            tracker.MarkReleased(b);
                            Require(allocator.Release(b) == MMC_OK, "HotHammer Release failed");
                            tp.releases.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                    if ((rng() & 3ULL) == 0) {
                        auto single = allocator.Alloc(hotSize);
                        if (single != nullptr) {
                            tracker.MarkAllocated(single);
                            tracker.MarkReleased(single);
                            Require(allocator.Release(single) == MMC_OK, "HotHammer single Release failed");
                        }
                    }
                    tp.ops.fetch_add(1, std::memory_order_relaxed);
                    tp.lastProgressMs.store(NowMs(), std::memory_order_relaxed);
                }
            }
        });

    tracker.AssertAllFree();
    Require(allocator.GetUsageInfo().second == 0, "ClassCreation leaked allocatedSize");
    Require(allocator.GetFragmentationBytes() == 0, "ClassCreation leaked fragmentationBytes");
    Require(allocator.GetReservedSize() == 0, "ClassCreation leaked reservedSize");
}

void CheckConcurrentStartStopAndGlobalAlloc(const StressConfig &cfg)
{
    // Część 1: Start/Stop w trakcie równoległych Alloc / CanAlloc / Release
    {
        constexpr uint64_t kCap = 2ULL * 1024 * 1024;
        TestAllocator allocator(kCap, 0);
        PageOwnershipTracker tracker(0, kCap);

        const int n = std::max(5, cfg.threads);
        std::vector<std::string> roles;
        roles.push_back("start_stop_toggler");
        for (int i = 1; i < n; ++i) {
            roles.push_back("worker");
        }

        RunConcurrentPhase("StartStopRace", cfg, n, roles,
            [&](int tid, const std::atomic<bool> &, const std::atomic<bool> &stopFlag, ThreadProgress &tp) {
                std::mt19937_64 rng(cfg.seed + static_cast<uint64_t>(tid) * 977ULL);
                if (tid == 0) {
                    while (!stopFlag.load(std::memory_order_acquire)) {
                        allocator.Stop();
                        Require(allocator.Alloc(kSlot) == nullptr, "StartStop Alloc succeeded while stopped");
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                        allocator.Start();
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                        tp.ops.fetch_add(1, std::memory_order_relaxed);
                        tp.lastProgressMs.store(NowMs(), std::memory_order_relaxed);
                    }
                    allocator.Start();
                } else {
                    std::vector<MmcMemBlobPtr> held;
                    while (!stopFlag.load(std::memory_order_acquire)) {
                        const uint64_t sz = ((rng() % 4) + 1) * kSlot;
                        (void)allocator.CanAlloc(sz);
                        auto b = allocator.Alloc(sz);
                        if (b != nullptr) {
                            tracker.MarkAllocated(b);
                            held.push_back(std::move(b));
                        }
                        if (held.size() > 4 || ((rng() & 1ULL) == 0 && !held.empty())) {
                            tracker.MarkReleased(held.back());
                            Require(allocator.Release(held.back()) == MMC_OK, "StartStop Release failed");
                            held.pop_back();
                        }
                        tp.ops.fetch_add(1, std::memory_order_relaxed);
                        tp.lastProgressMs.store(NowMs(), std::memory_order_relaxed);
                    }
                    for (const auto &b : held) {
                        tracker.MarkReleased(b);
                        Require(allocator.Release(b) == MMC_OK, "StartStop cleanup Release failed");
                    }
                }
            });

        allocator.Start();
        tracker.AssertAllFree();
        Require(allocator.GetUsageInfo().second == 0, "StartStop leaked memory");
    }

    // Część 2: Równoległy MmcGlobalAllocator w trybie Slab na 4 rankach
    {
        SlabEnvGuard slabEnv("1");
        constexpr int kRanks = 4;
        MmcGlobalAllocator global;
        std::vector<std::unique_ptr<PageOwnershipTracker>> rankTrackers;
        for (uint32_t r = 0; r < kRanks; ++r) {
            MmcLocalMemlInitInfo info{};
            info.bmAddr_ = static_cast<uint64_t>(r) * kCapacity * 2ULL;
            info.capacity_ = kCapacity;
            Require(global.Mount({r, MEDIA_DRAM}, info) == MMC_OK, "Global mount failed");
            Require(global.Start({r, MEDIA_DRAM}) == MMC_OK, "Global start failed");
            rankTrackers.push_back(std::make_unique<PageOwnershipTracker>(info.bmAddr_, info.capacity_));
        }

        const int n = std::max(6, cfg.threads);
        std::vector<std::string> roles(static_cast<size_t>(n), "global_worker");
        roles.back() = "global_frag_prober";

        RunConcurrentPhase("GlobalMultiRankSlab", cfg, n, roles,
            [&](int tid, const std::atomic<bool> &, const std::atomic<bool> &stopFlag, ThreadProgress &tp) {
                std::mt19937_64 rng(cfg.seed + static_cast<uint64_t>(tid) * 4099ULL);
                if (roles[tid] == "global_frag_prober") {
                    while (!stopFlag.load(std::memory_order_acquire)) {
                        (void)global.IsFragmentation(MEDIA_DRAM, kSlot);
                        (void)global.IsFragmentation(MEDIA_DRAM, 2 * kSlot);
                        (void)global.GetFragmentedRanks(MEDIA_DRAM, kSlot);
                        (void)global.GetMaxContiguousFreeSize(MEDIA_DRAM);
                        uint64_t total[MEDIA_NONE]{};
                        uint64_t used[MEDIA_NONE]{};
                        global.GetUsedInfo(total, used);
                        Require(used[MEDIA_DRAM] <= total[MEDIA_DRAM], "Global usedSize exceeded total");
                        tp.ops.fetch_add(1, std::memory_order_relaxed);
                        tp.lastProgressMs.store(NowMs(), std::memory_order_relaxed);
                    }
                } else {
                    std::vector<std::vector<MmcMemBlobPtr>> held;
                    while (!stopFlag.load(std::memory_order_acquire)) {
                        const uint32_t prefRank = static_cast<uint32_t>(rng() % kRanks);
                        const uint64_t sz = ((rng() % 3) + 1) * kSlot;
                        const uint32_t numBlobs = ((rng() % 3) == 0) ? 2U : 1U;
                        std::vector<MmcMemBlobPtr> blobs;
                        if (global.Alloc({sz, numBlobs, MEDIA_DRAM, {prefRank}, ALLOC_ARRANGE}, blobs) == MMC_OK) {
                            Require(blobs.size() == numBlobs, "Incomplete replica count on MMC_OK");
                            for (const auto &b : blobs) {
                                rankTrackers[b->Rank()]->MarkAllocated(b);
                            }
                            held.push_back(std::move(blobs));
                        }
                        while (held.size() > 8 || (!held.empty() && (rng() & 1ULL) == 0)) {
                            for (const auto &b : held.back()) {
                                rankTrackers[b->Rank()]->MarkReleased(b);
                            }
                            global.Free(held.back());
                            held.pop_back();
                        }
                        tp.ops.fetch_add(1, std::memory_order_relaxed);
                        tp.lastProgressMs.store(NowMs(), std::memory_order_relaxed);
                    }
                    for (auto &group : held) {
                        for (const auto &b : group) {
                            rankTrackers[b->Rank()]->MarkReleased(b);
                        }
                        global.Free(group);
                    }
                }
            });

        uint64_t total[MEDIA_NONE]{};
        uint64_t used[MEDIA_NONE]{};
        global.GetUsedInfo(total, used);
        Require(used[MEDIA_DRAM] == 0, "GlobalAllocator leaked usedSize");
        for (uint32_t r = 0; r < kRanks; ++r) {
            rankTrackers[r]->AssertAllFree();
            Require(global.Unmount({r, MEDIA_DRAM}) == MMC_OK, "Global unmount failed");
        }
    }
}

StressConfig ParseCliArgs(int argc, char **argv)
{
    StressConfig cfg;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--stress") {
            cfg.stressMode = true;
            cfg.verbose = true;
            cfg.threads = std::max(8, static_cast<int>(std::thread::hardware_concurrency()));
            cfg.durationMs = 2500;
        } else if (arg == "--verbose") {
            cfg.verbose = true;
        } else if (arg == "--threads" && i + 1 < argc) {
            cfg.threads = std::max(2, std::stoi(argv[++i]));
        } else if (arg == "--duration-ms" && i + 1 < argc) {
            cfg.durationMs = std::max(50, std::stoi(argv[++i]));
        } else if (arg == "--starvation-ms" && i + 1 < argc) {
            cfg.starvationMs = std::max(100, std::stoi(argv[++i]));
        } else if (arg == "--seed" && i + 1 < argc) {
            cfg.seed = std::stoull(argv[++i]);
        }
    }
    return cfg;
}

int main(int argc, char **argv)
{
    MmcOutLogger::Instance().SetLogLevel(ERROR_LEVEL);
    const StressConfig cfg = ParseCliArgs(argc, argv);
    try {
        if (!cfg.stressMode) {
            CheckFailures();
            CheckInvalidRelease();
            CheckIndependentFastPaths();
            CheckConcurrentPools();
            CheckBuildFromBlobsAndFragmentation();
        }
        CheckConcurrentFrameChurnAndAba(cfg);
        CheckConcurrentClassCreationAndStarvation(cfg);
        CheckConcurrentStartStopAndGlobalAlloc(cfg);
        if (!cfg.stressMode) {
            std::cout << "fault injection, independent fast paths, concurrent pools: OK\n";
        } else {
            std::cout << "concurrency stress test: OK\n";
        }
    } catch (const std::exception &error) {
        failAfter = -1;
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
