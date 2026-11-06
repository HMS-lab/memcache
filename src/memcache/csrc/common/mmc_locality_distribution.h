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

// 单个写入目标的权重条目：
// 权重以"份额"计，每个 backendId 总份额 = writeBackendIds.size()（见 BuildBackendDistribution 注释）。
// 例如 writeId1 -> {backendId1: 100%, backendId2: 33%} 展开为份额 {backendId1: 3, backendId2: 1}（写者数=3）。
struct BackendTarget {
    std::string backendId_;
    uint32_t weight_ = 0;
};

// 一个写者 backendId 的完整写入分布（targets_ 权重之和 = 配额 = 总份额/写者数，恒为整数）
struct BackendDistribution {
    std::string writerBackendId_;
    std::vector<BackendTarget> targets_;
};

/**
 * 构建「写者 backendId → 加权目标 backendId 列表」的映射表（贪心账本算法）。
 *
 * 输入：
 *   allBackendIds     全部 backendId（写者 ∪ memory），按注册/排序顺序传入，即为贪心兜底的账本顺序。
 *                     目标 backendId 只可能来自这里；memory 节点若未并入，永远不会被选中。
 *   writeBackendIds   写者 backendId（P 侧，n 个）。
 *   memoryBackendIds  memory backendId（D 侧，m 个，仅提供空间）。当前实现只把它当作角色描述，
 *                     不参与配比（容量/rank 数也不参与），权重按 backendId 个数均分。
 *
 * 份额模型（n = 写者数，m = memory 节点数，n + m = backendId 总数 = allBackendIds.size()）：
 *   每个 backendId 的总份额 = n 份（= 写者数）；
 *   总份额 = n * (n + m)；
 *   每个写者的配额 = 总份额 / n = (n + m) 份 = backendId 总数 → 恒为整数。
 *
 * 第一步（均匀贪心）：按 allBackendIds 顺序维护各 backendId 的剩余份额账本，
 *   写者依次从账本头部连续取满自己的配额 (n+m) 份。效果：不同 backendId 被消耗的份额
 *   总量均匀，"一个 backendId 分太少、另一个分太多"被约束住。
 *   例（n=3,m=1,顺序[A,B,C,D]）：A->{A:3,B:1}，B->{B:2,C:2}，C->{C:1,D:3}。
 *
 * 第二步（本地亲和，近似）：贪心遍历每个写者，若其权重最大的目标不是本地 backendId，
 *   则与「持有本地 backendId 份额」的其他写者做份额置换，尽量让写者自己的占比最大；
 *   置换保持各 backendId 被消耗总量守恒。
 *   注意：每个写者只做一趟、至多一次置换，且一次只与一个写者交换，因此置换后本地权重仍可能
 *   不是该写者的最大权重，不同写者的本地亲和率也可能不相等（例如 n=5,m=3 时为 37.5%~62.5%）。
 *   理论上限是 n/(n+m)（= 本地份额 / 配额）。
 */
std::vector<BackendDistribution> BuildBackendDistribution(const std::vector<std::string> &allBackendIds,
                                                          const std::vector<std::string> &writeBackendIds,
                                                          const std::vector<std::string> &memoryBackendIds);

// 按权重随机选一个目标 backendId；空表返回 nullptr。rng 仅用于打散组间顺序（组内 ranks 另行 shuffle）。
const BackendTarget *PickTargetByWeight(const BackendDistribution &dist, std::mt19937 &rng);

// 将写者分布表格式化为可读字符串并打印 INFO 日志，便于核对本地亲和与权重分布。
// 每行形如 "writer[writerA] -> {backendX:3, backendY:1}"；空表不打印。
void LogBackendDistributionTable(const std::vector<BackendDistribution> &table);

} // namespace mmc
} // namespace ock

#endif // MEM_FABRIC_MMC_LOCALITY_DISTRIBUTION_H
