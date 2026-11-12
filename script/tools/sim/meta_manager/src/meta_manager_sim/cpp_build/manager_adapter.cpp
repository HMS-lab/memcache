/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 * Licensed under Mulan PSL v2.
 * http://license.coscl.org.cn/MulanPSL2
 */
#include "manager_adapter.h"
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "mmc_meta_manager.h"
#include "mmc_msg_client_meta.h"
#include "ptracer.h"

extern "C" {
ptracer g_mmc_tracer{};
}

namespace py = pybind11;
namespace ock::mmc {
namespace {
class ActiveCall {
public:
    ActiveCall(std::atomic<uint64_t> &active, std::atomic<uint64_t> &peak) : active_(active)
    {
        auto count = active_.fetch_add(1) + 1;
        auto previous = peak.load();
        while (count > previous && !peak.compare_exchange_weak(previous, count)) {}
    }
    ~ActiveCall()
    {
        active_.fetch_sub(1);
    }

private:
    std::atomic<uint64_t> &active_;
};
constexpr uint32_t kRank = 0;
std::atomic<uint64_t> issuedIds{0};
uint64_t NextOperationId()
{
    if (issuedIds.fetch_add(1) >= UINT32_MAX) {
        throw std::runtime_error("Operation ID budget exhausted; start a new process");
    }
    return GenerateOperateId(kRank);
}
constexpr uint32_t kSingleBlob = 1;
constexpr uint64_t kPoolBaseAddress = 4096;
constexpr auto kPollInterval = std::chrono::milliseconds(1);

void CheckResult(Result result, const char *operation)
{
    if (result != MMC_OK) {
        throw std::runtime_error(std::string(operation) + " failed: " + std::to_string(result));
    }
}

} // namespace
Manager::Manager(uint64_t capacity, uint16_t high, uint16_t low, uint64_t leaseTtlMs, bool maxValueAlignment,
                 const std::string &policy, uint32_t shards)
{
    // Native logs go to stderr so stdout remains a usable report stream.
    // Missing keys are logged as errors by the service implementation.
    // Return codes carry these expected outcomes to Python instead.
    MmcOutLogger::Instance().SetLogLevel(FATAL_LEVEL);
    MmcOutLogger::Instance().SetExternalLogFunction(
        [](int level, const char *message) {
            if (level == FATAL_LEVEL) {
                std::cerr << message << '\n';
            }
        },
        true);
    auto lruType = policy == "lru" ? MetaLruType::LRU : MetaLruType::SHARDED_LRU;
    manager_ = MmcMakeRef<MmcMetaManager>(leaseTtlMs, high, low, MmcMetaExtConfig{}, 0, 0, shards, lruType);
    CheckResult(manager_->Start(), "Start");
    MmcMetaChangeCallbacks callbacks;
    callbacks.removed = [this](const std::string &, uint32_t, uint16_t) { reclaimed_.fetch_add(kSingleBlob); };
    manager_->SetChangeCallbacks(callbacks);
    std::vector<std::pair<std::string, MmcMemBlobDesc>> blobs;
    CheckResult(manager_->Mount({kRank, MEDIA_DRAM}, {kPoolBaseAddress, capacity}, blobs, maxValueAlignment), "Mount");
}

Manager::~Manager()
{
    Close();
}

OperationResult Manager::Alloc(const std::string &key, uint64_t size)
{
    ActiveCall active(activeCalls_, peakCalls_);
    OperationResult result;
    result.operationId = NextOperationId();
    const AllocOptions options{size, kSingleBlob, MEDIA_DRAM, {kRank}, ALLOC_FORCE_BY_RANK};
    auto start = std::chrono::steady_clock::now();
    result.code = manager_->Alloc(key, options, result.operationId, 0, result.descriptor);
    result.elapsedNs = ElapsedNs(start);
    if (result.code == MMC_OK) {
        ++allocations_;
    }
    return result;
}

OperationResult Manager::Get(const std::string &key)
{
    ActiveCall active(activeCalls_, peakCalls_);
    OperationResult result;
    result.operationId = NextOperationId();
    auto start = std::chrono::steady_clock::now();
    result.code = manager_->Get(key, result.operationId, nullptr, result.descriptor);
    result.elapsedNs = ElapsedNs(start);
    return result;
}

OperationResult Manager::Update(const std::string &key, uint64_t operationId, uint32_t rank, uint16_t media,
                                BlobActionResult action)
{
    ActiveCall active(activeCalls_, peakCalls_);
    OperationResult result;
    result.operationId = operationId;
    auto start = std::chrono::steady_clock::now();
    result.code = manager_->UpdateState(key, {rank, static_cast<MediaType>(media)}, action, operationId);
    result.elapsedNs = ElapsedNs(start);
    return result;
}

OperationResult Manager::Remove(const std::string &key)
{
    OperationResult result;
    auto start = std::chrono::steady_clock::now();
    result.code = manager_->Remove(key);
    result.elapsedNs = ElapsedNs(start);
    return result;
}

OperationResult Manager::CheckAndEvict(uint64_t wantedSize)
{
    OperationResult result;
    auto start = std::chrono::steady_clock::now();
    manager_->CheckAndEvict(MEDIA_DRAM, wantedSize);
    result.elapsedNs = ElapsedNs(start);
    return result;
}
uint64_t Manager::PeakCalls() const
{
    return peakCalls_.load();
}

bool Manager::WaitForCleanup(uint64_t timeoutMs)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    do {
        // Only one memory pool/blob per key is mounted and workload calls
        // pause during this wait. Each removed callback follows FinalizeBlobs.
        if (!manager_->IsEvictRunning() && allocations_.load() == KeyCount() + reclaimed_.load()) {
            return true;
        }
        std::this_thread::sleep_for(kPollInterval);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

std::string Manager::SegmentInfo() const
{
    return manager_->GetAllSegmentInfo().dump();
}

uint64_t Manager::Reclaimed() const
{
    return reclaimed_.load();
}

size_t Manager::KeyCount() const
{
    size_t count = 0;
    CheckResult(manager_->GetKeyCount(count), "GetKeyCount");
    return count;
}

std::vector<std::string> Manager::Keys() const
{
    std::vector<std::string> keys;
    CheckResult(manager_->GetAllKeys(keys), "GetAllKeys");
    return keys;
}

void Manager::Close()
{
    if (manager_ != nullptr) {
        manager_->RemoveAll();
        manager_->Stop();
        manager_ = nullptr;
    }
}

uint64_t Manager::ElapsedNs(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
}
} // namespace ock::mmc
