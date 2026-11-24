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
#include <iostream>
#include <memory>
#include <sstream>
#include "gtest/gtest.h"
#include "mmc_ref.h"
#include "mmc_meta_service.h"
#include "mmc_local_service_default.h"
#include "mmc_msg_client_meta.h"
#include "mmc_locality_strategy.h"
#include "mmc_mem_obj_meta.h"
#include "mmc_client_default.h"
#include "mmc_periodic_task.h"

using namespace testing;
using namespace std;
using namespace ock::mmc;

namespace {
constexpr const char *EXCEEDS_LIMIT_LOG = "exceeds limit";
// Every NetMsgUnpacker error log has this file tag; its presence indicates a deserialization error.
constexpr const char *PACKER_LOG_TAG = "mmc_msg_packer.h";
} // namespace

class TestMmcMetaService : public testing::Test {
public:
    TestMmcMetaService();

    void SetUp() override;

    void TearDown() override;

protected:
};
TestMmcMetaService::TestMmcMetaService() {}

void TestMmcMetaService::SetUp()
{
    cout << "this is NetEngine TEST_F setup:" << endl;
}

void TestMmcMetaService::TearDown()
{
    cout << "this is NetEngine TEST_F teardown" << endl;
}

static void UrlStringToChar(std::string &urlString, char *urlChar)
{
    for (uint32_t i = 0; i < urlString.length(); i++) {
        urlChar[i] = urlString.at(i);
    }
    urlChar[urlString.length()] = '\0';
}

TEST_F(TestMmcMetaService, Init)
{
    std::string metaUrl = "tcp://127.0.0.1:5678";
    std::string bmUrl = "tcp://127.0.0.1:5681";
    std::string hcomUrl = "tcp://127.0.0.1:5682";
    mmc_meta_service_config_t metaServiceConfig{};
    metaServiceConfig.logLevel = INFO_LEVEL;
    metaServiceConfig.logRotationFileSize = 2 * 1024 * 1024;
    metaServiceConfig.logRotationFileCount = 20;
    metaServiceConfig.evictThresholdHigh = 70;
    metaServiceConfig.evictThresholdLow = 60U;
    metaServiceConfig.haEnable = true;
    metaServiceConfig.lruShardCount = MMC_DEFAULT_META_LRU_SHARD_COUNT;
    metaServiceConfig.accTlsConfig.tlsEnable = false;
    metaServiceConfig.asyncFlushIntervalMs = 1000UL;
    metaServiceConfig.asyncFlushBatchLimit = 8UL;
    // mmc_meta_service_config_t no longer has localSsdSize
    UrlStringToChar(metaUrl, metaServiceConfig.discoveryURL);
    UrlStringToChar(bmUrl, metaServiceConfig.configStoreURL);
    auto metaService = MmcMakeRef<MmcMetaService>("testMetaService");
    ASSERT_TRUE(metaService->Start(metaServiceConfig) == MMC_OK);

    mmc_local_service_config_t localServiceConfig = {
        "", 0, 0, 1, "", "", "", "", 0, "device_sdma", 0, 0, 104857600, 104857600, 0, 0, {}, 0, nullptr, {}, {}};
    localServiceConfig.logLevel = INFO_LEVEL;
    localServiceConfig.accTlsConfig.tlsEnable = false;
    UrlStringToChar(metaUrl, localServiceConfig.discoveryURL);
    UrlStringToChar(bmUrl, localServiceConfig.bmIpPort);
    UrlStringToChar(hcomUrl, localServiceConfig.bmHcomUrl);
    auto localServiceDefault = MmcMakeRef<MmcLocalServiceDefault>("testLocalService");
    MmcLocalServicePtr localServicePtr = Convert<MmcLocalServiceDefault, MmcLocalService>(localServiceDefault);
    ASSERT_TRUE(localServicePtr->Start(localServiceConfig) == MMC_OK);

    PingMsg req;
    req.msgId = ML_PING_REQ;
    req.num = 123;
    PingMsg resp;
    ASSERT_TRUE(localServiceDefault->SyncCallMeta(req, resp, 30) == MMC_OK);

    AllocRequest reqAlloc;
    reqAlloc.key_ = "test";
    reqAlloc.options_ = AllocOptions(SIZE_32K, 1, MEDIA_HBM, {0}, 0);
    AllocResponse respAlloc;
    ASSERT_TRUE(localServiceDefault->SyncCallMeta(reqAlloc, respAlloc, 30) == MMC_OK);
    ASSERT_TRUE(respAlloc.numBlobs_ == 1);
    ASSERT_TRUE(respAlloc.blobs_.size() == 1);
    ASSERT_TRUE(respAlloc.blobs_[0].size_ == SIZE_32K);
    localServicePtr->Stop();
    metaService->Stop();
}

TEST_F(TestMmcMetaService, ExistRequest)
{
    GTEST_SKIP() << "stuck for long time, skipping test";
    std::string metaUrl = "tcp://127.0.0.1:5678";
    std::string bmUrl = "tcp://127.0.0.1:5681";
    std::string hcomUrl = "tcp://127.0.0.1:5682";
    mmc_meta_service_config_t metaServiceConfig{};
    metaServiceConfig.logLevel = INFO_LEVEL;
    metaServiceConfig.logRotationFileSize = 2 * 1024 * 1024;
    metaServiceConfig.logRotationFileCount = 20;
    metaServiceConfig.evictThresholdHigh = 70;
    metaServiceConfig.evictThresholdLow = 60U;
    metaServiceConfig.haEnable = true;
    metaServiceConfig.lruShardCount = MMC_DEFAULT_META_LRU_SHARD_COUNT;
    metaServiceConfig.accTlsConfig.tlsEnable = false;
    metaServiceConfig.asyncFlushIntervalMs = 1000UL;
    metaServiceConfig.asyncFlushBatchLimit = 8UL;
    // mmc_meta_service_config_t no longer has localSsdSize
    UrlStringToChar(metaUrl, metaServiceConfig.discoveryURL);
    UrlStringToChar(bmUrl, metaServiceConfig.configStoreURL);
    auto metaService = MmcMakeRef<MmcMetaService>("testMetaService");
    ASSERT_TRUE(metaService->Start(metaServiceConfig) == MMC_OK);
    mmc_local_service_config_t localServiceConfig = {
        "", 0, 0, 1, "", "", "", "", 0, "device_sdma", 0, 0, 104857600, 104857600, 0, 0, {}, 0, nullptr, {}, {}};
    localServiceConfig.logLevel = INFO_LEVEL;
    localServiceConfig.accTlsConfig.tlsEnable = false;
    UrlStringToChar(metaUrl, localServiceConfig.discoveryURL);
    UrlStringToChar(bmUrl, localServiceConfig.bmIpPort);
    UrlStringToChar(hcomUrl, localServiceConfig.bmHcomUrl);
    auto localServiceDefault = MmcMakeRef<MmcLocalServiceDefault>("testLocalService");
    MmcLocalServicePtr localServicePtr = Convert<MmcLocalServiceDefault, MmcLocalService>(localServiceDefault);
    ASSERT_TRUE(localServicePtr->Start(localServiceConfig) == MMC_OK);

    AllocRequest reqAlloc;
    reqAlloc.key_ = "test";
    reqAlloc.options_ = AllocOptions(SIZE_32K, 1, MEDIA_HBM, {0}, 0);
    AllocResponse respAlloc;
    ASSERT_TRUE(localServiceDefault->SyncCallMeta(reqAlloc, respAlloc, 30) == MMC_OK);

    UpdateRequest updateRequest{MMC_WRITE_OK, "test", 0, MEDIA_HBM, 0};
    Response updateResponse;
    ASSERT_TRUE(localServiceDefault->SyncCallMeta(updateRequest, updateResponse, 30) == MMC_OK);

    IsExistRequest reqExist;
    reqExist.key_ = "test";
    IsExistResponse respExist;
    ASSERT_TRUE(localServiceDefault->SyncCallMeta(reqExist, respExist, 30) == MMC_OK);
    ASSERT_TRUE(respExist.ret_ == MMC_OK);

    IsExistRequest reqNotExist;
    reqNotExist.key_ = "another_test";
    IsExistResponse respNotExist;
    ASSERT_TRUE(localServiceDefault->SyncCallMeta(reqNotExist, respNotExist, 30) == MMC_OK);
    ASSERT_TRUE(respNotExist.ret_ != MMC_OK);

    localServicePtr->Stop();
    metaService->Stop();
}

// Exercise the full TCP and serialization/deserialization path through MmcClientDefault::IsExist.
// Verify hit/miss results and the absence of deserialization ERROR logs in the capture window. The old bug parsed
// IsExistResponse as Response and incorrectly logged "Deserialize container size: 0 exceeds limit" on stream exhaustion.
TEST_F(TestMmcMetaService, IsExistClientRoundTrip)
{
    std::string metaUrl = "tcp://127.0.0.1:5678";
    std::string bmUrl = "tcp://127.0.0.1:5681";
    std::string hcomUrl = "tcp://127.0.0.1:5682";
    mmc_meta_service_config_t metaServiceConfig{};
    metaServiceConfig.logLevel = INFO_LEVEL;
    metaServiceConfig.logRotationFileSize = 2 * 1024 * 1024;
    metaServiceConfig.logRotationFileCount = 20;
    metaServiceConfig.evictThresholdHigh = 70;
    metaServiceConfig.evictThresholdLow = 60U;
    metaServiceConfig.haEnable = true;
    metaServiceConfig.lruShardCount = MMC_DEFAULT_META_LRU_SHARD_COUNT;
    metaServiceConfig.accTlsConfig.tlsEnable = false;
    metaServiceConfig.asyncFlushIntervalMs = 1000UL;
    metaServiceConfig.asyncFlushBatchLimit = 8UL;
    UrlStringToChar(metaUrl, metaServiceConfig.discoveryURL);
    UrlStringToChar(bmUrl, metaServiceConfig.configStoreURL);
    auto metaService = MmcMakeRef<MmcMetaService>("testMetaService");
    auto stopMetaService = [](MmcMetaService *service) { service->Stop(); };
    std::unique_ptr<MmcMetaService, decltype(stopMetaService)> metaServiceGuard(metaService.Get(), stopMetaService);
    ASSERT_TRUE(metaService->Start(metaServiceConfig) == MMC_OK);

    mmc_local_service_config_t localServiceConfig = {
        "", 0, 0, 1, "", "", "", "", 0, "device_sdma", 0, 0, 104857600, 104857600, 0, 0, {}, 0, nullptr, {}, {}};
    localServiceConfig.logLevel = INFO_LEVEL;
    localServiceConfig.accTlsConfig.tlsEnable = false;
    UrlStringToChar(metaUrl, localServiceConfig.discoveryURL);
    UrlStringToChar(bmUrl, localServiceConfig.bmIpPort);
    UrlStringToChar(hcomUrl, localServiceConfig.bmHcomUrl);
    auto localServiceDefault = MmcMakeRef<MmcLocalServiceDefault>("testLocalService");
    MmcLocalServicePtr localServicePtr = Convert<MmcLocalServiceDefault, MmcLocalService>(localServiceDefault);
    auto stopLocalService = [](MmcLocalService *service) { service->Stop(); };
    std::unique_ptr<MmcLocalService, decltype(stopLocalService)> localServiceGuard(localServicePtr.Get(),
                                                                                   stopLocalService);
    ASSERT_TRUE(localServicePtr->Start(localServiceConfig) == MMC_OK);

    const std::string hitKey = "client_roundtrip_exist";
    AllocRequest reqAlloc;
    reqAlloc.key_ = hitKey;
    reqAlloc.options_ = AllocOptions(SIZE_32K, 1, MEDIA_HBM, {0}, 0);
    AllocResponse respAlloc;
    ASSERT_TRUE(localServiceDefault->SyncCallMeta(reqAlloc, respAlloc, 30) == MMC_OK);

    UpdateRequest updateRequest{MMC_WRITE_OK, hitKey, 0, MEDIA_HBM, 0};
    Response updateResponse;
    ASSERT_TRUE(localServiceDefault->SyncCallMeta(updateRequest, updateResponse, 30) == MMC_OK);

    mmc_client_config_t clientConfig{};
    UrlStringToChar(metaUrl, clientConfig.discoveryURL);
    clientConfig.rankId = 0;
    clientConfig.rpcRetryTimeOut = 30;
    clientConfig.timeOut = 30;
    clientConfig.readThreadPoolNum = 1;
    clientConfig.writeThreadPoolNum = 1;
    clientConfig.aggregateIO = false;
    clientConfig.logLevel = INFO_LEVEL;
    ASSERT_TRUE(MmcClientDefault::RegisterInstance() == MMC_OK);
    // Wait for in-flight periodic callbacks before stopping and deleting the singleton.
    auto cleanupClient = [&localServiceGuard](MmcClientDefault *client) {
        UnregisterPeriodicTask("client_lease_cleanup");
        UnregisterPeriodicTask("client_metric_report");
        localServiceGuard.reset();
        client->Stop();
        EXPECT_EQ(MmcClientDefault::UnregisterInstance(), MMC_OK);
    };
    std::unique_ptr<MmcClientDefault, decltype(cleanupClient)> clientGuard(MmcClientDefault::GetInstance(),
                                                                           cleanupClient);
    MmcClientDefault *client = clientGuard.get();
    ASSERT_TRUE(client != nullptr);
    ASSERT_TRUE(client->Start(clientConfig) == MMC_OK);

    // In UT builds, all MMC_LOG_* output goes to std::cout; redirect it to capture IsExist deserialization logs.
    std::stringstream logCapture;
    std::streambuf *originalBuf = std::cout.rdbuf(logCapture.rdbuf());
    const Result hitRet = client->IsExist(hitKey, 0);
    const Result missRet = client->IsExist("client_roundtrip_missing", 0);
    std::cout.rdbuf(originalBuf);

    ASSERT_TRUE(hitRet == MMC_OK);
    ASSERT_TRUE(missRet == MMC_UNMATCHED_KEY);
    const std::string capturedLogs = logCapture.str();
    ASSERT_TRUE(capturedLogs.find(EXCEEDS_LIMIT_LOG) == std::string::npos)
        << "IsExist triggered deserialize size-limit log: " << capturedLogs;
    ASSERT_TRUE(capturedLogs.find(PACKER_LOG_TAG) == std::string::npos)
        << "IsExist triggered packer error log: " << capturedLogs;
}

TEST_F(TestMmcMetaService, BatchExistRequest)
{
    std::string metaUrl = "tcp://127.0.0.1:5678";
    std::string bmUrl = "tcp://127.0.0.1:5681";
    std::string hcomUrl = "tcp://127.0.0.1:5682";
    mmc_meta_service_config_t metaServiceConfig{};
    metaServiceConfig.logLevel = INFO_LEVEL;
    metaServiceConfig.logRotationFileSize = 2 * 1024 * 1024;
    metaServiceConfig.logRotationFileCount = 20;
    metaServiceConfig.evictThresholdHigh = 70;
    metaServiceConfig.evictThresholdLow = 60U;
    metaServiceConfig.haEnable = true;
    metaServiceConfig.lruShardCount = MMC_DEFAULT_META_LRU_SHARD_COUNT;
    metaServiceConfig.accTlsConfig.tlsEnable = false;
    metaServiceConfig.asyncFlushIntervalMs = 1000UL;
    metaServiceConfig.asyncFlushBatchLimit = 8UL;
    // mmc_meta_service_config_t no longer has localSsdSize
    UrlStringToChar(metaUrl, metaServiceConfig.discoveryURL);
    UrlStringToChar(bmUrl, metaServiceConfig.configStoreURL);
    auto metaService = MmcMakeRef<MmcMetaService>("testMetaService");
    ASSERT_TRUE(metaService->Start(metaServiceConfig) == MMC_OK);

    mmc_local_service_config_t localServiceConfig = {
        "", 0, 0, 1, "", "", "", "", 0, "device_sdma", 0, 0, 104857600, 104857600, 0, 0, {}, 0, nullptr, {}, {}};
    localServiceConfig.logLevel = INFO_LEVEL;
    localServiceConfig.accTlsConfig.tlsEnable = false;
    UrlStringToChar(metaUrl, localServiceConfig.discoveryURL);
    UrlStringToChar(bmUrl, localServiceConfig.bmIpPort);
    UrlStringToChar(hcomUrl, localServiceConfig.bmHcomUrl);
    auto localServiceDefault = MmcMakeRef<MmcLocalServiceDefault>("testLocalService");
    MmcLocalServicePtr localServicePtr = Convert<MmcLocalServiceDefault, MmcLocalService>(localServiceDefault);
    ASSERT_TRUE(localServicePtr->Start(localServiceConfig) == MMC_OK);

    AllocRequest reqAlloc;
    reqAlloc.options_ = AllocOptions(SIZE_32K, 1, MEDIA_HBM, {0}, 0);
    for (uint16_t i = 0U; i < 5U; ++i) {
        reqAlloc.key_ = "test_" + std::to_string(i);
        AllocResponse respAlloc;
        ASSERT_TRUE(localServiceDefault->SyncCallMeta(reqAlloc, respAlloc, 30) == MMC_OK);

        UpdateRequest updateRequest{MMC_WRITE_OK, "test_" + std::to_string(i), 0, MEDIA_HBM, 0};
        Response updateResponse;
        ASSERT_TRUE(localServiceDefault->SyncCallMeta(updateRequest, updateResponse, 30) == MMC_OK);
    }

    std::vector<std::string> allExistKeys;
    std::vector<std::string> partExistKeys;
    std::vector<std::string> allNotExistKeys;
    std::vector<Result> results;
    auto GetKeys = [](uint16_t start, uint16_t end, std::vector<std::string> &keys) {
        for (uint16_t i = start; i < end; ++i) {
            string key = "test_" + std::to_string(i);
            keys.push_back(key);
        }
    };
    GetKeys(0U, 5U, allExistKeys);
    GetKeys(2U, 7U, partExistKeys);
    GetKeys(5U, 10U, allNotExistKeys);

    auto CheckReturn = [&localServiceDefault](const std::vector<std::string> &keys,
                                              const std::vector<Result> &targetResults) {
        BatchIsExistRequest reqExist;
        reqExist.keys_ = keys;
        BatchIsExistResponse respExist;
        ASSERT_TRUE(localServiceDefault->SyncCallMeta(reqExist, respExist, 30) == MMC_OK);
        ASSERT_TRUE(respExist.results_.size() == targetResults.size());
        for (size_t i = 0; i < targetResults.size(); ++i) {
            ASSERT_TRUE(targetResults[i] == respExist.results_[i]);
        }
    };
    CheckReturn(allExistKeys, std::vector<Result>(5, MMC_OK));
    CheckReturn(partExistKeys, {MMC_OK, MMC_OK, MMC_OK, MMC_UNMATCHED_KEY, MMC_UNMATCHED_KEY});
    CheckReturn(allNotExistKeys, std::vector<Result>(5, MMC_UNMATCHED_KEY));

    localServicePtr->Stop();
    metaService->Stop();
}

TEST_F(TestMmcMetaService, QueryRequest)
{
    std::string metaUrl = "tcp://127.0.0.1:5678";
    std::string bmUrl = "tcp://127.0.0.1:5681";
    std::string hcomUrl = "tcp://127.0.0.1:5682";
    mmc_meta_service_config_t metaServiceConfig{};
    metaServiceConfig.logLevel = INFO_LEVEL;
    metaServiceConfig.logRotationFileSize = 2 * 1024 * 1024;
    metaServiceConfig.logRotationFileCount = 20;
    metaServiceConfig.evictThresholdHigh = 70;
    metaServiceConfig.evictThresholdLow = 60U;
    metaServiceConfig.haEnable = true;
    metaServiceConfig.lruShardCount = MMC_DEFAULT_META_LRU_SHARD_COUNT;
    metaServiceConfig.accTlsConfig.tlsEnable = false;
    metaServiceConfig.asyncFlushIntervalMs = 1000UL;
    metaServiceConfig.asyncFlushBatchLimit = 8UL;
    // mmc_meta_service_config_t no longer has localSsdSize
    UrlStringToChar(metaUrl, metaServiceConfig.discoveryURL);
    UrlStringToChar(bmUrl, metaServiceConfig.configStoreURL);
    auto metaService = MmcMakeRef<MmcMetaService>("testMetaService");
    ASSERT_TRUE(metaService->Start(metaServiceConfig) == MMC_OK);

    mmc_local_service_config_t localServiceConfig = {
        "", 0, 0, 1, "", "", "", "", 0, "device_sdma", 0, 0, 104857600, 104857600, 0, 0, {}, 0, nullptr, {}, {}};
    localServiceConfig.logLevel = INFO_LEVEL;
    localServiceConfig.accTlsConfig.tlsEnable = false;
    UrlStringToChar(metaUrl, localServiceConfig.discoveryURL);
    UrlStringToChar(bmUrl, localServiceConfig.bmIpPort);
    UrlStringToChar(hcomUrl, localServiceConfig.bmHcomUrl);
    auto localServiceDefault = MmcMakeRef<MmcLocalServiceDefault>("testLocalService");
    MmcLocalServicePtr localServicePtr = Convert<MmcLocalServiceDefault, MmcLocalService>(localServiceDefault);
    ASSERT_TRUE(localServicePtr->Start(localServiceConfig) == MMC_OK);

    AllocRequest reqAlloc;
    reqAlloc.key_ = "test";
    reqAlloc.options_ = AllocOptions(SIZE_32K, 1, MEDIA_HBM, {0}, 0);
    AllocResponse respAlloc;
    ASSERT_TRUE(localServiceDefault->SyncCallMeta(reqAlloc, respAlloc, 30) == MMC_OK);

    UpdateRequest updateRequest{MMC_WRITE_OK, "test", 0, MEDIA_HBM, 0};
    Response updateResponse;
    ASSERT_TRUE(localServiceDefault->SyncCallMeta(updateRequest, updateResponse, 30) == MMC_OK);

    QueryRequest reqQuery;
    reqQuery.key_ = "test";
    QueryResponse respQuery;
    ASSERT_TRUE(localServiceDefault->SyncCallMeta(reqQuery, respQuery, 30) == MMC_OK);
    ASSERT_TRUE(respQuery.queryInfo_.valid_ == true);
    ASSERT_TRUE(respQuery.queryInfo_.size_ == SIZE_32K);
    ASSERT_TRUE(respQuery.queryInfo_.prot_ == 0);
    ASSERT_TRUE(respQuery.queryInfo_.numBlobs_ == 1);

    QueryRequest reqQueryNone;
    reqQueryNone.key_ = "another_test";
    QueryResponse respQueryNone;
    ASSERT_TRUE(localServiceDefault->SyncCallMeta(reqQueryNone, respQueryNone, 30) == MMC_OK);
    ASSERT_TRUE(respQueryNone.queryInfo_.valid_ == false);

    localServicePtr->Stop();
    metaService->Stop();
}

TEST_F(TestMmcMetaService, BatchQueryRequest)
{
    std::string metaUrl = "tcp://127.0.0.1:5678";
    std::string bmUrl = "tcp://127.0.0.1:5681";
    std::string hcomUrl = "tcp://127.0.0.1:5682";
    mmc_meta_service_config_t metaServiceConfig{};
    metaServiceConfig.logLevel = INFO_LEVEL;
    metaServiceConfig.logRotationFileSize = 2 * 1024 * 1024;
    metaServiceConfig.logRotationFileCount = 20;
    metaServiceConfig.evictThresholdHigh = 70;
    metaServiceConfig.evictThresholdLow = 60U;
    metaServiceConfig.haEnable = true;
    metaServiceConfig.lruShardCount = MMC_DEFAULT_META_LRU_SHARD_COUNT;
    metaServiceConfig.accTlsConfig.tlsEnable = false;
    metaServiceConfig.asyncFlushIntervalMs = 1000UL;
    metaServiceConfig.asyncFlushBatchLimit = 8UL;
    // mmc_meta_service_config_t no longer has localSsdSize
    UrlStringToChar(metaUrl, metaServiceConfig.discoveryURL);
    UrlStringToChar(bmUrl, metaServiceConfig.configStoreURL);
    auto metaService = MmcMakeRef<MmcMetaService>("testMetaService");
    ASSERT_TRUE(metaService->Start(metaServiceConfig) == MMC_OK);

    mmc_local_service_config_t localServiceConfig = {
        "", 0, 0, 1, "", "", "", "", 0, "device_sdma", 0, 0, 104857600, 104857600, 0, 0, {}, 0, nullptr, {}, {}};
    localServiceConfig.logLevel = INFO_LEVEL;
    localServiceConfig.accTlsConfig.tlsEnable = false;
    UrlStringToChar(metaUrl, localServiceConfig.discoveryURL);
    UrlStringToChar(bmUrl, localServiceConfig.bmIpPort);
    UrlStringToChar(hcomUrl, localServiceConfig.bmHcomUrl);
    auto localServiceDefault = MmcMakeRef<MmcLocalServiceDefault>("testLocalService");
    MmcLocalServicePtr localServicePtr = Convert<MmcLocalServiceDefault, MmcLocalService>(localServiceDefault);
    ASSERT_TRUE(localServicePtr->Start(localServiceConfig) == MMC_OK);

    AllocRequest reqAlloc;
    std::map<std::string, std::pair<uint32_t, uint8_t>> keyMap; // key : {size, numBlobs}
    reqAlloc.options_ = AllocOptions(SIZE_32K, 1, MEDIA_HBM, {0}, 0);
    for (uint16_t i = 0U; i < 3U; ++i) {
        reqAlloc.key_ = "test_" + std::to_string(i);
        reqAlloc.options_.numBlobs_ = 1;
        keyMap.insert({reqAlloc.key_, {SIZE_32K, static_cast<uint8_t>(reqAlloc.options_.numBlobs_)}});
        AllocResponse respAlloc;
        ASSERT_TRUE(localServiceDefault->SyncCallMeta(reqAlloc, respAlloc, 30) == MMC_OK);

        UpdateRequest updateRequest{MMC_WRITE_OK, "test_" + std::to_string(i), 0, MEDIA_HBM, 0};
        Response updateResponse;
        ASSERT_TRUE(localServiceDefault->SyncCallMeta(updateRequest, updateResponse, 30) == MMC_OK);
    }

    std::vector<std::string> allExistKeys;
    std::vector<std::string> partExistKeys;
    std::vector<std::string> allNotExistKeys;
    std::vector<Result> results;
    auto GetKeys = [](uint16_t start, uint16_t end, std::vector<std::string> &keys) {
        for (uint16_t i = start; i < end; ++i) {
            string key = "test_" + std::to_string(i);
            keys.push_back(key);
        }
    };
    GetKeys(0U, 3U, allExistKeys);
    GetKeys(2U, 5U, partExistKeys);
    GetKeys(3U, 7U, allNotExistKeys);

    auto CheckReturn = [&localServiceDefault, &keyMap](const std::vector<std::string> &keys) {
        BatchQueryRequest reqQuery;
        reqQuery.keys_ = keys;
        BatchQueryResponse respQuery;
        ASSERT_TRUE(localServiceDefault->SyncCallMeta(reqQuery, respQuery, 30) == MMC_OK);
        ASSERT_TRUE(respQuery.batchQueryInfos_.size() == keys.size());
        auto queryInfo = respQuery.batchQueryInfos_.begin();
        for (const std::string &key : keys) {
            if (keyMap.find(key) != keyMap.end()) {
                ASSERT_TRUE(true == queryInfo->valid_);
                ASSERT_TRUE(keyMap[key].first == queryInfo->size_);
                ASSERT_TRUE(keyMap[key].second == queryInfo->numBlobs_);
                ASSERT_TRUE(0 == queryInfo->prot_);
            } else {
                ASSERT_TRUE(false == queryInfo->valid_);
            }
            ++queryInfo;
        }
    };
    CheckReturn(allExistKeys);
    CheckReturn(partExistKeys);
    CheckReturn(allNotExistKeys);

    localServicePtr->Stop();
    metaService->Stop();
}
