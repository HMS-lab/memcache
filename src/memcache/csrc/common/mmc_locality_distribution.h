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
#ifndef MEM_FABRIC_MMC_LOCALITY_DISTRIBUTION_H
#define MEM_FABRIC_MMC_LOCALITY_DISTRIBUTION_H

#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace ock {
namespace mmc {

// Weight entry for one write target.
// Weights are shares; each backendId has writeBackendIds.size() total shares (see BuildBackendDistribution).
// For example, writeId1 -> {backendId1: 100%, backendId2: 33%} becomes {backendId1: 3, backendId2: 1} for 3 writers.
struct BackendTarget {
    std::string backendId_;
    uint32_t weight_ = 0;
};

// Complete write distribution for one writer backendId (sum of targets_ weights = quota = total shares / writers;
// integral).
struct BackendDistribution {
    std::string writerBackendId_;
    std::vector<BackendTarget> targets_;
};

/**
 * Build the mapping from writer backendId to weighted target backendIds using a greedy ledger.
 *
 * Inputs:
 *   allBackendIds     All backendIds (writers union memory), in registration/sorted order; defines the fallback ledger
 * order.
 *                                       Targets can only come from this list; memory nodes omitted here are never
 * selected.
 *   writeBackendIds   Writer backendIds (P side, n writers).
 *   memoryBackendIds  Memory backendIds (D side, m nodes providing space only). Currently describes roles only;
 *                                       neither it nor capacity/rank counts affect ratios. Weights are divided equally
 * by backendId count.
 *
 * Share model (n = writers, m = memory nodes, n + m = backendId count = allBackendIds.size()):
 *   Each backendId has n total shares (= writer count).
 *   Total shares = n * (n + m).
 *   Writer quota = total shares / n = (n + m) shares = backendId count; always integral.
 *
 * Step 1 (uniform greedy): maintain a ledger of remaining shares in allBackendIds order.
 *   Writers sequentially take their full quota (n+m) from the ledger head. This keeps the total
 *   shares consumed from different backendIds balanced, preventing underuse or overuse of individual backends.
 *   Example (n=3,m=1, order [A,B,C,D]): A->{A:3,B:1}, B->{B:2,C:2}, C->{C:1,D:3}.
 *
 * Step 2 (approximate local affinity): visit writers greedily. If the highest-weight target is not local,
 *   exchange shares with another writer holding local-backend shares to maximize the local proportion.
 *   The exchange preserves total consumption for each backendId.
 *   Each writer gets at most one exchange with one other writer in a single pass. Its local weight may still
 *   not be its largest weight, and local-affinity ratios may differ (for example, 37.5% to 62.5% for n=5,m=3).
 *   The theoretical upper bound is n/(n+m) (= local shares / quota).
 */
std::vector<BackendDistribution> BuildBackendDistribution(const std::vector<std::string> &allBackendIds,
                                                          const std::vector<std::string> &writeBackendIds,
                                                          const std::vector<std::string> &memoryBackendIds);

// Select a target backendId by weight; return nullptr for an empty table. rng shuffles groups only (ranks shuffle
// separately).
const BackendTarget *PickTargetByWeight(const BackendDistribution &dist, std::mt19937 &rng);

// Format writer distributions as readable strings and log at INFO to inspect local affinity and weight distribution.
// Each line looks like "writer[writerA] -> {backendX:3, backendY:1}"; empty tables produce no output.
void LogBackendDistributionTable(const std::vector<BackendDistribution> &table);

} // namespace mmc
} // namespace ock

#endif // MEM_FABRIC_MMC_LOCALITY_DISTRIBUTION_H
