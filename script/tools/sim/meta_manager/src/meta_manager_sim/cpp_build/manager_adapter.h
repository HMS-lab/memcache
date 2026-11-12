/* Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 * Licensed under Mulan PSL v2. */
#pragma once
#include <atomic>
#include <chrono>
#include "mmc_meta_manager.h"
namespace ock::mmc {
struct OperationResult {
    Result code = MMC_OK;
    uint64_t operationId = 0;
    uint64_t elapsedNs = 0;
    MmcMemMetaDesc descriptor;
};

class Manager {
public:
    Manager(uint64_t capacity, uint16_t high, uint16_t low, uint64_t leaseTtlMs, bool maxValueAlignment,
            const std::string &policy, uint32_t shards);

    ~Manager();

    OperationResult Alloc(const std::string &key, uint64_t size);

    OperationResult Get(const std::string &key);

    OperationResult Update(const std::string &key, uint64_t operationId, uint32_t rank, uint16_t media,
                           BlobActionResult action);

    OperationResult Remove(const std::string &key);

    OperationResult CheckAndEvict(uint64_t wantedSize);
    uint64_t PeakCalls() const;

    bool WaitForCleanup(uint64_t timeoutMs);

    std::string SegmentInfo() const;
    uint64_t Reclaimed() const;
    size_t KeyCount() const;
    std::vector<std::string> Keys() const;
    void Close();

private:
    static uint64_t ElapsedNs(std::chrono::steady_clock::time_point start);
    std::atomic<uint64_t> activeCalls_{0};
    std::atomic<uint64_t> peakCalls_{0};
    std::atomic<uint64_t> reclaimed_{0};
    std::atomic<uint64_t> allocations_{0};
    MmcMetaManagerPtr manager_;
};
} // namespace ock::mmc
