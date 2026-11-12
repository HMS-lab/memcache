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

// BuildBackendDistribution test cases.
//
// Share model (see mmc_locality_distribution.h): n = writer count, m = memory count.
// Each backendId has n total shares; each writer has quota n + m. Therefore:
// * All writers consume exactly n shares from each backendId; under equal traffic, each machine receives 1/(n+m) of
// writes.
// * Theoretical local affinity per writer is at most n/(n+m); the greedy ledger plus one local exchange may achieve
// less.
//
// Coverage:
// 1. Real topologies: 2P+4D (6 machines), 4P+4D (8 machines: 2 P groups of 2 machines + 1 D group of 4), 5P+3D (8
// machines),
// 5P+1D, 2P+3D, 1P+3D (5 P processes on one machine + 3 D machines).
// 2. Structural invariants: quota conservation, per-backend total conservation, no zero weights/duplicate targets,
// determinism (1..12 topology matrix).
// 3. Edge/invalid inputs: no writers, no memory, empty allBackendIds, more writers than nodes, unregistered writer
// backendId,
// duplicate backendIds/writeBackendIds, sensitivity to allBackendIds order, memoryBackendIds not used for routing.
// 4. PickTargetByWeight: weight ratios, determinism with the same seed, nullptr for unusable tables.
// 5. Distribution log contents (regression for oss shadowing inside MMC_LOG_INFO corrupting output).
// 6. Fanout (target backendIds per writer): known topologies, theoretical lower bound, growth with 2P,
// greedy overflow to other P nodes, and sampled target sets. Current algorithm = greedy ledger + one local-affinity
// exchange.
// The exchange can increase fanout for local shares (for example, writer4 reaches 4 targets in 5P+3D). If changed to
// filling n local shares
// then overflowing only to D, update both fanout and cross-P expectations.
namespace {

// Weights are shares; each weight equals the number of shares.
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

// Target count in one writer distribution (nonzero; intentionally distinct in meaning from share constants).
constexpr size_t kOneTarget = 1;
constexpr size_t kTwoTargets = 2;
constexpr size_t kThreeTargets = 3;
constexpr size_t kFourTargets = 4;
constexpr size_t kFiveTargets = 5;
constexpr size_t kOneEntry = 1;

// Topology matrix size.
constexpr size_t kMinWriterCount = 1;
constexpr size_t kMaxWriterCount = 12;
constexpr size_t kMinMemoryCount = 0;
constexpr size_t kMaxMemoryCount = 12;

// PickTargetByWeight sampling parameters.
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

// Construct topology as meta_service does: sorted writers first, then sorted memory nodes.
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

// Fanout: backendIds reached by a writer. No duplicate targets, so this equals targets_ length.
size_t Fanout(const BackendDistribution &dist)
{
    return dist.targets_.size();
}

// Cross-writer weight: shares targeting other writers' backendIds (only P->P traffic, not all cross-node traffic).
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

// Target order has no semantic meaning (only shares per backendId matter); sort entries before comparing.
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

// Structural invariants:
// * Entry count equals writer count, in writeBackendIds order.
// * Each writer's weights sum to quota n+m.
// * No zero weights or duplicate targets per writer; every target belongs to allBackendIds.
// * Total consumption per backendId equals n.
// * Each writer's own backendId receives positive shares, at most the per-backend total n.
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
        // Table construction logs at INFO; the matrix builds every 1..12 topology, so use WARN in UT to avoid flooding
        // logs.
        MmcOutLogger::Instance().SetLogLevel(WARN_LEVEL);
    }

    void TearDown() override
    {
        MmcOutLogger::Instance().SetLogLevel(originalLevel_);
    }

protected:
    LogLevel originalLevel_ = INFO_LEVEL;
};

// 2P + 4D (6 machines, backendIds 1-6): each machine receives exactly 2 shares; writers have only 2/6 = 33.3% local
// affinity.
// The other 4 shares are split equally over 2 D nodes; each D serves only one writer.
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
    // Local-affinity upper bound n/(n+m) = 2/6; the implementation reaches it exactly.
    for (const auto &dist : table) {
        EXPECT_EQ(TargetWeight(dist, dist.writerBackendId_), kTwoShares);
        EXPECT_EQ(dist.targets_.size(), kThreeTargets);
    }
}

// 8 backendIds (1-8), 4 writers + 4 memory nodes (2 P groups of 2 machines, 1 D group of 4):
// Each writer is exactly 50% local + 50% on a single D; each of the 4 D nodes exclusively serves one writer.
// When n == m, each writer always has 2 targets. If its D fails, 50% of routing attempts miss until the table is
// rebuilt.
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

// 2P + 3D: D3 is shared by both writers (1 share each); each writer has 3 targets.
TEST_F(MmcLocalityDistributionTest, TwoWritersThreeMemoryNodesShareTheMiddleMemoryNode)
{
    const auto topology = MakeTopology(kTwoWriters, kThreeMemoryNodes);
    const auto table = BuildBackendDistribution(topology.allIds, topology.writerIds, topology.memoryIds);

    ExpectTableRows(table, {"1:2, 3:2, 4:1", "2:2, 4:1, 5:2"});
    ExpectTableInvariants(topology, table);
    // Writer 1 exclusively uses memory 3; only writer 2 uses 5; memory 4 provides 1 share to each.
    EXPECT_EQ(TargetWeight(table[0], "3"), kTwoShares);
    EXPECT_EQ(TargetWeight(table[0], "4"), kOneShare);
    EXPECT_EQ(TargetWeight(table[1], "4"), kOneShare);
    EXPECT_EQ(TargetWeight(table[1], "5"), kTwoShares);
}

// 5P + 3D (8 backendIds, writers 1-5, memory 6-8): remote shares prefer memory,
// so writers are also balanced: all have 5/8 local affinity, and remote targets are memory only, never other writers.
TEST_F(MmcLocalityDistributionTest, FiveWritersThreeMemoryNodesLocalAffinityIsEven)
{
    const auto topology = MakeTopology(kFiveWriters, kThreeMemoryNodes);
    const auto table = BuildBackendDistribution(topology.allIds, topology.writerIds, topology.memoryIds);

    ExpectTableRows(table, {"1:5, 6:3", "2:5, 6:2, 7:1", "3:5, 7:3", "4:5, 7:1, 8:2", "5:5, 8:3"});
    ExpectTableInvariants(topology, table);

    for (size_t i = 0; i < table.size(); ++i) {
        SCOPED_TRACE("writer=" + table[i].writerBackendId_);
        // Each writer has writerCount local shares; remote shares target memory only.
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

// 5P + 1D (6 backendIds): each writer fills 5 local shares, then puts the remaining share on the sole D node.
// Local affinity is uniformly 5/6, no longer varying with time/scan order.
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
    // Each of the 5 writers takes 1 share from D6, totaling exactly 5.
    EXPECT_EQ(TargetWeight(table[0], "6") + TargetWeight(table[1], "6") + TargetWeight(table[2], "6") +
                  TargetWeight(table[3], "6") + TargetWeight(table[4], "6"),
              kFiveShares);
}

// 1 writer + 3 memory nodes (for example, 5 P processes on one machine + 3 D machines): quota 4 = 1 local + 1 per D.
// Local affinity is only 1/4, but the four nodes split traffic equally.
TEST_F(MmcLocalityDistributionTest, SingleWriterSpreadsOverLocalAndMemory)
{
    const auto topology = MakeTopology(kOneWriter, kThreeMemoryNodes);
    const auto table = BuildBackendDistribution(topology.allIds, topology.writerIds, topology.memoryIds);

    ExpectTableRows(table, {"1:1, 2:1, 3:1, 4:1"});
    ExpectTableInvariants(topology, table);
    EXPECT_EQ(TargetWeight(table[0], "1"), kOneShare);
}

// No writers: return an empty table, without per-writer empty distributions or out-of-bounds access.
TEST_F(MmcLocalityDistributionTest, NoWriterProducesEmptyTable)
{
    const auto topology = MakeTopology(0, kThreeMemoryNodes);
    EXPECT_TRUE(BuildBackendDistribution(topology.allIds, topology.writerIds, topology.memoryIds).empty());

    std::mt19937 rng(kPickSeed);
    BackendDistribution empty;
    EXPECT_EQ(PickTargetByWeight(empty, rng), nullptr);
}

// No memory nodes: each writer writes only locally (100% local).
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

// Empty allBackendIds (missing topology): each writer receives an empty target list; no crash or modulo by zero.
// SelectTargetBackendForRank receives nullptr and falls back to ALLOC_ARRANGE.
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

// Writer backendId absent from allBackendIds: cursor falls back to 0; quota is still filled without local affinity.
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

// Production pitfall: BmRegister defaults backendId to empty. Nodes without explicit backendId all merge into one ""
// backendId, weighted as one machine; GetRanksForBackendId("") returns all unnamed ranks.
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

    // If the empty-string writer owns all nodes (no memory role), every node is 100% local.
    const auto localOnly = BuildBackendDistribution(allIds, allIds, {});
    ASSERT_EQ(localOnly.size(), allIds.size());
    for (size_t i = 0; i < localOnly.size(); ++i) {
        SCOPED_TRACE("backend=" + allIds[i]);
        EXPECT_EQ(localOnly[i].targets_.size(), kOneTarget);
        EXPECT_EQ(TargetWeight(localOnly[i], allIds[i]), kThreeShares);
    }
}

// memoryBackendIds describes roles only; targets must come from allBackendIds.
// A D machine listed only in memoryBackendIds, without inclusion in allBackendIds, is never selected.
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

    // Remote targets appear only when memory nodes are included in allBackendIds.
    const auto withMemoryInAll = BuildBackendDistribution({"1", "2", "3", "4"}, writerIds, {"3", "4"});
    ExpectTableRows(withMemoryInAll, {"1:2, 3:2", "4:2, 2:2"});
}

// Invalid input: duplicate writeBackendIds (possible if manually supplied when one backendId registers multiple ranks).
// The second same-name writer exchanges away the first writer's local shares, leaving the first with zero local shares.
// This shows step 2 performs only one exchange pass and does not guarantee local shares for every writer.
// Production backendIdRoleMap_ is keyed by backendId and naturally deduplicates, preventing this input.
TEST_F(MmcLocalityDistributionTest, DuplicateWriterBackendIdCanLoseLocalShare)
{
    const auto table = BuildBackendDistribution({"1", "2", "3"}, {"1", "1"}, {"2", "3"});

    ExpectTableRows(table, {"2:1, 3:2", "2:1, 1:2"});
    EXPECT_EQ(TargetWeight(table[0], "1"), 0U);
    EXPECT_EQ(TargetWeight(table[1], "1"), kTwoShares);
    // Totals remain conserved: backendId 1 has 2 shares = writer count.
    const auto load = PerBackendLoad(table);
    EXPECT_EQ(load.at("1"), static_cast<uint32_t>(kTwoWriters));
    EXPECT_EQ(load.at("2"), static_cast<uint32_t>(kTwoWriters));
    EXPECT_EQ(load.at("3"), static_cast<uint32_t>(kTwoWriters));
}

// Invalid input: duplicate backendId in allBackendIds (misuse when writer/memory roles overlap).
// The ledger counts entries rather than IDs, doubling consumption for this backendId (2n instead of n),
// while preserving each writer's quota. This test locks in that behavior; callers must ensure unique IDs in
// allBackendIds.
TEST_F(MmcLocalityDistributionTest, DuplicatedBackendIdIsCountedTwice)
{
    const auto table = BuildBackendDistribution({"1", "1", "2"}, {"1"}, {"2"});

    ExpectTableRows(table, {"1:2, 2:1"});
    const auto load = PerBackendLoad(table);
    EXPECT_EQ(load.at("1"), kTwoShares);
    EXPECT_EQ(load.at("2"), kOneShare);
}

// More writers than nodes (intermediate state from differing registration/topology discovery order): all writers target
// the sole node.
// Quota shrinks to 1, violating the n+m shares-per-writer assumption, but avoiding an infinite loop.
TEST_F(MmcLocalityDistributionTest, MoreWritersThanBackendsCollapseToOneTarget)
{
    const std::vector<std::string> writerIds = {"1", "2", "3"};
    const auto table = BuildBackendDistribution({"1"}, writerIds, {});

    ExpectTableRows(table, {"1:1", "1:1", "1:1"});
    const auto load = PerBackendLoad(table);
    EXPECT_EQ(load.at("1"), static_cast<uint32_t>(kThreeWriters));
}

// allBackendIds defines the greedy ledger scan order. The caller (meta_service) sorts strings and supplies
// writers before memory; reordering changes which D nodes each writer receives, although totals remain conserved.
TEST_F(MmcLocalityDistributionTest, TableIgnoresAllBackendIdsOrder)
{
    const std::vector<std::string> writerIds = {"1", "2"};
    const std::vector<std::string> memoryIds = {"3", "4", "5"};

    const auto sortedTable = BuildBackendDistribution({"1", "2", "3", "4", "5"}, writerIds, memoryIds);
    ExpectTableRows(sortedTable, {"1:2, 3:2, 4:1", "2:2, 4:1, 5:2"});

    const auto shuffledTable = BuildBackendDistribution({"5", "1", "3", "2", "4"}, writerIds, memoryIds);
    ExpectTableRows(shuffledTable, {"1:2, 3:2, 4:1", "2:2, 4:1, 5:2"});

    // The new algorithm scans self -> memory list, so allIds ordering no longer changes the result.
    EXPECT_EQ(FormatDistribution(sortedTable[0]), FormatDistribution(shuffledTable[0]));
    const auto sortedLoad = PerBackendLoad(sortedTable);
    const auto shuffledLoad = PerBackendLoad(shuffledTable);
    for (const auto &backendId : memoryIds) {
        EXPECT_EQ(sortedLoad.at(backendId), static_cast<uint32_t>(kTwoWriters));
        EXPECT_EQ(shuffledLoad.at(backendId), static_cast<uint32_t>(kTwoWriters));
    }
}

// Topology matrix: 1..12 writers x 0..12 memory nodes; verify structural invariants and determinism.
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

            // When n == m, each writer has exactly 2 targets: n local shares + n shares from one D.
            if (memoryCount == writerCount) {
                for (const auto &dist : table) {
                    EXPECT_EQ(dist.targets_.size(), kTwoTargets);
                }
            }
        }
    }
}

// Known-topology fanout (targets per writer). The deployed local-affinity exchange increases fanout for some writers:
// 5P+3D: writer4 has 4 targets and writer2/3/5 have 3; pure greedy/theoretical optimum requires only 2-3.
// 8P+11D reaches 5 targets. These figures provide a baseline for algorithm changes.
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

// Theoretical fanout lower bound: each backendId holds at most n shares and writer quota is n+m,
// so each writer needs at least ceil((n+m)/n) targets and at most n+m (at least 1 share per target).
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

// For 2P + m D nodes, fanout = 1 + ceil(m/2): each D has only 2 shares,
// so every 2 additional D machines add one target. This estimates convergence for 2P topologies.
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

// The deployed greedy ledger overflows to adjacent backendIds; allBackendIds lists writers before memory.
// With few D nodes, writers reach other P nodes: in 5P+3D, writer2 sends 3/8 to P3/P4, and writer3/4 send 1/8 each.
// In 5P+1D (quota 6), writer1/2/4 send 1/6, 2/6, 1/6 to the next P, respectively. In 4P+4D this is exactly 0.
// If overflow becomes memory-only, all expectations in this test should become 0, providing a regression anchor.
// Remote shares target memory only: no known topology should write to another writer (P).
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

// Sample the actual reduction: each writer's allocations reach only the backendIds in its table;
// the set size equals fanout, rather than n+m with pure random allocation.
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
        // Reduction must yield fewer targets than the full node set.
        EXPECT_LT(hitBackendIds.size(), topology.allIds.size());
        for (const auto &target : dist.targets_) {
            EXPECT_EQ(hitBackendIds.count(target.backendId_), kOneEntry);
        }
    }
}

// Weighted sampling: writer 4 in 5P+3D has distribution {5:1, 6:2, 7:2, 4:3}; sample ratios must match weights.
// Order starts with local 4, not 5; target-list order is not weight order.
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

// The same RNG seed must yield exactly the same target sequence; distributed nodes depend on this determinism.
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

// Empty table / zero total weight returns nullptr; caller falls back to ALLOC_ARRANGE.
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

// Log regression: MMC_LOG_INFO declares oss internally, shadowing any local oss;
// this historically produced corrupted output combining an unfinished message with itself.
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
