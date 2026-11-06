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
// 在 targets 中找到指定 backendId 的条目；不存在则追加零权重条目，返回其引用。
// 注意：返回的引用仅在调用者当前不再次 push_back 的情况下有效；需要多次修改同一列表时请改用 AddWeight。
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

// 对 targets 中指定 backendId 的条目累加 delta（可为负）；条目不存在则按 (backendId, +delta) 插入。
// 内部幂等、无跨调用引用，供需要多次修改同一列表的场景使用。
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
    const uint32_t perBackendShare = static_cast<uint32_t>(writerCount); // 每个 backendId 总份额
    // 每个写者的配额 = 总份额 / 写者数 = n*(n+m)/n = n+m 份，即 backendId 总数（恒为整数）
    const uint32_t quota = static_cast<uint32_t>(allBackendIds.size());

    std::vector<BackendDistribution> table;
    if (writerCount == 0) {
        return table;
    }
    table.reserve(writerCount);

    // 第一步：贪心账本。accountBook[i] = allBackendIds[i] 的剩余份额。
    // 每个写者从「自己的 backendId」位置开始消耗（本地优先），不够再按 allBackendIds 顺序向后扩散取份额（环形）。
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
        // 扫描顺序：自己 → 所有 memory（按 allBackendIds 顺序）→ 其余 backend（环形兜底）。
        // 目的：让「远端份额」优先落到 memory 节点，避免写请求跨到别的 writer(P) 上。
        // 正常配置下 memory 的账本总量 (m * n) 恰好等于所有 writer 的远端需求 (n * m)，
        // 因此兜底段只是防御，不会真正消耗其他 writer 的份额。
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

    // 第二步（选做）：本地亲和交换 —— 让每个写者尽量多写自己。
    // 遍历每个写者，若权重最大的目标不是本地，则在其余写者中找「持有本地 backendId 份额最多」者，
    // 做等量置换（自己获得本地份额，对方获得 maxTarget 份额），保持各 backendId 被消耗总量守恒。
    // 注意：swap 过程中 targets_ 会 push_back，可能触发 vector 扩容使引用/指针失效，故一律按索引+重新查找操作。
    for (size_t i = 0; i < table.size(); ++i) {
        BackendDistribution &dist = table[i];
        const std::string localId = dist.writerBackendId_;
        // 找权重最大条目（按索引，避免悬垂指针）
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
        // 找持有 localId 份额最多的其他写者
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
        // 写者 dist：本地 +exchange，maxTarget -exchange（AddWeight 幂等，无引用失效）
        AddWeight(dist.targets_, localId, static_cast<int32_t>(exchange));
        AddWeight(dist.targets_, maxBackendId, -static_cast<int32_t>(exchange));
        // 对方写者：本地(localId) -exchange，maxBackendId +exchange
        BackendDistribution &other = table[bestOtherIdx];
        AddWeight(other.targets_, localId, -static_cast<int32_t>(exchange));
        AddWeight(other.targets_, maxBackendId, static_cast<int32_t>(exchange));
        // 移除权重归零的条目，保持列表干净
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
        // 注意：局部流变量不能取名为 oss —— MMC_LOG_INFO 宏内部也声明了 oss，会遮蔽本变量，
        // 使宏里拼串时取到自己的半成品消息，最终日志内容错乱。
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
