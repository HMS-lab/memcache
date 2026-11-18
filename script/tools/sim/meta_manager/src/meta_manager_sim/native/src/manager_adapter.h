/* Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
  Licensed under Mulan PSL v2. See the repository LICENSE file. */

#ifndef META_MANAGER_SIM_MANAGER_ADAPTER_H
#define META_MANAGER_SIM_MANAGER_ADAPTER_H

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <nlohmann/json.hpp>

#include "mmc_meta_manager.h"

namespace ock::mmc {

using Json = nlohmann::json;

class Manager {
public:
    explicit Manager(const Json &config, bool detailedStats);
    ~Manager();

    void Access(const std::string &key, uint64_t size);

    Json Progress() const;
    Json TakeLatencies();
    Json Snapshot();
    void Close();

private:
    using Clock = std::chrono::steady_clock;

    uint64_t NextId();
    void CompleteLease(const std::string &key, uint64_t size, uint64_t id, const MmcMemMetaDesc &descriptor, bool write,
                       Clock::time_point before);
    void WaitForCompletion();
    size_t CachedKeyCount() const;

    void RecordTiming(const std::string &name, Clock::time_point before);
    void RecordAccess(const std::string &model, uint64_t size, bool hit, uint64_t reserved);
    void RecordAllocation(uint64_t size, uint64_t reserved, Result code);

    MmcMetaManagerPtr manager_;
    std::set<uint32_t> poolRanks_;
    std::atomic<uint64_t> evictedKeys_{0};
    uint64_t issuedIds_ = 0;
    bool detailedStats_;

    Json counters_;
    Json prefixes_ = Json::object();
    Json allocations_ = Json::object();
    Json latencies_ = Json::object();
};

} // namespace ock::mmc

#endif // META_MANAGER_SIM_MANAGER_ADAPTER_H
