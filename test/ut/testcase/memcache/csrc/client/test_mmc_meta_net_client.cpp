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

#define private public
#include "mmc_meta_net_client.h"
#undef private

using namespace testing;
using namespace std;
using namespace ock::mmc;

/* ── Minimal fake NetEngine for testing HandleLinkBroken / Stop / Connect ─ */

class FakeNetEngine : public NetEngine {
public:
    Result Start(const NetEngineOptions &options) override
    {
        return MMC_OK;
    }

    void Stop() override
    {
        stopCalled = true;
    }

    Result ConnectToPeer(uint32_t peerId, const std::string &peerIp, uint16_t port, NetLinkPtr &newLink, bool isForce,
                         bool ignoreRankId = false) override
    {
        connectCalled = true;
        lastPeerId = peerId;
        lastPeerIp = peerIp;
        lastPort = port;
        lastForce = isForce;
        lastIgnoreRankId = ignoreRankId;
        return connectResult;
    }

    Result Call(uint32_t targetId, int16_t opCode, const char *reqData, uint32_t reqDataLen, char **respData,
                uint32_t &respDataLen, int32_t timeoutInSecond) override
    {
        return MMC_ERROR;
    }

    Result Send(uint32_t peerId, const char *reqData, uint32_t reqDataLen, int32_t timeoutInSecond) override
    {
        return MMC_ERROR;
    }

    bool stopCalled = false;
    bool connectCalled = false;
    uint32_t lastPeerId = 0;
    std::string lastPeerIp;
    uint16_t lastPort = 0;
    bool lastForce = false;
    bool lastIgnoreRankId = false;
    Result connectResult = MMC_OK;
};

/* ── Existing UpdateServerUrl tests ──────────────────────────────────────── */

class TestMetaNetClient : public testing::Test {
public:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(TestMetaNetClient, UpdateServerUrl_NotStarted_ReturnsError)
{
    MetaNetClient client("tcp://127.0.0.1:5000", "test");
    auto ret = client.UpdateServerUrl("tcp://10.0.0.1:6000");
    EXPECT_EQ(ret, MMC_NOT_STARTED);
}

TEST_F(TestMetaNetClient, UpdateServerUrl_SameUrl_NoChange)
{
    MetaNetClient client("tcp://127.0.0.1:5000", "test");
    client.started_ = true;
    client.ip_ = "127.0.0.1";
    client.port_ = 5000;
    client.serverUrl_ = "tcp://127.0.0.1:5000";

    auto ret = client.UpdateServerUrl("tcp://127.0.0.1:5000");
    EXPECT_EQ(ret, MMC_OK);
    EXPECT_EQ(client.ip_, "127.0.0.1");
    EXPECT_EQ(client.port_, 5000);
    EXPECT_EQ(client.serverUrl_, "tcp://127.0.0.1:5000");
}

TEST_F(TestMetaNetClient, UpdateServerUrl_DifferentIp_UpdatesFields)
{
    MetaNetClient client("tcp://127.0.0.1:5000", "test");
    client.started_ = true;
    client.ip_ = "127.0.0.1";
    client.port_ = 5000;
    client.serverUrl_ = "tcp://127.0.0.1:5000";

    auto ret = client.UpdateServerUrl("tcp://10.0.0.1:6000");
    EXPECT_EQ(ret, MMC_OK);
    EXPECT_EQ(client.ip_, "10.0.0.1");
    EXPECT_EQ(client.port_, 6000);
    EXPECT_EQ(client.serverUrl_, "tcp://10.0.0.1:6000");
}

TEST_F(TestMetaNetClient, UpdateServerUrl_DifferentPort_UpdatesFields)
{
    MetaNetClient client("tcp://127.0.0.1:5000", "test");
    client.started_ = true;
    client.ip_ = "127.0.0.1";
    client.port_ = 5000;
    client.serverUrl_ = "tcp://127.0.0.1:5000";

    auto ret = client.UpdateServerUrl("tcp://127.0.0.1:7000");
    EXPECT_EQ(ret, MMC_OK);
    EXPECT_EQ(client.ip_, "127.0.0.1");
    EXPECT_EQ(client.port_, 7000);
    EXPECT_EQ(client.serverUrl_, "tcp://127.0.0.1:7000");
}

/* ── Connect() ignoreRankId propagation tests ───────────────────────────── */

TEST_F(TestMetaNetClient, Connect_IgnoreRankIdTrue_StoresFlagAndPassesToEngine)
{
    MetaNetClient client("tcp://127.0.0.1:5000", "test");
    auto *rawEngine = new FakeNetEngine();
    NetEnginePtr guard(rawEngine);
    client.engine_ = rawEngine;

    auto ret = client.Connect("tcp://127.0.0.1:5000", true);
    EXPECT_EQ(ret, MMC_OK);
    EXPECT_TRUE(client.ignoreRankId_);
    EXPECT_TRUE(rawEngine->connectCalled);
    EXPECT_TRUE(rawEngine->lastIgnoreRankId);
}

TEST_F(TestMetaNetClient, Connect_Default_IgnoreRankIdFalse)
{
    MetaNetClient client("tcp://127.0.0.1:5000", "test");
    auto *rawEngine = new FakeNetEngine();
    NetEnginePtr guard(rawEngine);
    client.engine_ = rawEngine;

    auto ret = client.Connect("tcp://127.0.0.1:5000");
    EXPECT_EQ(ret, MMC_OK);
    EXPECT_FALSE(client.ignoreRankId_);
    EXPECT_TRUE(rawEngine->connectCalled);
    EXPECT_FALSE(rawEngine->lastIgnoreRankId);
}

/* ── Stop() tests: three-phase mutex approach, stopping_ flag ────────────── */

TEST_F(TestMetaNetClient, Stop_NotStarted_LeavesStoppingFalse)
{
    MetaNetClient client("tcp://127.0.0.1:5000", "test");
    // started_ defaults to false; Stop() should return early without setting stopping_.
    client.Stop();
    EXPECT_FALSE(client.stopping_);
    EXPECT_FALSE(client.started_);
}

TEST_F(TestMetaNetClient, Stop_Started_SetsStoppingAndClearsStarted)
{
    MetaNetClient client("tcp://127.0.0.1:5000", "test");
    client.started_ = true;
    // engine_ is nullptr; Stop() should still set flags and skip engine_->Stop().
    client.Stop();
    EXPECT_TRUE(client.stopping_);
    EXPECT_FALSE(client.started_);
    EXPECT_EQ(client.engine_.Get(), nullptr);
}

TEST_F(TestMetaNetClient, Stop_StartedWithEngine_CallsEngineStopAndClears)
{
    MetaNetClient client("tcp://127.0.0.1:5000", "test");
    client.started_ = true;
    auto *rawEngine = new FakeNetEngine();
    NetEnginePtr guard(rawEngine); // prevent premature deletion
    client.engine_ = rawEngine;

    client.Stop();
    // Stop() must set stopping_ and started_=false, call engine->Stop(), then null engine_.
    EXPECT_TRUE(client.stopping_);
    EXPECT_FALSE(client.started_);
    EXPECT_TRUE(rawEngine->stopCalled);
    EXPECT_EQ(client.engine_.Get(), nullptr);
}

/* ── HandleLinkBroken() tests: stopping_ early-exit ─────────────────────── */

TEST_F(TestMetaNetClient, HandleLinkBroken_StoppingTrue_AbortsBeforeConnect)
{
    MetaNetClient client("tcp://127.0.0.1:5000", "test");
    auto *rawEngine = new FakeNetEngine();
    NetEnginePtr guard(rawEngine);
    client.engine_ = rawEngine;
    client.stopping_ = true;

    NetLinkPtr nullLink;
    auto ret = client.HandleLinkBroken(nullLink);
    // Must return MMC_ERROR without calling ConnectToPeer.
    EXPECT_EQ(ret, MMC_ERROR);
    EXPECT_FALSE(rawEngine->connectCalled);
}

TEST_F(TestMetaNetClient, HandleLinkBroken_ConnectSucceeds_ReturnsOk)
{
    MetaNetClient client("tcp://127.0.0.1:5000", "test");
    auto *rawEngine = new FakeNetEngine();
    rawEngine->connectResult = MMC_OK;
    NetEnginePtr guard(rawEngine);
    client.engine_ = rawEngine;
    client.stopping_ = false;
    client.ip_ = "127.0.0.1";
    client.port_ = 5000;

    NetLinkPtr nullLink;
    auto ret = client.HandleLinkBroken(nullLink);
    // ConnectToPeer returns OK and retryHandler_ is null → return MMC_OK.
    EXPECT_EQ(ret, MMC_OK);
    EXPECT_TRUE(rawEngine->connectCalled);
}

TEST_F(TestMetaNetClient, HandleLinkBroken_WithRetryHandler_CallsHandler)
{
    MetaNetClient client("tcp://127.0.0.1:5000", "test");
    auto *rawEngine = new FakeNetEngine();
    rawEngine->connectResult = MMC_OK;
    NetEnginePtr guard(rawEngine);
    client.engine_ = rawEngine;
    client.stopping_ = false;
    client.ip_ = "127.0.0.1";
    client.port_ = 5000;

    bool handlerCalled = false;
    client.retryHandler_ = [&handlerCalled]() -> int32_t {
        handlerCalled = true;
        return MMC_OK;
    };

    NetLinkPtr nullLink;
    auto ret = client.HandleLinkBroken(nullLink);
    // ConnectToPeer returns OK and retryHandler_ is set → call handler and return its result.
    EXPECT_EQ(ret, MMC_OK);
    EXPECT_TRUE(rawEngine->connectCalled);
    EXPECT_TRUE(handlerCalled);
}

TEST_F(TestMetaNetClient, HandleLinkBroken_EngineNullptr_ReturnsNotInitialized)
{
    MetaNetClient client("tcp://127.0.0.1:5000", "test");
    // engine_ defaults to nullptr; HandleLinkBroken must return early.
    client.stopping_ = false;

    NetLinkPtr nullLink;
    auto ret = client.HandleLinkBroken(nullLink);
    EXPECT_EQ(ret, MMC_NOT_INITIALIZED);
}

TEST_F(TestMetaNetClient, Stop_CalledTwice_SecondCallIsNoOp)
{
    MetaNetClient client("tcp://127.0.0.1:5000", "test");
    client.started_ = true;
    auto *rawEngine = new FakeNetEngine();
    NetEnginePtr guard(rawEngine);
    client.engine_ = rawEngine;

    client.Stop();
    EXPECT_TRUE(rawEngine->stopCalled);
    EXPECT_FALSE(client.started_);
    EXPECT_EQ(client.engine_.Get(), nullptr);

    // Reset flag to verify second Stop() does not call engine->Stop() again.
    rawEngine->stopCalled = false;
    client.Stop();
    EXPECT_FALSE(rawEngine->stopCalled);
}

/* ── ApplyDiscoveredServerUrl() tests ───────────────────────────────────── */

TEST_F(TestMetaNetClient, ApplyDiscoveredServerUrl_EmptyUrl_ReturnsErrorAndKeepsState)
{
    MetaNetClient client("tcp://127.0.0.1:5000", "test");
    // 新 leader 地址尚未登记（KEY_META_SERVICE_ADDR 写入间隙）
    client.discoverUrlFunc_ = []() -> std::string { return ""; };
    client.ip_ = "127.0.0.1";
    client.port_ = 5000;
    client.serverUrl_ = "tcp://127.0.0.1:5000";

    bool switched = false;
    auto ret = client.ApplyDiscoveredServerUrl(switched);
    EXPECT_EQ(ret, MMC_ERROR);
    EXPECT_FALSE(switched);
    EXPECT_EQ(client.serverUrl_, "tcp://127.0.0.1:5000");
}

TEST_F(TestMetaNetClient, ApplyDiscoveredServerUrl_SameAddress_IdempotentSkip)
{
    MetaNetClient client("tcp://127.0.0.1:5000", "test");
    client.discoverUrlFunc_ = []() -> std::string { return "tcp://127.0.0.1:5000"; };
    client.ip_ = "127.0.0.1";
    client.port_ = 5000;
    client.serverUrl_ = "tcp://127.0.0.1:5000";

    bool switched = false;
    auto ret = client.ApplyDiscoveredServerUrl(switched);
    // 地址与当前一致：幂等命中，不切换、不改状态
    EXPECT_EQ(ret, MMC_OK);
    EXPECT_FALSE(switched);
    EXPECT_EQ(client.serverUrl_, "tcp://127.0.0.1:5000");
}

TEST_F(TestMetaNetClient, ApplyDiscoveredServerUrl_DifferentAddress_UpdatesServerUrlOnly)
{
    MetaNetClient client("tcp://127.0.0.1:5000", "test");
    client.discoverUrlFunc_ = []() -> std::string { return "tcp://10.0.0.1:6000"; };
    client.ip_ = "127.0.0.1";
    client.port_ = 5000;
    client.serverUrl_ = "tcp://127.0.0.1:5000";

    bool switched = false;
    auto ret = client.ApplyDiscoveredServerUrl(switched);
    // 核心不变量：只更新 serverUrl_（期望目标），ip_/port_ 仍反映实际连接地址，
    // 仅由 ResolveAndConnect 成功后赋值（延迟赋值修复）
    EXPECT_EQ(ret, MMC_OK);
    EXPECT_TRUE(switched);
    EXPECT_EQ(client.serverUrl_, "tcp://10.0.0.1:6000");
    EXPECT_EQ(client.ip_, "127.0.0.1");
    EXPECT_EQ(client.port_, 5000U);
}

/* ── HandleServerChangedNotify() tests ──────────────────────────────────── */

TEST_F(TestMetaNetClient, ServerChangedNotify_NoDiscoverFunc_ReturnsOkWithoutConnect)
{
    MetaNetClient client("tcp://127.0.0.1:5000", "test");
    auto *rawEngine = new FakeNetEngine();
    NetEnginePtr guard(rawEngine);
    client.engine_ = rawEngine;
    client.started_ = true;
    client.stopping_ = false;
    // discoverUrlFunc_ 保持为空：无从得知新 leader 地址，不做任何处理

    auto ret = client.HandleServerChangedNotify();
    EXPECT_EQ(ret, MMC_OK);
    EXPECT_FALSE(rawEngine->connectCalled);
}

TEST_F(TestMetaNetClient, ServerChangedNotify_ReconnectingInFlight_SkipsEarly)
{
    MetaNetClient client("tcp://127.0.0.1:5000", "test");
    auto *rawEngine = new FakeNetEngine();
    NetEnginePtr guard(rawEngine);
    client.engine_ = rawEngine;
    client.started_ = true;
    client.stopping_ = false;
    int discoverCount = 0;
    client.discoverUrlFunc_ = [&discoverCount]() -> std::string {
        discoverCount++;
        return "tcp://10.0.0.1:6000";
    };
    // 模拟 HandleLinkBroken 正在重连：Notify 必须让位且不触发 discover/connect
    client.reconnecting_ = true;

    auto ret = client.HandleServerChangedNotify();
    EXPECT_EQ(ret, MMC_OK);
    EXPECT_EQ(discoverCount, 0);
    EXPECT_FALSE(rawEngine->connectCalled);
}

TEST_F(TestMetaNetClient, ServerChangedNotify_IdempotentAddress_NoReconnect)
{
    MetaNetClient client("tcp://127.0.0.1:5000", "test");
    auto *rawEngine = new FakeNetEngine();
    NetEnginePtr guard(rawEngine);
    client.engine_ = rawEngine;
    client.started_ = true;
    client.stopping_ = false;
    client.ip_ = "127.0.0.1";
    client.port_ = 5000;
    client.serverUrl_ = "tcp://127.0.0.1:5000";
    // discover 返回当前地址（HandleLinkBroken 已切完或地址未变）
    client.discoverUrlFunc_ = []() -> std::string { return "tcp://127.0.0.1:5000"; };

    auto ret = client.HandleServerChangedNotify();
    EXPECT_EQ(ret, MMC_OK);
    EXPECT_FALSE(rawEngine->connectCalled);
    EXPECT_EQ(client.ip_, "127.0.0.1");
    EXPECT_EQ(client.port_, 5000U);
}

TEST_F(TestMetaNetClient, ServerChangedNotify_NewAddress_ConnectsForceAndUpdatesState)
{
    MetaNetClient client("tcp://127.0.0.1:5000", "test");
    auto *rawEngine = new FakeNetEngine();
    NetEnginePtr guard(rawEngine);
    client.engine_ = rawEngine;
    client.started_ = true;
    client.stopping_ = false;
    client.ip_ = "127.0.0.1";
    client.port_ = 5000;
    client.serverUrl_ = "tcp://127.0.0.1:5000";
    client.discoverUrlFunc_ = []() -> std::string { return "tcp://10.0.0.1:6000"; };
    bool handlerCalled = false;
    client.retryHandler_ = [&handlerCalled]() -> int32_t {
        handlerCalled = true;
        return MMC_OK;
    };

    auto ret = client.HandleServerChangedNotify();
    EXPECT_EQ(ret, MMC_OK);
    EXPECT_TRUE(rawEngine->connectCalled);
    EXPECT_EQ(rawEngine->lastPeerIp, "10.0.0.1");
    EXPECT_EQ(rawEngine->lastPort, 6000U);
    EXPECT_TRUE(rawEngine->lastForce); // 断链/切主重连必须 isForce=true
    EXPECT_TRUE(handlerCalled);        // 切主后需重新注册 BM
    // 连接成功后 ip_/port_ 才更新为实际连接地址（延迟赋值）
    EXPECT_EQ(client.serverUrl_, "tcp://10.0.0.1:6000");
    EXPECT_EQ(client.ip_, "10.0.0.1");
    EXPECT_EQ(client.port_, 6000U);
}
