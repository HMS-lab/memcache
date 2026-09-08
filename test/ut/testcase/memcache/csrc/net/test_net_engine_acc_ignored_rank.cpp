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
#include <mutex>

#include "gtest/gtest.h"
#include "mmc_common_includes.h"

#define private public
#include "smem_acc_api.h"
#undef private

// 先按正常方式包含 acc_links 头文件，避免 private->public 宏干扰其内部实现。
#include "mmc_acc_runtime.h"

#define private public
#include "mmc_net_engine_acc.h"
#include "mmc_net_link_acc.h"
#undef private

using namespace testing;
using namespace std;
using namespace ock::mmc;

/* 最小可实例化的 TcpLink，仅用于 HandleNewLink / HandleLinkBroken 的单元测试。 */
class TestTcpLink final : public ock::acc::AccTcpLinkComplex {
public:
    struct State {
        uint32_t id;
        uint64_t context;
    };

    explicit TestTcpLink(uint32_t id) : AccTcpLinkComplex(&state_, false), state_{id, 0} {}

private:
    State state_;
};

class TestNetEngineAccIgnoredRank : public testing::Test {
public:
    void SetUp() override
    {
        originalId_ = MFAccApi::gSmemAccLinkId;
        originalSetContext_ = MFAccApi::gSmemAccLinkSetContext;
        originalGetContext_ = MFAccApi::gSmemAccLinkGetContext;
        originalRelease_ = MFAccApi::gSmemAccLinkRelease;
        MFAccApi::gSmemAccLinkId = [](smem_acc_link_t link) { return static_cast<TestTcpLink::State *>(link)->id; };
        MFAccApi::gSmemAccLinkSetContext = [](smem_acc_link_t link, uint64_t context) {
            static_cast<TestTcpLink::State *>(link)->context = context;
        };
        MFAccApi::gSmemAccLinkGetContext = [](smem_acc_link_t link) {
            return static_cast<TestTcpLink::State *>(link)->context;
        };
        MFAccApi::gSmemAccLinkRelease = [](smem_acc_link_t) {};
    }

    void TearDown() override
    {
        MFAccApi::gSmemAccLinkId = originalId_;
        MFAccApi::gSmemAccLinkSetContext = originalSetContext_;
        MFAccApi::gSmemAccLinkGetContext = originalGetContext_;
        MFAccApi::gSmemAccLinkRelease = originalRelease_;
    }

private:
    smemAccLinkIdFunc originalId_{nullptr};
    smemAccLinkSetContextFunc originalSetContext_{nullptr};
    smemAccLinkGetContextFunc originalGetContext_{nullptr};
    smemAccLinkReleaseFunc originalRelease_{nullptr};
};

// 与 mmc_net_engine_acc.cpp 中的 INVALID_RANK_ID 保持一致。
constexpr uint64_t INVALID_RANK_ID_FOR_TEST = UINT64_MAX;

TEST_F(TestNetEngineAccIgnoredRank, HandleNewLink_InvalidRankId_AddsToIgnoredSet)
{
    NetEngineAcc engine;
    TcpConnReq req{};
    req.rankId = INVALID_RANK_ID_FOR_TEST;
    TcpLinkPtr link(new TestTcpLink(42));

    auto ret = engine.HandleNewLink(req, link);
    EXPECT_EQ(ret, MMC_OK);
    EXPECT_EQ(engine.ignoredRankLinkIds_.count(42), 1U);
}

TEST_F(TestNetEngineAccIgnoredRank, HandleNewLink_ValidRankId_AddsToPeerLinkMap)
{
    NetEngineAcc engine;
    engine.peerLinkMap_ = MmcMakeRef<NetLinkMapAcc>();
    TcpConnReq req{};
    req.rankId = 7;
    TcpLinkPtr link(new TestTcpLink(43));

    auto ret = engine.HandleNewLink(req, link);
    EXPECT_EQ(ret, MMC_OK);
    EXPECT_EQ(engine.ignoredRankLinkIds_.count(43), 0U);

    NetLinkAccPtr found;
    EXPECT_TRUE(engine.peerLinkMap_->Find(7, found));
    EXPECT_NE(found.Get(), nullptr);
}

TEST_F(TestNetEngineAccIgnoredRank, HandleLinkBroken_IgnoredLink_ErasesFromSetAndSkipsHandler)
{
    NetEngineAcc engine;
    engine.ignoredRankLinkIds_.insert(42);

    bool brokenHandlerCalled = false;
    engine.RegLinkBrokenHandler([&brokenHandlerCalled](const NetLinkPtr &) -> int32_t {
        brokenHandlerCalled = true;
        return MMC_OK;
    });

    TcpLinkPtr link(new TestTcpLink(42));

    auto ret = engine.HandleLinkBroken(link);
    EXPECT_EQ(ret, MMC_OK);
    EXPECT_EQ(engine.ignoredRankLinkIds_.count(42), 0U);
    EXPECT_FALSE(brokenHandlerCalled);
}
