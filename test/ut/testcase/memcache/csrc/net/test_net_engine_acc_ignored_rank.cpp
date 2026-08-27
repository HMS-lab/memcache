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
#include "gtest/gtest.h"

// 先按正常方式包含 acc_links 头文件，避免 private->public 宏干扰其内部实现。
#include "acc_tcp_server.h"

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
    explicit TestTcpLink(uint32_t id) : AccTcpLinkComplex(-1, "127.0.0.1:0", id) {}

    int32_t BlockSend(void *, uint32_t) override
    {
        return 0;
    }
    int32_t BlockRecv(void *, uint32_t) override
    {
        return 0;
    }
    int32_t PollingInput(int32_t) const override
    {
        return 0;
    }
    int32_t SetSendTimeout(uint32_t) const override
    {
        return 0;
    }
    int32_t SetReceiveTimeout(uint32_t) const override
    {
        return 0;
    }
    int32_t EnableNonBlocking() const override
    {
        return 0;
    }
    void Close() override {}
    bool IsConnected() const override
    {
        return true;
    }

    int32_t NonBlockSend(int16_t, const ock::acc::AccDataBufferPtr &, const ock::acc::AccDataBufferPtr &) override
    {
        return 0;
    }
    int32_t NonBlockSend(int16_t, uint32_t, const ock::acc::AccDataBufferPtr &,
                         const ock::acc::AccDataBufferPtr &) override
    {
        return 0;
    }
    int32_t NonBlockSend(int16_t, int16_t, uint32_t, const ock::acc::AccDataBufferPtr &,
                         const ock::acc::AccDataBufferPtr &) override
    {
        return 0;
    }
    int32_t EnqueueAndModifyEpoll(const ock::acc::AccMsgHeader &, const ock::acc::AccDataBufferPtr &,
                                  const ock::acc::AccDataBufferPtr &) override
    {
        return 0;
    }
};

class TestNetEngineAccIgnoredRank : public testing::Test {
public:
    void SetUp() override {}
    void TearDown() override {}
};

// 与 mmc_net_engine_acc.cpp 中的 INVALID_RANK_ID 保持一致。
constexpr uint64_t INVALID_RANK_ID_FOR_TEST = UINT64_MAX;

TEST_F(TestNetEngineAccIgnoredRank, HandleNewLink_InvalidRankId_AddsToIgnoredSet)
{
    NetEngineAcc engine;
    TcpConnReq req{};
    req.rankId = INVALID_RANK_ID_FOR_TEST;
    TcpLinkPtr link(new TestTcpLink(42));

    Result ret = engine.HandleNewLink(req, link);
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

    Result ret = engine.HandleNewLink(req, link);
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

    Result ret = engine.HandleLinkBroken(link);
    EXPECT_EQ(ret, MMC_OK);
    EXPECT_EQ(engine.ignoredRankLinkIds_.count(42), 0U);
    EXPECT_FALSE(brokenHandlerCalled);
}
