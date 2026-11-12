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
#include "mmc_locality_distribution.h"

#include <algorithm>
#include <sstream>
#include <unordered_map>

#include "mmc_logger.h"

namespace ock {
namespace mmc {

namespace {
// Find the backendId in targets; if absent, append a zero-weight entry and return its reference.
// The reference remains valid only until the next push_back; use AddWeight for repeated modifications of the same list.
BackendTarget &LocateOrInsertTarget(std::vector<BackendTarget> &targets, const std::string &backendId)
{
    for (auto &target : targets) {
        if (target.backendId_ == backendId) {
            return target;
        }
    }
    targets.emplace_back(BackendTarget{backendId, 0});
    return targets.back();
}

// Add delta (possibly negative) to the backendId weight in targets; insert (backendId, +delta) if absent.
// Idempotent internally; retains no references across calls. Use for repeated modifications of the same list.
void AddWeight(std::vector<BackendTarget> &targets, const std::string &backendId, int32_t delta)
{
    for (auto &target : targets) {
        if (target.backendId_ == backendId) {
            target.weight_ = static_cast<uint32_t>(static_cast<int32_t>(target.weight_) + delta);
            return;
        }
    }
    if (delta > 0) {
        targets.emplace_back(BackendTarget{backendId, static_cast<uint32_t>(delta)});
    }
}
} // namespace

std::vector<BackendDistribution> BuildBackendDistribution(const std::vector<std::string> &allBackendIds,
                                                          const std::vector<std::string> &writeBackendIds,
                                                          const std::vector<std::string> &memoryBackendIds)
{
    const size_t writerCount = writeBackendIds.size();
    const size_t totalCount = allBackendIds.size();
    const uint32_t perBackendShare = static_cast<uint32_t>(writerCount); // Total shares for each backendId
    // Writer quota = total shares / writer count = n*(n+m)/n = n+m shares, the backendId count (always integral).
    const uint32_t quota = static_cast<uint32_t>(allBackendIds.size());

    std::vector<BackendDistribution> table;
    if (writerCount == 0) {
        return table;
    }
    table.reserve(writerCount);

    // Step 1: greedy ledger. accountBook[i] holds the remaining shares for allBackendIds[i].
    // Each writer starts at its own backendId (local first), then takes remaining shares in circular allBackendIds
    // order.
    std::vector<uint32_t> accountBook(totalCount, perBackendShare);
    std::unordered_map<std::string, size_t> backendIndex;
    for (size_t i = 0; i < allBackendIds.size(); ++i) {
        backendIndex[allBackendIds[i]] = i;
    }
    for (const auto &writeId : writeBackendIds) {
        BackendDistribution dist;
        dist.writerBackendId_ = writeId;
        uint32_t need = quota;
        const size_t selfIdx = (backendIndex.find(writeId) != backendIndex.end()) ? backendIndex[writeId] : 0;
        // Scan order: self -> all memory nodes (in allBackendIds order) -> remaining backends (circular fallback).
        // Prefer memory nodes for remote shares to avoid sending writes to other writers (P nodes).
        // With a normal configuration, the memory ledger total (m * n) equals all writers' remote demand (n * m),
        // so fallback is defensive and does not actually consume other writers' shares.
        std::vector<size_t> scanOrder;
        scanOrder.reserve(totalCount);
        scanOrder.push_back(selfIdx);
        for (const auto &memId : memoryBackendIds) {
            const auto it = backendIndex.find(memId);
            if (it != backendIndex.end()) {
                scanOrder.push_back(it->second);
            }
        }
        for (size_t k = 1; k < totalCount; ++k) {
            const size_t idx = (selfIdx + k) % totalCount;
            if (std::find(scanOrder.begin(), scanOrder.end(), idx) == scanOrder.end()) {
                scanOrder.push_back(idx);
            }
        }
        for (const size_t idx : scanOrder) {
            if (need == 0) {
                break;
            }
            if (accountBook[idx] == 0) {
                continue;
            }
            const uint32_t take = std::min(need, accountBook[idx]);
            accountBook[idx] -= take;
            LocateOrInsertTarget(dist.targets_, allBackendIds[idx]).weight_ += take;
            need -= take;
        }
        table.emplace_back(std::move(dist));
    }

    // Step 2 (optional): local-affinity exchange to maximize each writer's local writes.
    // For each writer whose highest-weight target is remote, find the other writer holding the most local-backend
    // shares.
    // Swap equal shares (local shares for maxTarget shares), preserving total consumption for each backendId.
    // Swapping can push_back into targets_ and invalidate references/pointers; use indices and repeat lookups.
    for (size_t i = 0; i < table.size(); ++i) {
        BackendDistribution &dist = table[i];
        const std::string localId = dist.writerBackendId_;
        // Find the highest-weight entry by index to avoid dangling pointers.
        int maxIdx = -1;
        for (size_t j = 0; j < dist.targets_.size(); ++j) {
            if (maxIdx == -1 || dist.targets_[j].weight_ > dist.targets_[maxIdx].weight_) {
                maxIdx = static_cast<int>(j);
            }
        }
        if (maxIdx == -1 || dist.targets_[maxIdx].weight_ == 0 || dist.targets_[maxIdx].backendId_ == localId) {
            continue;
        }
        const std::string maxBackendId = dist.targets_[maxIdx].backendId_;
        const uint32_t maxWeight = dist.targets_[maxIdx].weight_;
        // Find the other writer holding the most localId shares.
        size_t bestOtherIdx = SIZE_MAX;
        uint32_t bestHeld = 0;
        for (size_t k = 0; k < table.size(); ++k) {
            if (k == i) {
                continue;
            }
            for (auto &target : table[k].targets_) {
                if (target.backendId_ == localId && target.weight_ > bestHeld) {
                    bestHeld = target.weight_;
                    bestOtherIdx = k;
                }
            }
        }
        if (bestOtherIdx == SIZE_MAX || bestHeld == 0) {
            continue;
        }
        const uint32_t exchange = std::min(maxWeight, bestHeld);
        // Writer dist: local +exchange, maxTarget -exchange (AddWeight is idempotent and avoids reference
        // invalidation).
        AddWeight(dist.targets_, localId, static_cast<int32_t>(exchange));
        AddWeight(dist.targets_, maxBackendId, -static_cast<int32_t>(exchange));
        // Other writer: local(localId) -exchange, maxBackendId +exchange.
        BackendDistribution &other = table[bestOtherIdx];
        AddWeight(other.targets_, localId, -static_cast<int32_t>(exchange));
        AddWeight(other.targets_, maxBackendId, static_cast<int32_t>(exchange));
        // Remove zero-weight entries to keep the list clean.
        auto eraseZero = [](std::vector<BackendTarget> &targets) {
            targets.erase(
                std::remove_if(targets.begin(), targets.end(), [](const BackendTarget &t) { return t.weight_ == 0; }),
                targets.end());
        };
        eraseZero(dist.targets_);
        eraseZero(other.targets_);
    }
    // print the distribution table to check the validity of the distribution
    LogBackendDistributionTable(table);
    return table;
}

void LogBackendDistributionTable(const std::vector<BackendDistribution> &table)
{
    for (const auto &dist : table) {
        // Do not name the local stream oss: MMC_LOG_INFO declares oss internally and would shadow it,
        // causing the macro to use its own unfinished message and corrupt the log output.
        std::ostringstream message;
        message << "ALLOC_LOCAL_FIRST: dist writer[" << dist.writerBackendId_ << "] -> {";
        for (size_t j = 0; j < dist.targets_.size(); ++j) {
            if (j > 0) {
                message << ", ";
            }
            message << dist.targets_[j].backendId_ << ":" << dist.targets_[j].weight_;
        }
        message << "}";
        MMC_LOG_INFO(message.str());
    }
}

const BackendTarget *PickTargetByWeight(const BackendDistribution &dist, std::mt19937 &rng)
{
    if (dist.targets_.empty()) {
        return nullptr;
    }
    uint32_t totalWeight = 0;
    for (const auto &target : dist.targets_) {
        totalWeight += target.weight_;
    }
    if (totalWeight == 0) {
        return nullptr;
    }
    std::uniform_int_distribution<uint32_t> pick(0, totalWeight - 1);
    uint32_t roll = pick(rng);
    for (const auto &target : dist.targets_) {
        if (roll < target.weight_) {
            return &target;
        }
        roll -= target.weight_;
    }
    return &dist.targets_.back();
}

} // namespace mmc
} // namespace ock
