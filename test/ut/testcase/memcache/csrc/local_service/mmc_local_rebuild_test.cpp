/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * MemCache_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#include <algorithm>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "mmc_local_service_default.h"
#include "mmc_msg_client_meta.h"

namespace ock {
namespace mmc {
namespace {
constexpr uint32_t kLocalRank = 3;
constexpr uint64_t kBlobSize = 4096;
constexpr uint64_t kDramGva = 4096;
constexpr uint64_t kHbmGva = 8192;
constexpr size_t kQueryBatchSize = 1024;
constexpr size_t kRegisterBatchSize = 10240;
constexpr size_t kSingleItem = 1;
constexpr size_t kSecondBatch = 2;
constexpr size_t kThreeRequests = 3;

using BlobMap = std::map<std::string, std::vector<MmcMemBlobDesc>>;
using BlobList = std::vector<std::pair<std::string, MmcMemBlobDesc>>;

template<typename Tag, typename Tag::Type member>
struct RebuildTestAccessor {
    friend typename Tag::Type GetMember(Tag)
    {
        return member;
    }
};

struct BlobMapMember {
    using Type = BlobMap MmcLocalServiceDefault::*;
    friend Type GetMember(BlobMapMember);
};
struct OptionsMember {
    using Type = mmc_local_service_config_t MmcLocalServiceDefault::*;
    friend Type GetMember(OptionsMember);
};
struct BmProxyMember {
    using Type = MmcBmProxyPtr MmcLocalServiceDefault::*;
    friend Type GetMember(BmProxyMember);
};
struct UbsIoProxyMember {
    using Type = MmcUbsIoProxyPtr MmcLocalServiceDefault::*;
    friend Type GetMember(UbsIoProxyMember);
};
struct MetaClientMember {
    using Type = MetaNetClientPtr MmcLocalServiceDefault::*;
    friend Type GetMember(MetaClientMember);
};
struct EngineMember {
    using Type = NetEnginePtr MetaNetClient::*;
    friend Type GetMember(EngineMember);
};
struct UbsIoStartedMember {
    using Type = bool MmcUbsIoProxy::*;
    friend Type GetMember(UbsIoStartedMember);
};
struct BatchExistSymbol {
    using Type = ubsio_batch_existFunc *;
    friend Type GetMember(BatchExistSymbol);
};
struct ExistSymbol {
    using Type = ubsio_existFunc *;
    friend Type GetMember(ExistSymbol);
};

template struct RebuildTestAccessor<BlobMapMember, &MmcLocalServiceDefault::blobMap_>;
template struct RebuildTestAccessor<OptionsMember, &MmcLocalServiceDefault::options_>;
template struct RebuildTestAccessor<BmProxyMember, &MmcLocalServiceDefault::bmProxyPtr_>;
template struct RebuildTestAccessor<UbsIoProxyMember, &MmcLocalServiceDefault::ubsIoProxyPtr_>;
template struct RebuildTestAccessor<MetaClientMember, &MmcLocalServiceDefault::metaNetClient_>;
template struct RebuildTestAccessor<EngineMember, &MetaNetClient::engine_>;
template struct RebuildTestAccessor<UbsIoStartedMember, &MmcUbsIoProxy::started_>;
template struct RebuildTestAccessor<BatchExistSymbol, &DlUbsioApi::pUbsioBatchExist>;
template struct RebuildTestAccessor<ExistSymbol, &DlUbsioApi::pUbsioExist>;

struct RebuildBackend {
    std::set<std::string> existingKeys;
    std::vector<std::vector<std::string>> queryBatches;
    size_t singleCalls = 0;
    size_t failBatch = 0;
};
RebuildBackend *gRebuildBackend = nullptr;

int32_t QueryBatch(const char **keys, uint32_t count, bool *results, uint32_t)
{
    gRebuildBackend->queryBatches.emplace_back(keys, keys + count);
    // A failed batch may leave partial/default results; RegisterBm must ignore all of them.
    std::fill_n(results, count, false);
    if (gRebuildBackend->queryBatches.size() == gRebuildBackend->failBatch) {
        // Simulate a positive partial result despite the overall failure.
        if (count != 0) {
            results[0] = true;
        }
        return MMC_ERROR;
    }
    for (uint32_t index = 0; index < count; ++index) {
        results[index] = gRebuildBackend->existingKeys.count(keys[index]) != 0;
    }
    return MMC_OK;
}

bool QuerySingle(const char *key, uint32_t)
{
    ++gRebuildBackend->singleCalls;
    return gRebuildBackend->existingKeys.count(key) != 0;
}

class RebuildNetEngine : public NetEngine {
public:
    Result Start(const NetEngineOptions &) override
    {
        return MMC_OK;
    }

    void Stop() override {}

    Result ConnectToPeer(uint32_t, const std::string &, uint16_t, NetLinkPtr &, bool, bool) override
    {
        return MMC_ERROR;
    }

    Result Send(uint32_t, const char *, uint32_t, int32_t) override
    {
        return MMC_ERROR;
    }

    Result Call(uint32_t, int16_t opCode, const char *reqData, uint32_t reqDataLen, char **respData,
                uint32_t &respDataLen, int32_t) override
    {
        EXPECT_EQ(opCode, ML_BM_REGISTER_REQ);
        NetMsgUnpacker unpacker(std::string(reqData, reqDataLen));
        BmRegisterRequest request;
        EXPECT_EQ(request.Deserialize(unpacker), MMC_OK);
        requests.push_back(request);
        if (callResult != MMC_OK) {
            return callResult;
        }
        NetMsgPacker packer;
        Response response(responseResult);
        response.Serialize(packer);
        const auto data = packer.String();
        *respData = static_cast<char *>(std::malloc(data.size()));
        if (*respData == nullptr) {
            return MMC_MALLOC_FAILED;
        }
        std::copy(data.begin(), data.end(), *respData);
        respDataLen = static_cast<uint32_t>(data.size());
        return MMC_OK;
    }

    Result callResult = MMC_OK;
    Result responseResult = MMC_OK;
    std::vector<BmRegisterRequest> requests;
};

class MmcLocalRebuildTest : public testing::Test {
protected:
    void SetUp() override
    {
        auto &options = local_.*GetMember(OptionsMember{});
        options.rankId = kLocalRank;
        options.storageEnabled = true;
        local_.*GetMember(BmProxyMember{}) = MmcMakeRef<MmcBmProxy>("rebuild_test");
        auto proxy = MmcMakeRef<MmcUbsIoProxy>("rebuild_test");
        ASSERT_NE(proxy, nullptr);
        proxy.Get()->*GetMember(UbsIoStartedMember{}) = true;
        local_.*GetMember(UbsIoProxyMember{}) = proxy;

        engine_ = MmcMakeRef<RebuildNetEngine>();
        ASSERT_NE(engine_, nullptr);
        auto client = MmcMakeRef<MetaNetClient>("", "rebuild_test");
        ASSERT_NE(client, nullptr);
        client.Get()->*GetMember(EngineMember{}) = engine_.Get();
        local_.*GetMember(MetaClientMember{}) = client;

        originalBatchExist_ = *GetMember(BatchExistSymbol{});
        originalExist_ = *GetMember(ExistSymbol{});
        gRebuildBackend = &backend_;
        *GetMember(BatchExistSymbol{}) = QueryBatch;
        *GetMember(ExistSymbol{}) = QuerySingle;
    }

    void TearDown() override
    {
        *GetMember(BatchExistSymbol{}) = originalBatchExist_;
        *GetMember(ExistSymbol{}) = originalExist_;
        gRebuildBackend = nullptr;
    }

    BlobMap &Backups()
    {
        return local_.*GetMember(BlobMapMember{});
    }

    void AddSsdKeys(size_t count, bool exists)
    {
        for (size_t index = 0; index < count; ++index) {
            const auto key = "key-" + std::to_string(index);
            Backups()[key] = {ssd_};
            if (exists) {
                backend_.existingKeys.insert(key);
            }
        }
    }

    std::vector<std::string> BackupKeys()
    {
        std::vector<std::string> keys;
        for (const auto &entry : Backups()) {
            keys.push_back(entry.first);
        }
        return keys;
    }

    BlobList ReportedBlobs()
    {
        BlobList blobs;
        for (const auto &request : engine_->requests) {
            blobs.insert(blobs.end(), request.blobList_.begin(), request.blobList_.end());
        }
        return blobs;
    }

    void ExpectCompleted()
    {
        ASSERT_FALSE(engine_->requests.empty());
        EXPECT_TRUE(engine_->requests.back().blobList_.empty());
        EXPECT_EQ(std::count_if(engine_->requests.begin(), engine_->requests.end(),
                                [](const BmRegisterRequest &request) { return request.blobList_.empty(); }),
                  kSingleItem);
        for (const auto &request : engine_->requests) {
            EXPECT_EQ(request.rank_, kLocalRank);
            EXPECT_TRUE(request.storageEnabled_);
        }
        EXPECT_EQ(backend_.singleCalls, 0U);
    }

    MmcLocalServiceDefault local_{"rebuild_test"};
    MmcRef<RebuildNetEngine> engine_;
    RebuildBackend backend_;
    ubsio_batch_existFunc originalBatchExist_ = nullptr;
    ubsio_existFunc originalExist_ = nullptr;
    const MmcMemBlobDesc ssd_{kLocalRank, 0, kBlobSize, MEDIA_SSD};
    const MmcMemBlobDesc dram_{kLocalRank, kDramGva, kBlobSize, MEDIA_DRAM};
    const MmcMemBlobDesc hbm_{kLocalRank, kHbmGva, kBlobSize, MEDIA_HBM};
};

TEST_F(MmcLocalRebuildTest, BatchResultsPreserveReplicasAndSupplementSsd)
{
    Backups() = {{"dram-only", {dram_}},
                 {"hbm-only", {hbm_}},
                 {"live", {ssd_}},
                 {"mixed", {dram_, ssd_, hbm_}},
                 {"stale", {ssd_}}};
    backend_.existingKeys = {"dram-only", "hbm-only", "live"};
    const auto keys = BackupKeys();

    ASSERT_EQ(local_.RegisterBm(), MMC_OK);

    ASSERT_EQ(backend_.queryBatches.size(), kSingleItem);
    EXPECT_EQ(backend_.queryBatches.front(), keys);
    EXPECT_EQ(Backups().count("stale"), 0U);
    EXPECT_EQ(Backups().at("mixed"), (std::vector<MmcMemBlobDesc>{dram_, hbm_}));
    EXPECT_EQ(Backups().at("dram-only"), (std::vector<MmcMemBlobDesc>{dram_}));
    const BlobList expected{{"dram-only", dram_}, {"dram-only", ssd_}, {"hbm-only", hbm_}, {"hbm-only", ssd_},
                            {"live", ssd_},       {"mixed", dram_},    {"mixed", hbm_}};
    EXPECT_EQ(ReportedBlobs(), expected);
    ExpectCompleted();
}

TEST_F(MmcLocalRebuildTest, QueriesEachKeyOnceAcrossFullAndTailBatches)
{
    AddSsdKeys(kQueryBatchSize + kSingleItem, true);
    Backups().begin()->second = {dram_, ssd_, hbm_};
    const auto keys = BackupKeys();
    const auto before = Backups();

    ASSERT_EQ(local_.RegisterBm(), MMC_OK);

    ASSERT_EQ(backend_.queryBatches.size(), kSecondBatch);
    EXPECT_EQ(backend_.queryBatches.front().size(), kQueryBatchSize);
    EXPECT_EQ(backend_.queryBatches.back().size(), kSingleItem);
    auto queriedKeys = backend_.queryBatches.front();
    queriedKeys.insert(queriedKeys.end(), backend_.queryBatches.back().begin(), backend_.queryBatches.back().end());
    EXPECT_EQ(queriedKeys, keys);
    EXPECT_EQ(Backups(), before);
    ExpectCompleted();
}

TEST_F(MmcLocalRebuildTest, ErasingEntireBatchesDoesNotSkipKeysOrFinishEarly)
{
    AddSsdKeys(kQueryBatchSize + kSingleItem, false);

    ASSERT_EQ(local_.RegisterBm(), MMC_OK);

    EXPECT_TRUE(Backups().empty());
    EXPECT_EQ(backend_.queryBatches.size(), kSecondBatch);
    EXPECT_EQ(engine_->requests.size(), kSingleItem);
    ExpectCompleted();
}

TEST_F(MmcLocalRebuildTest, QueryFailureDropsSsdBatchAndContinues)
{
    AddSsdKeys(kQueryBatchSize + kSingleItem, true);
    auto it = Backups().begin();
    const auto mixedKey = it->first;
    it->second = {dram_, ssd_, hbm_};
    ++it;
    const auto dramOnlyKey = it->first;
    it->second = {dram_};
    const auto tailKey = Backups().rbegin()->first;
    backend_.failBatch = kSingleItem;

    ASSERT_EQ(local_.RegisterBm(), MMC_OK);

    EXPECT_EQ(Backups(), (BlobMap{{mixedKey, {dram_, hbm_}}, {dramOnlyKey, {dram_}}, {tailKey, {ssd_}}}));
    EXPECT_EQ(ReportedBlobs(), (BlobList{{mixedKey, dram_}, {mixedKey, hbm_}, {dramOnlyKey, dram_}, {tailKey, ssd_}}));
    ASSERT_EQ(backend_.queryBatches.size(), kSecondBatch);
    EXPECT_EQ(backend_.queryBatches.front().size(), kQueryBatchSize);
    EXPECT_EQ(backend_.queryBatches.back(), (std::vector<std::string>{tailKey}));
    ExpectCompleted();
}

TEST_F(MmcLocalRebuildTest, LaterQueryFailurePreservesSuccessfulBatchesAndCompletes)
{
    AddSsdKeys(kQueryBatchSize + kSingleItem, true);
    const auto tailKey = Backups().rbegin()->first;
    auto expected = Backups();
    expected.erase(tailKey);
    backend_.failBatch = kSecondBatch;

    ASSERT_EQ(local_.RegisterBm(), MMC_OK);

    EXPECT_EQ(Backups(), expected);
    EXPECT_EQ(ReportedBlobs().size(), kQueryBatchSize);
    EXPECT_EQ(backend_.queryBatches.size(), kSecondBatch);
    ExpectCompleted();
}

TEST_F(MmcLocalRebuildTest, MissingBatchApiDropsSsdBackupsAndCompletes)
{
    Backups() = {{"live", {ssd_}}};
    *GetMember(BatchExistSymbol{}) = nullptr;

    ASSERT_EQ(local_.RegisterBm(), MMC_OK);

    EXPECT_TRUE(Backups().empty());
    EXPECT_TRUE(ReportedBlobs().empty());
    EXPECT_TRUE(backend_.queryBatches.empty());
    EXPECT_EQ(engine_->requests.size(), kSingleItem);
    ExpectCompleted();
}

TEST_F(MmcLocalRebuildTest, NoBackendPreservesExistingDescriptors)
{
    local_.*GetMember(UbsIoProxyMember{}) = nullptr;
    Backups() = {{"mixed", {dram_, ssd_, hbm_}}};
    const auto before = Backups();

    ASSERT_EQ(local_.RegisterBm(), MMC_OK);

    EXPECT_EQ(Backups(), before);
    EXPECT_EQ(ReportedBlobs(), (BlobList{{"mixed", dram_}, {"mixed", ssd_}, {"mixed", hbm_}}));
    EXPECT_TRUE(backend_.queryBatches.empty());
    ExpectCompleted();
}

TEST_F(MmcLocalRebuildTest, EmptyMapSendsOnlyCompletion)
{
    ASSERT_EQ(local_.RegisterBm(), MMC_OK);

    EXPECT_TRUE(backend_.queryBatches.empty());
    EXPECT_EQ(engine_->requests.size(), kSingleItem);
    ExpectCompleted();
}

TEST_F(MmcLocalRebuildTest, RegistrationBatchesRemainIndependentOfQueryBatches)
{
    AddSsdKeys(kRegisterBatchSize + kSingleItem, true);
    const auto before = Backups();

    ASSERT_EQ(local_.RegisterBm(), MMC_OK);

    ASSERT_EQ(engine_->requests.size(), kThreeRequests);
    EXPECT_EQ(engine_->requests.front().blobList_.size(), kRegisterBatchSize);
    EXPECT_EQ(engine_->requests[kSingleItem].blobList_.size(), kSingleItem);
    EXPECT_EQ(ReportedBlobs().size(), before.size());
    EXPECT_EQ(Backups(), before);
    ExpectCompleted();
}

TEST_F(MmcLocalRebuildTest, RpcFailureDoesNotSendCompletion)
{
    Backups() = {{"live", {ssd_}}};
    backend_.existingKeys.insert("live");
    for (bool transportFailure : {true, false}) {
        engine_->requests.clear();
        engine_->callResult = transportFailure ? MMC_ERROR : MMC_OK;
        engine_->responseResult = transportFailure ? MMC_OK : MMC_ERROR;

        EXPECT_EQ(local_.RegisterBm(), MMC_ERROR);

        ASSERT_EQ(engine_->requests.size(), kSingleItem);
        EXPECT_FALSE(engine_->requests.front().blobList_.empty());
    }
}
} // namespace
} // namespace mmc
} // namespace ock
