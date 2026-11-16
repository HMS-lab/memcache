/* Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
  Licensed under Mulan PSL v2. See the repository LICENSE file. */

#include "manager_adapter.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "mmc_msg_client_meta.h"
#include "ptracer.h"

extern "C" {
ptracer g_mmc_tracer{};
}

namespace ock::mmc {
namespace {

constexpr uint32_t kClientRank = 0;
constexpr uint32_t kBlobCopies = 1;
constexpr uint64_t kPoolBase = 4096;
constexpr auto kCompletionTimeout = std::chrono::seconds(30);
constexpr auto kCompletionPoll = std::chrono::milliseconds(1);

void Check(Result code, const char *operation)
{
    if (code != MMC_OK) {
        throw std::runtime_error(std::string(operation) + " failed: " + std::to_string(code));
    }
}

void Add(Json &object, const std::string &name, uint64_t amount = 1)
{
    const auto previous = object.value(name, uint64_t{0});
    if (amount > UINT64_MAX - previous) {
        throw std::runtime_error("Counter overflow: " + name);
    }
    object[name] = previous + amount;
}

Json AccessCounters()
{
    Json counters = Json::object();
    for (const auto *name : {"reads", "hits", "misses", "read_bytes", "hit_bytes", "miss_bytes", "allocs", "alloc_ok",
                             "alloc_fail", "alloc_bytes", "alloc_reserved_bytes", "fail_bytes"}) {
        counters[name] = uint64_t{0};
    }
    return counters;
}

void CountAccess(Json &counters, uint64_t size, bool hit, uint64_t reserved)
{
    Add(counters, "reads");
    Add(counters, "read_bytes", size);
    Add(counters, hit ? "hits" : "misses");
    Add(counters, hit ? "hit_bytes" : "miss_bytes", size);
    if (hit) {
        return;
    }

    Add(counters, "allocs");
    Add(counters, reserved ? "alloc_ok" : "alloc_fail");
    if (reserved) {
        Add(counters, "alloc_bytes", size);
        Add(counters, "alloc_reserved_bytes", reserved);
    } else {
        Add(counters, "fail_bytes", size);
    }
}

} // namespace

// Initialization and shutdown

Manager::Manager(const Json &config, bool detailedStats) : detailedStats_(detailedStats)
{
    const auto highWater = config.at("max_water_level").get<uint16_t>();
    const auto lowWater = config.at("min_water_level").get<uint16_t>();
    const auto leaseTtlMs = config.at("lease_ttl_ms").get<uint32_t>();
    const auto lruShards = config.at("lru_shards").get<uint32_t>();
    const auto policy = config.at("eviction_policy").get<std::string>();

    // Note: stdout carries JSON replies, so native diagnostics belong on stderr
    MmcOutLogger::Instance().SetLogLevel(FATAL_LEVEL);
    MmcOutLogger::Instance().SetExternalLogFunction(
        [](int level, const char *message) {
            if (level == FATAL_LEVEL) {
                std::cerr << message << '\n';
            }
        },
        true);

    manager_ = MmcMakeRef<MmcMetaManager>(leaseTtlMs, highWater, lowWater, MmcMetaExtConfig{}, 0, 0, lruShards,
                                          policy == "lru" ? MetaLruType::LRU : MetaLruType::SHARDED_LRU);
    Check(manager_->Start(), "Start");

    // Note: Native removal callbacks can run on the manager's own threads
    MmcMetaChangeCallbacks callbacks;
    callbacks.removed = [this](const std::string &, uint32_t, uint16_t) { ++evictedKeys_; };
    manager_->SetChangeCallbacks(callbacks);

    uint32_t rank = 0;
    for (const auto &group : config.at("pools")) {
        const auto count = group.at("count").get<uint32_t>();
        const auto capacity = group.at("capacity_bytes").get<uint64_t>();
        for (uint32_t index = 0; index < count; ++index) {
            std::vector<std::pair<std::string, MmcMemBlobDesc>> blobs;
            Check(manager_->Mount({rank, MEDIA_DRAM}, {kPoolBase, capacity}, blobs), "Mount");
            poolRanks_.insert(rank++);
        }
    }

    counters_ = AccessCounters();
    counters_["evict_checks"] = uint64_t{0};
}

Manager::~Manager()
{
    Close();
}

void Manager::Close()
{
    if (manager_ != nullptr) {
        manager_->RemoveAll();
        manager_->Stop();
        manager_ = nullptr;
    }
}

// Sequential block access and lease completion

void Manager::Access(const std::string &key, uint64_t size, const std::string &model)
{
    if (key.empty() || size == 0 || size > UINT64_MAX - kPoolBase) {
        throw std::runtime_error("Invalid key/size");
    }

    MmcMemMetaDesc descriptor;
    auto id = NextId();
    auto before = Clock::now();
    auto result = manager_->Get(key, id, nullptr, descriptor);
    RecordTiming("lookup", before);

    if (result == MMC_OK) {
        CompleteLease(key, size, id, descriptor, false, before);
        RecordAccess(model, size, true, 0);
        return;
    }
    if (result != MMC_UNMATCHED_KEY) {
        Check(result, "Get");
    }

    id = NextId();
    before = Clock::now();
    // Note: No preferred ranks lets native random placement use every pool
    result = manager_->Alloc(key, {size, kBlobCopies, MEDIA_DRAM, {}, ALLOC_RANDOM}, id, 0, descriptor);
    RecordTiming("alloc", before);

    uint64_t reserved = 0;
    if (result == MMC_OK) {
        CompleteLease(key, size, id, descriptor, true, before);
        reserved = descriptor.blobs_.front().allocSize_;
    } else if (result != MMC_ERROR) {
        Check(result, "Alloc");
    }

    RecordAccess(model, size, false, reserved);
    RecordAllocation(size, reserved, result);
    if (result != MMC_OK) {
        return;
    }

    // Check eviction once, after completing the new block's write lease
    const auto evictionStart = detailedStats_ ? Clock::now() : Clock::time_point{};
    manager_->CheckAndEvict(MEDIA_DRAM, 0);
    Add(counters_, "evict_checks");
    WaitForCompletion();
    RecordTiming("evict_check", evictionStart);
}

uint64_t Manager::NextId()
{
    if (issuedIds_++ >= UINT32_MAX) {
        throw std::runtime_error("Operation ID budget exhausted");
    }
    return GenerateOperateId(kClientRank);
}

void Manager::CompleteLease(const std::string &key, uint64_t size, uint64_t id, const MmcMemMetaDesc &descriptor,
                            bool write, Clock::time_point before)
{
    // Note: A successful lookup alone does not guarantee a readable block
    if (descriptor.numBlobs_ != kBlobCopies || descriptor.blobs_.size() != kBlobCopies) {
        throw std::runtime_error("Invalid blob count");
    }

    const auto &blob = descriptor.blobs_.front();
    if (blob.size_ != size || blob.allocSize_ < size || blob.mediaType_ != MEDIA_DRAM ||
        !poolRanks_.count(blob.rank_) || blob.state_ != (write ? ALLOCATED : READABLE)) {
        throw std::runtime_error("Invalid native descriptor");
    }
    if (Clock::now() - before >= std::chrono::milliseconds(blob.leaseTimeoutTtlMs_)) {
        throw std::runtime_error("Lease validity uncertain");
    }

    // Complete the lease using the original operation ID and returned location
    const auto start = detailedStats_ ? Clock::now() : Clock::time_point{};
    Check(manager_->UpdateState(key, {blob.rank_, MEDIA_DRAM}, write ? MMC_WRITE_OK : MMC_READ_FINISH, id),
          "UpdateState");
    RecordTiming(write ? "write_finish" : "read_finish", start);
}

// Native completion and snapshots

void Manager::WaitForCompletion()
{
    const auto deadline = Clock::now() + kCompletionTimeout;
    // Note: The eviction task can finish before its removal callbacks complete
    while (manager_->IsEvictRunning() ||
           counters_.at("alloc_ok").get<uint64_t>() != CachedKeyCount() + evictedKeys_.load()) {
        if (Clock::now() >= deadline) {
            throw std::runtime_error("Native cleanup timed out");
        }
        std::this_thread::sleep_for(kCompletionPoll);
    }
}

size_t Manager::CachedKeyCount() const
{
    size_t count = 0;
    Check(manager_->GetKeyCount(count), "GetKeyCount");
    return count;
}

Json Manager::Progress() const
{
    auto progress = counters_;
    progress["cached_keys"] = CachedKeyCount();
    progress["evicted"] = evictedKeys_.load();
    return progress;
}

Json Manager::Snapshot()
{
    WaitForCompletion();

    Json pools = Json::array();
    for (const auto &pool : manager_->GetAllSegmentInfo()) {
        const uint64_t capacity = pool.at("capacity");
        const uint64_t used = pool.at("allocatedSize");
        const uint64_t padding = pool.at("fragmentationBytes");
        if (padding > used || used > capacity) {
            throw std::runtime_error("Invalid pool snapshot");
        }
        pools.push_back({{"rank", pool.at("rank")},
                         {"capacity_bytes", capacity},
                         {"used_bytes", used},
                         {"padding_bytes", padding}});
    }

    return {{"progress", Progress()},
            {"pools", pools},
            {"details", detailedStats_
                            ? Json{{"prefixes", prefixes_}, {"allocations", allocations_}, {"latencies", latencies_}}
                            : Json(nullptr)}};
}

// Counters and optional operation timings

void Manager::RecordTiming(const std::string &name, Clock::time_point before)
{
    if (!detailedStats_) {
        return;
    }

    const auto duration = Clock::now() - before;
    const auto nanoseconds =
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count());
    auto &row = latencies_[name];
    if (row.is_null()) {
        row = Json::object();
    }
    Add(row, "count");
    Add(row, "total_ns", nanoseconds);
    row["min_ns"] = std::min(row.value("min_ns", nanoseconds), nanoseconds);
    row["max_ns"] = std::max(row.value("max_ns", nanoseconds), nanoseconds);
}

void Manager::RecordAccess(const std::string &model, uint64_t size, bool hit, uint64_t reserved)
{
    CountAccess(counters_, size, hit, reserved);
    if (!detailedStats_) {
        return;
    }

    auto &row = prefixes_[model];
    if (row.is_null()) {
        row = AccessCounters();
    }
    CountAccess(row, size, hit, reserved);
}

void Manager::RecordAllocation(uint64_t size, uint64_t reserved, Result code)
{
    if (!detailedStats_) {
        return;
    }

    auto &row = allocations_[std::to_string(size)];
    if (row.is_null()) {
        row = {{"allocs", 0u},        {"alloc_ok", 0u},    {"alloc_fail", 0u},
               {"request_bytes", 0u}, {"alloc_bytes", 0u}, {"alloc_reserved_bytes", 0u},
               {"fail_bytes", 0u}};
    }
    Add(row, "allocs");
    Add(row, "request_bytes", size);
    Add(row, code == MMC_OK ? "alloc_ok" : "alloc_fail");
    if (code == MMC_OK) {
        Add(row, "alloc_bytes", size);
        Add(row, "alloc_reserved_bytes", reserved);
    } else {
        Add(row, "fail_bytes", size);
    }
}

} // namespace ock::mmc
