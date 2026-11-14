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

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

#include "gtest/gtest.h"

#include "mmc_ref.h"
#include "mmc_net_engine.h"
#include "mmc_net_engine_acc.h"
#include "mmc_msg_client_meta.h"

using namespace testing;
using namespace std;
using namespace ock::mmc;

namespace {
constexpr uint16_t TEST_PORT_BASE = 5831U;
constexpr const char *LOCAL_IP = "127.0.0.1";
constexpr uint32_t SERVER_RANK_ID = 0U;
constexpr uint32_t CLIENT_RANK_ID = 1U;
constexpr int32_t LONG_TIMEOUT_SECONDS = 60;
constexpr int32_t FAST_FAIL_MAX_SECONDS = 5;
constexpr int32_t SHORT_TIMEOUT_SECONDS = 1;
constexpr int32_t TIMEOUT_TOLERANCE_SECONDS = 2;
constexpr uint16_t NET_WORKER_THREAD_COUNT = 2U;
constexpr int SLEEP_TO_ENSURE_INFLIGHT_MS = 500;

int32_t HandleNeverReply(NetContextPtr &ctx)
{
    (void)ctx;
    return MMC_OK;
}

NetEngineOptions MakeServerOptions(const std::string &name, uint16_t port)
{
    NetEngineOptions options;
    options.name = name;
    options.ip = LOCAL_IP;
    options.port = port;
    options.threadCount = NET_WORKER_THREAD_COUNT;
    options.rankId = SERVER_RANK_ID;
    options.startListener = true;
    options.tlsOption.tlsEnable = false;
    return options;
}

NetEngineOptions MakeClientOptions(const std::string &name)
{
    NetEngineOptions options;
    options.name = name;
    options.threadCount = NET_WORKER_THREAD_COUNT;
    options.rankId = CLIENT_RANK_ID;
    options.startListener = false;
    options.tlsOption.tlsEnable = false;
    return options;
}

NetEnginePtr StartServer(uint16_t port, const std::string &name, NetReqReceivedHandler handler)
{
    NetEnginePtr server = NetEngine::Create();
    if (server == nullptr) {
        return nullptr;
    }
    server->RegRequestReceivedHandler(LOCAL_META_OPCODE_REQ::ML_PING_REQ, std::move(handler));
    server->RegNewLinkHandler([](const NetLinkPtr &) { return MMC_OK; });
    if (server->Start(MakeServerOptions(name, port)) != MMC_OK) {
        return nullptr;
    }
    return server;
}

NetEnginePtr StartClient(const std::string &name)
{
    NetEnginePtr client = NetEngine::Create();
    if (client == nullptr) {
        return nullptr;
    }
    client->RegRequestReceivedHandler(LOCAL_META_OPCODE_REQ::ML_PING_REQ, nullptr);
    client->RegLinkBrokenHandler([](const NetLinkPtr &) { return MMC_OK; });
    if (client->Start(MakeClientOptions(name)) != MMC_OK) {
        return nullptr;
    }
    return client;
}

Result CallOnce(NetEnginePtr &engine, uint32_t peerId, int32_t timeoutSeconds, uint64_t &outNum)
{
    PingMsg req;
    req.destRankId = peerId;
    req.msgId = LOCAL_META_OPCODE_REQ::ML_PING_REQ;
    req.num = 0;
    PingMsg resp;
    Result ret = engine->Call(peerId, req.msgId, req, resp, timeoutSeconds);
    if (ret == MMC_OK) {
        outNum = resp.num;
    }
    return ret;
}

std::future<Result> CallAsync(NetEnginePtr &engine, uint32_t peerId, int32_t timeoutSeconds)
{
    return std::async(std::launch::async, [&engine, peerId, timeoutSeconds]() {
        uint64_t num = 0;
        return CallOnce(engine, peerId, timeoutSeconds, num);
    });
}

std::chrono::seconds ElapsedSeconds(const std::chrono::steady_clock::time_point &start)
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start);
}
} // namespace

class TestMmcNetEngineFailPending : public testing::Test {
public:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(TestMmcNetEngineFailPending, CallReturnsLinkNotFoundWhenServerStopsWhileInflight)
{
    const uint16_t port = TEST_PORT_BASE;
    NetEnginePtr server = StartServer(port, "srv_u1", HandleNeverReply);
    ASSERT_NE(server, nullptr);
    NetEnginePtr client = StartClient("cli_u1");
    ASSERT_NE(client, nullptr);

    NetLinkPtr link;
    ASSERT_EQ(client->ConnectToPeer(SERVER_RANK_ID, LOCAL_IP, port, link, false), MMC_OK);

    auto start = std::chrono::steady_clock::now();
    auto future = CallAsync(client, SERVER_RANK_ID, LONG_TIMEOUT_SECONDS);
    std::this_thread::sleep_for(std::chrono::milliseconds(SLEEP_TO_ENSURE_INFLIGHT_MS));

    server->Stop();

    auto ret = future.get();
    auto elapsed = ElapsedSeconds(start);

    EXPECT_EQ(ret, MMC_LINK_NOT_FOUND);
    EXPECT_LT(elapsed.count(), FAST_FAIL_MAX_SECONDS);

    client->Stop();
}

TEST_F(TestMmcNetEngineFailPending, CallReturnsLinkNotFoundWhenForceReconnectWithInflight)
{
    const uint16_t port = TEST_PORT_BASE + 1U;
    NetEnginePtr server = StartServer(port, "srv_u2", HandleNeverReply);
    ASSERT_NE(server, nullptr);
    NetEnginePtr client = StartClient("cli_u2");
    ASSERT_NE(client, nullptr);

    NetLinkPtr link;
    ASSERT_EQ(client->ConnectToPeer(SERVER_RANK_ID, LOCAL_IP, port, link, false), MMC_OK);

    auto start = std::chrono::steady_clock::now();
    auto future = CallAsync(client, SERVER_RANK_ID, LONG_TIMEOUT_SECONDS);
    std::this_thread::sleep_for(std::chrono::milliseconds(SLEEP_TO_ENSURE_INFLIGHT_MS));

    NetLinkPtr newLink;
    client->ConnectToPeer(SERVER_RANK_ID, LOCAL_IP, port, newLink, true);

    auto ret = future.get();
    auto elapsed = ElapsedSeconds(start);

    EXPECT_EQ(ret, MMC_LINK_NOT_FOUND);
    EXPECT_LT(elapsed.count(), FAST_FAIL_MAX_SECONDS);

    client->Stop();
    server->Stop();
}

TEST_F(TestMmcNetEngineFailPending, AllInflightFailFastOnStop)
{
    const uint16_t port = TEST_PORT_BASE + 2U;
    NetEnginePtr server = StartServer(port, "srv_u3", HandleNeverReply);
    ASSERT_NE(server, nullptr);
    NetEnginePtr client = StartClient("cli_u3");
    ASSERT_NE(client, nullptr);

    NetLinkPtr link;
    ASSERT_EQ(client->ConnectToPeer(SERVER_RANK_ID, LOCAL_IP, port, link, false), MMC_OK);

    auto start = std::chrono::steady_clock::now();
    auto future = CallAsync(client, SERVER_RANK_ID, LONG_TIMEOUT_SECONDS);
    std::this_thread::sleep_for(std::chrono::milliseconds(SLEEP_TO_ENSURE_INFLIGHT_MS));

    client->Stop();

    auto ret = future.get();
    auto elapsed = ElapsedSeconds(start);

    EXPECT_EQ(ret, MMC_LINK_NOT_FOUND);
    EXPECT_LT(elapsed.count(), FAST_FAIL_MAX_SECONDS);

    server->Stop();
}

TEST_F(TestMmcNetEngineFailPending, MultipleInflightCallsAllFailFastOnLinkBroken)
{
    const uint16_t port = TEST_PORT_BASE + 3U;
    NetEnginePtr server = StartServer(port, "srv_u4", HandleNeverReply);
    ASSERT_NE(server, nullptr);
    NetEnginePtr client = StartClient("cli_u4");
    ASSERT_NE(client, nullptr);

    NetLinkPtr link;
    ASSERT_EQ(client->ConnectToPeer(SERVER_RANK_ID, LOCAL_IP, port, link, false), MMC_OK);

    constexpr int numCalls = 4;
    std::vector<std::future<Result>> futures;
    for (int i = 0; i < numCalls; i++) {
        futures.push_back(CallAsync(client, SERVER_RANK_ID, LONG_TIMEOUT_SECONDS));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(SLEEP_TO_ENSURE_INFLIGHT_MS));

    auto start = std::chrono::steady_clock::now();
    server->Stop();

    for (int i = 0; i < numCalls; i++) {
        auto ret = futures[i].get();
        EXPECT_EQ(ret, MMC_LINK_NOT_FOUND);
    }
    auto elapsed = ElapsedSeconds(start);

    EXPECT_LT(elapsed.count(), FAST_FAIL_MAX_SECONDS);

    client->Stop();
}

TEST_F(TestMmcNetEngineFailPending, RemovePendingOnTimeout)
{
    const uint16_t port = TEST_PORT_BASE + 5U;
    NetEnginePtr server = StartServer(port, "srv_u10", HandleNeverReply);
    ASSERT_NE(server, nullptr);
    NetEnginePtr client = StartClient("cli_u10");
    ASSERT_NE(client, nullptr);

    NetLinkPtr link;
    ASSERT_EQ(client->ConnectToPeer(SERVER_RANK_ID, LOCAL_IP, port, link, false), MMC_OK);

    auto start = std::chrono::steady_clock::now();
    uint64_t num = 0;
    auto ret = CallOnce(client, SERVER_RANK_ID, SHORT_TIMEOUT_SECONDS, num);
    auto elapsed = ElapsedSeconds(start);

    EXPECT_EQ(ret, MMC_TIMEOUT);
    EXPECT_GE(elapsed.count(), SHORT_TIMEOUT_SECONDS);
    EXPECT_LE(elapsed.count(), SHORT_TIMEOUT_SECONDS + TIMEOUT_TOLERANCE_SECONDS);

    client->Stop();
    server->Stop();
}
