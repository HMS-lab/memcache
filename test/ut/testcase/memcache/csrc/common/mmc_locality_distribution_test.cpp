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
#include <cstddef>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "mmc_logger.h"

using namespace ock::mmc;

// BuildBackendDistribution 的用例集合。
//
// 份额模型（见 mmc_locality_distribution.h）：n = 写者数，m = memory 数，
// 每个 backendId 的总份额 = n，每个写者的配额 = n + m，因此：
//   * 每个 backendId 被所有写者消耗的总份额恒为 n，等流量假设下每台机器的写入占比恒为 1/(n+m)；
//   * 单个写者的本地亲和率理论上限 = n/(n+m)，实际值由「贪心账本 + 一次本地亲和置换」决定，可能低于上限。
//
// 用例覆盖：
//   1. 真实拓扑：2P+4D（6 机）、4P+4D（8 机，即 2P 每 P 2 机 + 1D 4 机）、5P+3D（8 机）、
//      5P+1D、2P+3D、1P+3D（5P 同机 + 3D）；
//   2. 结构不变量：配额守恒、单 backendId 总量守恒、无零权重、无重复目标、确定性（1..12 拓扑矩阵）；
//   3. 边界/异常输入：无写者、无 memory、空 allBackendIds、写者数 > 节点数、写者 backendId 未注册、
//      backendId 重复、writeBackendIds 重复、allBackendIds 顺序敏感、memoryBackendIds 参数不参与选路；
//   4. 选择器 PickTargetByWeight 的权重比例、同种子确定性、不可用表返回 nullptr；
//   5. 分布表日志内容（防止 MMC_LOG_INFO 宏内部 oss 变量遮蔽导致日志错乱的问题回归）；
//   6. fanout 维度（每个写者会落到几个 backendId）：已知拓扑的 fanout、理论下界、2P 的增长规律、
//      贪心溢出到其他 P 节点、抽样落点集合。当前算法=「贪心账本 + 一次本地亲和置换」，第二步会为了
//      本地份额把 fanout 抬高（如 5P+3D 的 writer4 到 4 个目标）；若改成「本地 n 份填满 + 溢出只到 D」，
//      fanout 相关期望值和跨 P 期望值都需要同步更新。
namespace {

// 权重以「份额」计，权重值等于份额个数。
constexpr uint32_t kOneShare = 1;
constexpr uint32_t kTwoShares = 2;
constexpr uint32_t kThreeShares = 3;
constexpr uint32_t kFourShares = 4;
constexpr uint32_t kFiveShares = 5;

constexpr size_t kFirstBackendId = 1;
constexpr size_t kOneWriter = 1;
constexpr size_t kTwoWriters = 2;
constexpr size_t kThreeWriters = 3;
constexpr size_t kFourWriters = 4;
constexpr size_t kFiveWriters = 5;
constexpr size_t kOneMemoryNode = 1;
constexpr size_t kTwoMemoryNodes = 2;
constexpr size_t kThreeMemoryNodes = 3;
constexpr size_t kFourMemoryNodes = 4;
constexpr size_t kElevenMemoryNodes = 11;
constexpr size_t kEightWriters = 8;

// 单个写者分布表中的目标个数（不含 0，故意与份额常量区分语义）
constexpr size_t kOneTarget = 1;
constexpr size_t kTwoTargets = 2;
constexpr size_t kThreeTargets = 3;
constexpr size_t kFourTargets = 4;
constexpr size_t kFiveTargets = 5;
constexpr size_t kOneEntry = 1;

// 拓扑矩阵规模
constexpr size_t kMinWriterCount = 1;
constexpr size_t kMaxWriterCount = 12;
constexpr size_t kMinMemoryCount = 0;
constexpr size_t kMaxMemoryCount = 12;

// PickTargetByWeight 抽样检查参数
constexpr uint32_t kPickSampleCount = 200000;
constexpr uint32_t kPickSeed = 42;
constexpr uint32_t kDeterminismPickCount = 1000;
constexpr uint32_t kFanoutSampleCount = 10000;
constexpr double kPickRatioTolerance = 0.01;

struct Topology {
    std::vector<std::string> allIds;
    std::vector<std::string> writerIds;
    std::vector<std::string> memoryIds;
};

std::vector<std::string> MakeBackendIds(size_t count)
{
    std::vector<std::string> ids;
    ids.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        ids.push_back(std::to_string(i + kFirstBackendId));
    }
    return ids;
}

std::vector<std::string> Head(const std::vector<std::string> &ids, size_t count)
{
    return std::vector<std::string>(ids.begin(), ids.begin() + static_cast<std::ptrdiff_t>(count));
}

std::vector<std::string> Tail(const std::vector<std::string> &ids, size_t start)
{
    return std::vector<std::string>(ids.begin() + static_cast<std::ptrdiff_t>(start), ids.end());
}

// 按 meta_service 的装配方式（写者在前、memory 在后、各自有序）构造拓扑。
Topology MakeTopology(size_t writerCount, size_t memoryCount)
{
    Topology topology;
    topology.allIds = MakeBackendIds(writerCount + memoryCount);
    topology.writerIds = Head(topology.allIds, writerCount);
    topology.memoryIds = Tail(topology.allIds, writerCount);
    return topology;
}

std::string FormatDistribution(const BackendDistribution &dist)
{
    std::ostringstream text;
    for (size_t i = 0; i < dist.targets_.size(); ++i) {
        if (i > 0) {
            text << ", ";
        }
        text << dist.targets_[i].backendId_ << ":" << dist.targets_[i].weight_;
    }
    return text.str();
}

uint32_t TotalWeight(const BackendDistribution &dist)
{
    uint32_t total = 0;
    for (const auto &target : dist.targets_) {
        total += target.weight_;
    }
    return total;
}

uint32_t TargetWeight(const BackendDistribution &dist, const std::string &backendId)
{
    for (const auto &target : dist.targets_) {
        if (target.backendId_ == backendId) {
            return target.weight_;
        }
    }
    return 0;
}

std::map<std::string, uint32_t> PerBackendLoad(const std::vector<BackendDistribution> &table)
{
    std::map<std::string, uint32_t> load;
    for (const auto &dist : table) {
        for (const auto &target : dist.targets_) {
            load[target.backendId_] += target.weight_;
        }
    }
    return load;
}

bool ContainsId(const std::vector<std::string> &ids, const std::string &backendId)
{
    return std::find(ids.begin(), ids.end(), backendId) != ids.end();
}

// fanout：该写者会落到的 backendId 个数。目标列表内无重复条目，故等于 targets_ 长度。
size_t Fanout(const BackendDistribution &dist)
{
    return dist.targets_.size();
}

// 跨写者权重：落到「其他写者 backendId」上的份额和（不是所有跨节点流量，只是 P→P 部分）。
uint32_t CrossWriterWeight(const BackendDistribution &dist, const std::vector<std::string> &writerIds)
{
    uint32_t total = 0;
    for (const auto &target : dist.targets_) {
        if (target.backendId_ != dist.writerBackendId_ && ContainsId(writerIds, target.backendId_)) {
            total += target.weight_;
        }
    }
    return total;
}

// 目标列表的顺序不参与语义（表只关心每个 backendId 拿到多少份），比较前统一按条目排序。
std::string NormalizeRow(const std::string &row)
{
    std::vector<std::string> parts;
    std::stringstream ss(row);
    std::string item;
    while (std::getline(ss, item, ',')) {
        const size_t pos = item.find_first_not_of(' ');
        parts.push_back(pos == std::string::npos ? std::string() : item.substr(pos));
    }
    std::sort(parts.begin(), parts.end());
    std::string normalized;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            normalized += ", ";
        }
        normalized += parts[i];
    }
    return normalized;
}

void ExpectTableRows(const std::vector<BackendDistribution> &table, const std::vector<std::string> &expectedRows)
{
    ASSERT_EQ(table.size(), expectedRows.size());
    for (size_t i = 0; i < expectedRows.size(); ++i) {
        SCOPED_TRACE("row " + std::to_string(i));
        EXPECT_EQ(NormalizeRow(FormatDistribution(table[i])), NormalizeRow(expectedRows[i]));
    }
}

// 结构不变量：
//   * 表项数 = 写者数，且顺序与 writeBackendIds 一致；
//   * 每个写者权重和 = 配额 n+m；
//   * 无零权重、同一写者内无重复目标、目标必在 allBackendIds 内；
//   * 每个 backendId 被消耗总量 = n；
//   * 写者自己的 backendId 一定拿到正份额，且不超过单 backendId 总份额 n。
void ExpectTableInvariants(const Topology &topology, const std::vector<BackendDistribution> &table)
{
    const uint32_t writerCount = static_cast<uint32_t>(topology.writerIds.size());
    const uint32_t quota = static_cast<uint32_t>(topology.allIds.size());
    ASSERT_EQ(table.size(), topology.writerIds.size());

    std::map<std::string, uint32_t> perBackendLoad;
    for (size_t i = 0; i < table.size(); ++i) {
        const auto &dist = table[i];
        SCOPED_TRACE("writer=" + dist.writerBackendId_);
        EXPECT_EQ(dist.writerBackendId_, topology.writerIds[i]);
        EXPECT_EQ(TotalWeight(dist), quota);
        std::set<std::string> distinctTargets;
        for (const auto &target : dist.targets_) {
            EXPECT_GT(target.weight_, 0U);
            EXPECT_TRUE(ContainsId(topology.allIds, target.backendId_));
            EXPECT_TRUE(distinctTargets.insert(target.backendId_).second);
            perBackendLoad[target.backendId_] += target.weight_;
        }
        const uint32_t localWeight = TargetWeight(dist, dist.writerBackendId_);
        EXPECT_GT(localWeight, 0U);
        EXPECT_LE(localWeight, writerCount);
    }

    for (const auto &backendId : topology.allIds) {
        SCOPED_TRACE("backend=" + backendId);
        ASSERT_EQ(perBackendLoad.count(backendId), kOneEntry);
        EXPECT_EQ(perBackendLoad[backendId], writerCount);
    }
}

} // namespace

class MmcLocalityDistributionTest : public testing::Test {
public:
    void SetUp() override
    {
        originalLevel_ = MmcOutLogger::Instance().GetLogLevel();
        // 构建分布表会打印 INFO 日志；矩阵用例会给 1..12 的每个拓扑建表，UT 中降到 WARN 以免刷屏。
        MmcOutLogger::Instance().SetLogLevel(WARN_LEVEL);
    }

    void TearDown() override
    {
        MmcOutLogger::Instance().SetLogLevel(originalLevel_);
    }

protected:
    LogLevel originalLevel_ = INFO_LEVEL;
};

// 2P + 4D（6 台机，backendId 1-6）：每台机器恰好收 2 份，但写者只有 2/6 = 33.3% 落在本地，
// 其余 4 份平摊到 2 个 D；每个 D 只服务一个写者。
TEST_F(MmcLocalityDistributionTest, TwoWritersFourMemoryNodesStayEvenlyLoaded)
{
    const auto topology = MakeTopology(kTwoWriters, kFourMemoryNodes);
    const auto table = BuildBackendDistribution(topology.allIds, topology.writerIds, topology.memoryIds);

    ExpectTableRows(table, {"1:2, 3:2, 4:2", "5:2, 6:2, 2:2"});
    ExpectTableInvariants(topology, table);

    const auto load = PerBackendLoad(table);
    for (const auto &backendId : topology.allIds) {
        EXPECT_EQ(load.at(backendId), static_cast<uint32_t>(kTwoWriters));
    }
    // 本地亲和率上限 n/(n+m) = 2/6，实现正好命中上限。
    for (const auto &dist : table) {
        EXPECT_EQ(TargetWeight(dist, dist.writerBackendId_), kTwoShares);
        EXPECT_EQ(dist.targets_.size(), kThreeTargets);
    }
}

// 8 个 backendId（1-8），4 个写者 + 4 个 memory（2P 每个 P 2 机、1D 4 机）：
// 每个写者恰好 50% 本地 + 50% 落到唯一一个 D，且 4 个 D 各被一个写者独占。
// 注意：n == m 时每个写者的目标恒为 2 个 —— 一旦这个 D 故障，该写者当次有 50% 选路落空（等待重建分布表）。
TEST_F(MmcLocalityDistributionTest, FourWritersFourMemoryNodesPinOneMemoryNodeEach)
{
    const auto topology = MakeTopology(kFourWriters, kFourMemoryNodes);
    const auto table = BuildBackendDistribution(topology.allIds, topology.writerIds, topology.memoryIds);

    ExpectTableRows(table, {"1:4, 5:4", "2:4, 6:4", "3:4, 7:4", "4:4, 8:4"});
    ExpectTableInvariants(topology, table);

    std::map<std::string, uint32_t> rowsPerMemory;
    for (const auto &dist : table) {
        EXPECT_EQ(TargetWeight(dist, dist.writerBackendId_), kFourShares);
        EXPECT_EQ(dist.targets_.size(), kTwoTargets);
        for (const auto &target : dist.targets_) {
            if (ContainsId(topology.memoryIds, target.backendId_)) {
                ++rowsPerMemory[target.backendId_];
            }
        }
    }
    for (const auto &memoryId : topology.memoryIds) {
        EXPECT_EQ(rowsPerMemory[memoryId], kOneEntry);
    }
}

// 2P + 3D：D3 被两个写者分摊（各 1 份），写者各有 3 个目标。
TEST_F(MmcLocalityDistributionTest, TwoWritersThreeMemoryNodesShareTheMiddleMemoryNode)
{
    const auto topology = MakeTopology(kTwoWriters, kThreeMemoryNodes);
    const auto table = BuildBackendDistribution(topology.allIds, topology.writerIds, topology.memoryIds);

    ExpectTableRows(table, {"1:2, 3:2, 4:1", "2:2, 4:1, 5:2"});
    ExpectTableInvariants(topology, table);
    // 写者 1 独占 memory 3，写者 2 才用到 5，memory 4 被两者各分 1 份。
    EXPECT_EQ(TargetWeight(table[0], "3"), kTwoShares);
    EXPECT_EQ(TargetWeight(table[0], "4"), kOneShare);
    EXPECT_EQ(TargetWeight(table[1], "4"), kOneShare);
    EXPECT_EQ(TargetWeight(table[1], "5"), kTwoShares);
}

// 5P + 3D（8 个 backendId，1-5 为写者、6-8 为 memory）：远端份额优先落到 memory，
// 因此写者维度也均衡：本地亲和率统一为 5/8，远端目标只落在 memory、不再跨到别的写者。
TEST_F(MmcLocalityDistributionTest, FiveWritersThreeMemoryNodesLocalAffinityIsEven)
{
    const auto topology = MakeTopology(kFiveWriters, kThreeMemoryNodes);
    const auto table = BuildBackendDistribution(topology.allIds, topology.writerIds, topology.memoryIds);

    ExpectTableRows(table, {"1:5, 6:3", "2:5, 6:2, 7:1", "3:5, 7:3", "4:5, 7:1, 8:2", "5:5, 8:3"});
    ExpectTableInvariants(topology, table);

    for (size_t i = 0; i < table.size(); ++i) {
        SCOPED_TRACE("writer=" + table[i].writerBackendId_);
        // 每个写者本地份额都等于 writerCount，远端只落在 memory。
        EXPECT_EQ(TargetWeight(table[i], table[i].writerBackendId_), kFiveShares);
        for (const auto &target : table[i].targets_) {
            if (target.backendId_ != table[i].writerBackendId_) {
                EXPECT_NE(topology.memoryIds.end(),
                          std::find(topology.memoryIds.begin(), topology.memoryIds.end(), target.backendId_))
                    << "cross-writer target " << target.backendId_ << " for writer " << table[i].writerBackendId_;
            }
        }
    }
}

// 5P + 1D（6 个 backendId）：每个写者先吃满本地 5 份，剩下 1 份都落到唯一的 D，
// 本地亲和率统一为 5/6，不再随时间/扫描顺序波动。
TEST_F(MmcLocalityDistributionTest, FiveWritersOneMemoryNodeLocalAffinityIsEven)
{
    const auto topology = MakeTopology(kFiveWriters, kOneMemoryNode);
    const auto table = BuildBackendDistribution(topology.allIds, topology.writerIds, topology.memoryIds);

    ExpectTableRows(table, {"1:5, 6:1", "2:5, 6:1", "3:5, 6:1", "4:5, 6:1", "5:5, 6:1"});
    ExpectTableInvariants(topology, table);

    for (size_t i = 0; i < table.size(); ++i) {
        SCOPED_TRACE("writer=" + table[i].writerBackendId_);
        EXPECT_EQ(TargetWeight(table[i], table[i].writerBackendId_), kFiveShares);
        EXPECT_EQ(TargetWeight(table[i], "6"), kOneShare);
    }
    // D6 被 5 个写者各分 1 份，合计恰好 5 份。
    EXPECT_EQ(TargetWeight(table[0], "6") + TargetWeight(table[1], "6") + TargetWeight(table[2], "6") +
                  TargetWeight(table[3], "6") + TargetWeight(table[4], "6"),
              kFiveShares);
}

// 1 个写者 + 3 个 memory（例如 5P 同机 + 3 台 D 机）：配额 4 = 本地 1 + 每个 D 1，
// 写者无法体现本地亲和（只有 1/4），但四个节点严格均分。
TEST_F(MmcLocalityDistributionTest, SingleWriterSpreadsOverLocalAndMemory)
{
    const auto topology = MakeTopology(kOneWriter, kThreeMemoryNodes);
    const auto table = BuildBackendDistribution(topology.allIds, topology.writerIds, topology.memoryIds);

    ExpectTableRows(table, {"1:1, 2:1, 3:1, 4:1"});
    ExpectTableInvariants(topology, table);
    EXPECT_EQ(TargetWeight(table[0], "1"), kOneShare);
}

// 没有写者时返回空表（不能返回每个写者一条空分布，也不能越界）。
TEST_F(MmcLocalityDistributionTest, NoWriterProducesEmptyTable)
{
    const auto topology = MakeTopology(0, kThreeMemoryNodes);
    EXPECT_TRUE(BuildBackendDistribution(topology.allIds, topology.writerIds, topology.memoryIds).empty());

    std::mt19937 rng(kPickSeed);
    BackendDistribution empty;
    EXPECT_EQ(PickTargetByWeight(empty, rng), nullptr);
}

// 没有 memory 节点时每个写者只写本地（100% 本地）。
TEST_F(MmcLocalityDistributionTest, NoMemoryNodeKeepsWritesLocal)
{
    const auto topology = MakeTopology(kTwoWriters, 0);
    const auto table = BuildBackendDistribution(topology.allIds, topology.writerIds, topology.memoryIds);

    ExpectTableRows(table, {"1:2", "2:2"});
    ExpectTableInvariants(topology, table);
    for (const auto &dist : table) {
        EXPECT_EQ(dist.targets_.size(), kOneTarget);
        EXPECT_EQ(TargetWeight(dist, dist.writerBackendId_), TotalWeight(dist));
    }
}

// allBackendIds 为空（拓扑信息缺失）：每个写者拿到空目标表，不崩、不做 0 取模；
// 调用方 SelectTargetBackendForRank 会因 nullptr 回退到 ALLOC_ARRANGE。
TEST_F(MmcLocalityDistributionTest, EmptyAllBackendIdsProducesEmptyTargets)
{
    const std::vector<std::string> writerIds = {"1", "2"};
    const auto table = BuildBackendDistribution({}, writerIds, {});

    ASSERT_EQ(table.size(), writerIds.size());
    for (size_t i = 0; i < table.size(); ++i) {
        EXPECT_EQ(table[i].writerBackendId_, writerIds[i]);
        EXPECT_TRUE(table[i].targets_.empty());
        EXPECT_EQ(TotalWeight(table[i]), 0U);
    }
    std::mt19937 rng(kPickSeed);
    EXPECT_EQ(PickTargetByWeight(table[0], rng), nullptr);
}

// 写者 backendId 不在 allBackendIds 中：cursor 回退到 0，仍能分完配额，只是没有本地亲和。
TEST_F(MmcLocalityDistributionTest, UnregisteredWriterBackendIdStartsFromFirstEntry)
{
    const std::vector<std::string> allIds = {"9", "10"};
    const std::vector<std::string> writerIds = {"1", "2"};
    const auto table = BuildBackendDistribution(allIds, writerIds, allIds);

    ExpectTableRows(table, {"9:2", "10:2"});
    const auto load = PerBackendLoad(table);
    EXPECT_EQ(load.at("9"), static_cast<uint32_t>(kTwoWriters));
    EXPECT_EQ(load.at("10"), static_cast<uint32_t>(kTwoWriters));
    for (const auto &dist : table) {
        EXPECT_EQ(TargetWeight(dist, dist.writerBackendId_), 0U);
    }
}

// 生产陷阱：BmRegister 的 backendId 默认是空串。未显式传 backendId 的节点会全部聚成一个 ""
// backendId，在分布表里仍只算「一台机器」的权重；GetRanksForBackendId("") 会返回所有未命名 rank。
TEST_F(MmcLocalityDistributionTest, EmptyBackendIdIsTreatedAsSingleNode)
{
    const std::vector<std::string> allIds = {"", "1", "2"};
    const std::vector<std::string> writerIds = {""};
    const auto table = BuildBackendDistribution(allIds, writerIds, {"1", "2"});

    ASSERT_EQ(table.size(), kOneWriter);
    EXPECT_EQ(TargetWeight(table[0], ""), kOneShare);
    EXPECT_EQ(TargetWeight(table[0], "1"), kOneShare);
    EXPECT_EQ(TargetWeight(table[0], "2"), kOneShare);
    EXPECT_EQ(TotalWeight(table[0]), kThreeShares);

    // 若空串写者独占全部节点（无 memory 角色），则每个节点 100% 本地。
    const auto localOnly = BuildBackendDistribution(allIds, allIds, {});
    ASSERT_EQ(localOnly.size(), allIds.size());
    for (size_t i = 0; i < localOnly.size(); ++i) {
        SCOPED_TRACE("backend=" + allIds[i]);
        EXPECT_EQ(localOnly[i].targets_.size(), kOneTarget);
        EXPECT_EQ(TargetWeight(localOnly[i], allIds[i]), kThreeShares);
    }
}

// memoryBackendIds 只用于描述角色，不参与选路：目标只能来自 allBackendIds。
// 只把它传进 memoryBackendIds、不并入 allBackendIds，D 机器永远不会被选中。
TEST_F(MmcLocalityDistributionTest, MemoryBackendIdListIsIgnoredForTargets)
{
    const std::vector<std::string> writerIds = {"1", "2"};
    const auto withoutMemory = BuildBackendDistribution({"1", "2"}, writerIds, {});
    const auto withMemoryList = BuildBackendDistribution({"1", "2"}, writerIds, {"3", "4"});

    ASSERT_EQ(withMemoryList.size(), withoutMemory.size());
    for (size_t i = 0; i < withMemoryList.size(); ++i) {
        EXPECT_EQ(FormatDistribution(withMemoryList[i]), FormatDistribution(withoutMemory[i]));
        EXPECT_EQ(TargetWeight(withMemoryList[i], "3"), 0U);
        EXPECT_EQ(TargetWeight(withMemoryList[i], "4"), 0U);
    }

    // 只有把 memory 并入 allBackendIds，才会产生远端目标。
    const auto withMemoryInAll = BuildBackendDistribution({"1", "2", "3", "4"}, writerIds, {"3", "4"});
    ExpectTableRows(withMemoryInAll, {"1:2, 3:2", "4:2, 2:2"});
}

// 异常输入：writeBackendIds 重复（同一 backendId 注册了多个 rank 时可能人为传入）。
// 第二个同名写者会从第一个写者手里换走本地份额，导致第一个写者最终 0 本地份额 ——
// 说明第二步只做单趟置换、且不保证每个写者都保留本地份额。
// 生产路径上 backendIdRoleMap_ 以 backendId 为 key，天然去重，不会出现该输入。
TEST_F(MmcLocalityDistributionTest, DuplicateWriterBackendIdCanLoseLocalShare)
{
    const auto table = BuildBackendDistribution({"1", "2", "3"}, {"1", "1"}, {"2", "3"});

    ExpectTableRows(table, {"2:1, 3:2", "2:1, 1:2"});
    EXPECT_EQ(TargetWeight(table[0], "1"), 0U);
    EXPECT_EQ(TargetWeight(table[1], "1"), kTwoShares);
    // 总量仍守恒：backendId 1 共 2 份 = 写者数。
    const auto load = PerBackendLoad(table);
    EXPECT_EQ(load.at("1"), static_cast<uint32_t>(kTwoWriters));
    EXPECT_EQ(load.at("2"), static_cast<uint32_t>(kTwoWriters));
    EXPECT_EQ(load.at("3"), static_cast<uint32_t>(kTwoWriters));
}

// 异常输入：allBackendIds 里同一 backendId 出现两次（写者与 memory 角色重叠时的误用）。
// 账本按条目而非按 id 计数，该 backendId 被消耗的份额会翻倍（2n 而不是 n），
// 但每个写者的配额仍守恒。用例锁定该行为，调用方必须保证 allBackendIds 内 id 唯一。
TEST_F(MmcLocalityDistributionTest, DuplicatedBackendIdIsCountedTwice)
{
    const auto table = BuildBackendDistribution({"1", "1", "2"}, {"1"}, {"2"});

    ExpectTableRows(table, {"1:2, 2:1"});
    const auto load = PerBackendLoad(table);
    EXPECT_EQ(load.at("1"), kTwoShares);
    EXPECT_EQ(load.at("2"), kOneShare);
}

// 写者数大于节点数（注册顺序与拓扑发现顺序不一致时的中间态）：所有写者都指向唯一节点，
// 配额被迫收缩为 1，不再满足「每写者 n+m 份」的假设，但不会死循环。
TEST_F(MmcLocalityDistributionTest, MoreWritersThanBackendsCollapseToOneTarget)
{
    const std::vector<std::string> writerIds = {"1", "2", "3"};
    const auto table = BuildBackendDistribution({"1"}, writerIds, {});

    ExpectTableRows(table, {"1:1", "1:1", "1:1"});
    const auto load = PerBackendLoad(table);
    EXPECT_EQ(load.at("1"), static_cast<uint32_t>(kThreeWriters));
}

// allBackendIds 的顺序即贪心账本的扫描顺序，调用方（meta_service）按字符串排序后
// 「写者在前、memory 在后」传入；顺序变化会改变每个写者分到哪些 D，虽然总量守恒。
TEST_F(MmcLocalityDistributionTest, TableIgnoresAllBackendIdsOrder)
{
    const std::vector<std::string> writerIds = {"1", "2"};
    const std::vector<std::string> memoryIds = {"3", "4", "5"};

    const auto sortedTable = BuildBackendDistribution({"1", "2", "3", "4", "5"}, writerIds, memoryIds);
    ExpectTableRows(sortedTable, {"1:2, 3:2, 4:1", "2:2, 4:1, 5:2"});

    const auto shuffledTable = BuildBackendDistribution({"5", "1", "3", "2", "4"}, writerIds, memoryIds);
    ExpectTableRows(shuffledTable, {"1:2, 3:2, 4:1", "2:2, 4:1, 5:2"});

    // 新算法按「自己 → memory 列表」扫描，allIds 的排列不再影响结果。
    EXPECT_EQ(FormatDistribution(sortedTable[0]), FormatDistribution(shuffledTable[0]));
    const auto sortedLoad = PerBackendLoad(sortedTable);
    const auto shuffledLoad = PerBackendLoad(shuffledTable);
    for (const auto &backendId : memoryIds) {
        EXPECT_EQ(sortedLoad.at(backendId), static_cast<uint32_t>(kTwoWriters));
        EXPECT_EQ(shuffledLoad.at(backendId), static_cast<uint32_t>(kTwoWriters));
    }
}

// 1..12 写者 × 0..12 memory 的拓扑矩阵：验证结构不变量与结果确定性。
TEST_F(MmcLocalityDistributionTest, TopologyMatrixSatisfiesInvariants)
{
    for (size_t writerCount = kMinWriterCount; writerCount <= kMaxWriterCount; ++writerCount) {
        for (size_t memoryCount = kMinMemoryCount; memoryCount <= kMaxMemoryCount; ++memoryCount) {
            const auto topology = MakeTopology(writerCount, memoryCount);
            SCOPED_TRACE("writers=" + std::to_string(writerCount) + " memory=" + std::to_string(memoryCount));
            const auto table = BuildBackendDistribution(topology.allIds, topology.writerIds, topology.memoryIds);
            ExpectTableInvariants(topology, table);

            const auto repeated = BuildBackendDistribution(topology.allIds, topology.writerIds, topology.memoryIds);
            ASSERT_EQ(repeated.size(), table.size());
            for (size_t i = 0; i < table.size(); ++i) {
                EXPECT_EQ(FormatDistribution(repeated[i]), FormatDistribution(table[i]));
            }

            // n == m 时每个写者恰好 2 个目标：本地 n 份 + 唯一一个 D 的 n 份。
            if (memoryCount == writerCount) {
                for (const auto &dist : table) {
                    EXPECT_EQ(dist.targets_.size(), kTwoTargets);
                }
            }
        }
    }
}

// 已知拓扑的 fanout（每写者目标数）。现网「本地亲和置换」会把部分写者的 fanout 抬高：
// 5P+3D 的 writer4 到 4 个、writer2/3/5 到 3 个，而纯贪心/理论最优只要 2~3 个。
// 8P+11D 则到 5 个。这些数字是算法改动时的对照基线。
TEST_F(MmcLocalityDistributionTest, FanoutIsPinnedForKnownTopologies)
{
    struct FanoutCase {
        size_t writerCount;
        size_t memoryCount;
        std::vector<size_t> expectedFanout;
    };
    const std::vector<FanoutCase> cases = {
        {kOneWriter, kThreeMemoryNodes, {kFourTargets}},
        {kTwoWriters, 0, {kOneTarget, kOneTarget}},
        {kTwoWriters, kThreeMemoryNodes, {kThreeTargets, kThreeTargets}},
        {kTwoWriters, kFourMemoryNodes, {kThreeTargets, kThreeTargets}},
        {kThreeWriters, kTwoMemoryNodes, {kTwoTargets, kThreeTargets, kTwoTargets}},
        {kFourWriters, kThreeMemoryNodes, {kTwoTargets, kThreeTargets, kThreeTargets, kTwoTargets}},
        {kFourWriters, kFourMemoryNodes, {kTwoTargets, kTwoTargets, kTwoTargets, kTwoTargets}},
        {kFiveWriters, kOneMemoryNode, {kTwoTargets, kTwoTargets, kTwoTargets, kTwoTargets, kTwoTargets}},
        {kFiveWriters, kThreeMemoryNodes, {kTwoTargets, kThreeTargets, kTwoTargets, kThreeTargets, kTwoTargets}},
        {kEightWriters,
         kElevenMemoryNodes,
         {kThreeTargets, kThreeTargets, kFourTargets, kThreeTargets, kThreeTargets, kFourTargets, kThreeTargets,
          kThreeTargets}},
    };

    for (const auto &oneCase : cases) {
        const auto topology = MakeTopology(oneCase.writerCount, oneCase.memoryCount);
        SCOPED_TRACE("writers=" + std::to_string(oneCase.writerCount) +
                     " memory=" + std::to_string(oneCase.memoryCount));
        const auto table = BuildBackendDistribution(topology.allIds, topology.writerIds, topology.memoryIds);
        ASSERT_EQ(table.size(), oneCase.expectedFanout.size());
        for (size_t i = 0; i < table.size(); ++i) {
            EXPECT_EQ(Fanout(table[i]), oneCase.expectedFanout[i]);
        }
    }
}

// fanout 的理论下界：单 backendId 最多承载 n 份份额，写者配额 n+m，
// 故每个写者至少 ceil((n+m)/n) 个目标，至多 n+m 个（每个目标至少 1 份）。
TEST_F(MmcLocalityDistributionTest, FanoutMeetsTheoreticalLowerBound)
{
    for (size_t writerCount = kMinWriterCount; writerCount <= kMaxWriterCount; ++writerCount) {
        for (size_t memoryCount = kMinMemoryCount; memoryCount <= kMaxMemoryCount; ++memoryCount) {
            const auto topology = MakeTopology(writerCount, memoryCount);
            SCOPED_TRACE("writers=" + std::to_string(writerCount) + " memory=" + std::to_string(memoryCount));
            const auto table = BuildBackendDistribution(topology.allIds, topology.writerIds, topology.memoryIds);
            const size_t quota = topology.allIds.size();
            const size_t lowerBound = (quota + writerCount - kOneEntry) / writerCount;
            for (const auto &dist : table) {
                EXPECT_GE(Fanout(dist), lowerBound);
                EXPECT_LE(Fanout(dist), quota);
            }
        }
    }
}

// 2P + m 个 D 时 fanout = 1 + ceil(m/2)：每个 D 只有 2 份份额，
// 每多 2 台 D 才多一个目标。这条规律可用来预估 2P 场景的收敛程度。
TEST_F(MmcLocalityDistributionTest, TwoWriterFanoutGrowsOnePerTwoMemoryNodes)
{
    for (size_t memoryCount = kMinMemoryCount; memoryCount <= kMaxMemoryCount; ++memoryCount) {
        const auto topology = MakeTopology(kTwoWriters, memoryCount);
        SCOPED_TRACE("memory=" + std::to_string(memoryCount));
        const auto table = BuildBackendDistribution(topology.allIds, topology.writerIds, topology.memoryIds);
        const size_t expectedFanout = kOneTarget + (memoryCount + kOneEntry) / kTwoTargets;
        for (const auto &dist : table) {
            EXPECT_EQ(Fanout(dist), expectedFanout);
        }
    }
}

// 现网贪心账本会溢出到「相邻」的 backendId，而 allBackendIds 是写者在前、memory 在后，
// 所以 D 少的时候写者会落到别的 P 节点：5P+3D 中 writer2 有 3/8 写到 P3/P4，writer3/4 各 1/8；
// 5P+1D（配额 6）中 writer1/2/4 分别有 1/6、2/6、1/6 写到下一个 P。4P+4D 恰好是 0。
// 若后续改成「溢出只打 memory」，本用例的期望值应全部变成 0，可作为该改动的回归锚点。
// 远端份额只落在 memory：任何已知拓扑都不应出现「写到别的 writer(P)」的情况。
TEST_F(MmcLocalityDistributionTest, NoCrossWriterSpillForKnownTopologies)
{
    const std::vector<std::pair<size_t, size_t>> topologies = {{kFiveWriters, kThreeMemoryNodes},
                                                               {kFiveWriters, kOneMemoryNode},
                                                               {kFourWriters, kFourMemoryNodes},
                                                               {kThreeWriters, kTwoMemoryNodes},
                                                               {kEightWriters, kElevenMemoryNodes}};
    for (const auto &one : topologies) {
        const auto topology = MakeTopology(one.first, one.second);
        SCOPED_TRACE("writers=" + std::to_string(one.first) + " memory=" + std::to_string(one.second));
        const auto table = BuildBackendDistribution(topology.allIds, topology.writerIds, topology.memoryIds);
        ASSERT_EQ(table.size(), one.first);
        for (const auto &dist : table) {
            EXPECT_EQ(CrossWriterWeight(dist, topology.writerIds), 0U);
        }
    }
}

// 抽样验证「收缩」的实际效果：一个写者的分配请求最终只会落到它表里的那几个 backendId，
// 集合大小等于 fanout（不是纯随机时的 n+m 个）。
TEST_F(MmcLocalityDistributionTest, SampledTargetSetMatchesTableFanout)
{
    const auto topology = MakeTopology(kFiveWriters, kThreeMemoryNodes);
    const auto table = BuildBackendDistribution(topology.allIds, topology.writerIds, topology.memoryIds);
    ASSERT_EQ(table.size(), kFiveWriters);

    std::mt19937 rng(kPickSeed);
    for (const auto &dist : table) {
        SCOPED_TRACE("writer=" + dist.writerBackendId_);
        std::set<std::string> hitBackendIds;
        for (uint32_t i = 0; i < kFanoutSampleCount; ++i) {
            const BackendTarget *target = PickTargetByWeight(dist, rng);
            ASSERT_NE(target, nullptr);
            hitBackendIds.insert(target->backendId_);
        }
        EXPECT_EQ(hitBackendIds.size(), Fanout(dist));
        EXPECT_LT(hitBackendIds.size(), topology.allIds.size()); // 收缩后必然少于全量节点数
        for (const auto &target : dist.targets_) {
            EXPECT_EQ(hitBackendIds.count(target.backendId_), kOneEntry);
        }
    }
}

// 权重抽样：5P+3D 中写者 4 的分布为 {5:1, 6:2, 7:2, 4:3}，抽样比例应与权重比例一致；
// 注意顺序为「先本地 4，不是 5」——目标列表顺序不是权重顺序。
TEST_F(MmcLocalityDistributionTest, PickTargetByWeightMatchesWeightRatio)
{
    const auto topology = MakeTopology(kFiveWriters, kThreeMemoryNodes);
    const auto table = BuildBackendDistribution(topology.allIds, topology.writerIds, topology.memoryIds);
    ASSERT_EQ(table.size(), kFiveWriters);
    ASSERT_EQ(FormatDistribution(table[3]), "4:5, 7:1, 8:2");

    std::mt19937 rng(kPickSeed);
    std::map<std::string, uint32_t> hits;
    for (uint32_t i = 0; i < kPickSampleCount; ++i) {
        const BackendTarget *target = PickTargetByWeight(table[3], rng);
        ASSERT_NE(target, nullptr);
        ++hits[target->backendId_];
    }

    const double quota = static_cast<double>(TotalWeight(table[3]));
    for (const auto &target : table[3].targets_) {
        SCOPED_TRACE("target=" + target.backendId_);
        const double expectedRatio = static_cast<double>(target.weight_) / quota;
        const double actualRatio = static_cast<double>(hits[target.backendId_]) / static_cast<double>(kPickSampleCount);
        EXPECT_NEAR(actualRatio, expectedRatio, kPickRatioTolerance);
    }
}

// 同一 rng 种子必须产生完全相同的目标序列（分布式节点之间依赖该确定性）。
TEST_F(MmcLocalityDistributionTest, PickTargetByWeightIsDeterministicForSameSeed)
{
    const auto topology = MakeTopology(kFiveWriters, kThreeMemoryNodes);
    const auto table = BuildBackendDistribution(topology.allIds, topology.writerIds, topology.memoryIds);
    ASSERT_FALSE(table.empty());

    std::mt19937 firstRng(kPickSeed);
    std::mt19937 secondRng(kPickSeed);
    for (uint32_t i = 0; i < kDeterminismPickCount; ++i) {
        const BackendTarget *first = PickTargetByWeight(table[2], firstRng);
        const BackendTarget *second = PickTargetByWeight(table[2], secondRng);
        ASSERT_NE(first, nullptr);
        ASSERT_NE(second, nullptr);
        EXPECT_EQ(first->backendId_, second->backendId_);
    }
}

// 空表 / 总权重为 0 时返回 nullptr，调用方回退到 ALLOC_ARRANGE。
TEST_F(MmcLocalityDistributionTest, PickTargetByWeightReturnsNullForUnusableDistribution)
{
    std::mt19937 rng(kPickSeed);

    BackendDistribution empty;
    EXPECT_EQ(PickTargetByWeight(empty, rng), nullptr);

    BackendDistribution zeroWeight;
    zeroWeight.writerBackendId_ = "1";
    zeroWeight.targets_ = {{"2", 0}};
    EXPECT_EQ(PickTargetByWeight(zeroWeight, rng), nullptr);

    BackendDistribution singleTarget;
    singleTarget.writerBackendId_ = "1";
    singleTarget.targets_ = {{"2", kThreeShares}};
    const BackendTarget *target = PickTargetByWeight(singleTarget, rng);
    ASSERT_NE(target, nullptr);
    EXPECT_EQ(target->backendId_, "2");
}

// 日志内容回归：MMC_LOG_INFO 宏内部声明了 oss，若这里也用 oss 会被遮蔽，
// 日志会变成「半成品消息 + 自己的消息」的错乱输出（历史缺陷）。
TEST_F(MmcLocalityDistributionTest, LogBackendDistributionTablePrintsTargets)
{
    ASSERT_EQ(MmcOutLogger::Instance().SetLogLevel(INFO_LEVEL), 0);

    const std::vector<BackendDistribution> table = {{"1", {{"1", kTwoShares}, {"3", kTwoShares}}}};
    testing::internal::CaptureStdout();
    LogBackendDistributionTable(table);
    const std::string output = testing::internal::GetCapturedStdout();
    EXPECT_NE(output.find("dist writer[1] -> {1:2, 3:2}"), std::string::npos);

    testing::internal::CaptureStdout();
    const std::vector<BackendDistribution> emptyTable;
    LogBackendDistributionTable(emptyTable);
    EXPECT_TRUE(testing::internal::GetCapturedStdout().empty());
}
