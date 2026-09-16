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
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "mmc_local_service_default.h"
#include "mmc_msg_client_meta.h"

namespace ock {
namespace mmc {
namespace {
constexpr uint32_t kLocalRank = 3;
constexpr uint32_t kOtherRank = 4;
constexpr uint64_t kBlobSize = 4096;
constexpr uint64_t kDramGva = 4096;
constexpr uint64_t kHbmGva = 8192;
constexpr size_t kOneRequest = 1;
constexpr size_t kTwoRequests = 2;
constexpr int kUnknownEvent = -1;

using BlobMap = std::map<std::string, std::vector<MmcMemBlobDesc>>;

// Inspect the real recovery map and inject only the transport, without production test hooks.
template<typename Tag, typename Tag::Type member>
struct DeleteTestAccessor {
    friend typename Tag::Type GetMember(Tag)
    {
        return member;
    }
};

struct BlobMapMember {
    using Type = BlobMap MmcLocalServiceDefault::*;
    friend Type GetMember(BlobMapMember);
};
struct BlobMutexMember {
    using Type = std::mutex MmcLocalServiceDefault::*;
    friend Type GetMember(BlobMutexMember);
};
struct OptionsMember {
    using Type = mmc_local_service_config_t MmcLocalServiceDefault::*;
    friend Type GetMember(OptionsMember);
};
struct MetaClientMember {
    using Type = MetaNetClientPtr MmcLocalServiceDefault::*;
    friend Type GetMember(MetaClientMember);
};
struct DeleteHandlerMember {
    using Type = void (MmcLocalServiceDefault::*)(int, const std::vector<std::string> &);
    friend Type GetMember(DeleteHandlerMember);
};
struct EngineMember {
    using Type = NetEnginePtr MetaNetClient::*;
    friend Type GetMember(EngineMember);
};

template struct DeleteTestAccessor<BlobMapMember, &MmcLocalServiceDefault::blobMap_>;
template struct DeleteTestAccessor<BlobMutexMember, &MmcLocalServiceDefault::blobMutex_>;
template struct DeleteTestAccessor<OptionsMember, &MmcLocalServiceDefault::options_>;
template struct DeleteTestAccessor<MetaClientMember, &MmcLocalServiceDefault::metaNetClient_>;
template struct DeleteTestAccessor<DeleteHandlerMember, &MmcLocalServiceDefault::HandleUbsIoMetaEvents>;
template struct DeleteTestAccessor<EngineMember, &MetaNetClient::engine_>;

class DeleteEventNetEngine : public NetEngine {
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
        EXPECT_EQ(opCode, ML_UBSIO_META_DELETE_REQ);
        NetMsgUnpacker unpacker(std::string(reqData, reqDataLen));
        UbsIoMetaDeleteRequest request;
        EXPECT_EQ(request.Deserialize(unpacker), MMC_OK);
        requests.push_back(request);
        if (onCall) {
            onCall();
        }
        if (callResult != MMC_OK) {
            return callResult;
        }
        NetMsgPacker packer;
        UbsIoMetaDeleteResponse response(responseResult);
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
    std::vector<UbsIoMetaDeleteRequest> requests;
    std::function<void()> onCall;
};

class MmcLocalSsdDeleteTest : public testing::Test {
protected:
    void SetUp() override
    {
        (local_.*GetMember(OptionsMember{})).rankId = kLocalRank;
        Backups() = {{"ssd-only", {ssd_}},
                     {"mixed", {dram_, ssd_, hbm_}},
                     {"dram-only", {dram_}},
                     {"other-rank", {MmcMemBlobDesc{kOtherRank, 0, kBlobSize, MEDIA_SSD}}}};
    }

    BlobMap &Backups()
    {
        return local_.*GetMember(BlobMapMember{});
    }

    void Delete(const std::vector<std::string> &keys)
    {
        (local_.*GetMember(DeleteHandlerMember{}))(UBSIO_META_DELETE, keys);
    }

    void AttachMeta()
    {
        engine_ = MmcMakeRef<DeleteEventNetEngine>();
        ASSERT_NE(engine_, nullptr);
        auto client = MmcMakeRef<MetaNetClient>("", "ssd_delete_test");
        ASSERT_NE(client, nullptr);
        client.Get()->*GetMember(EngineMember{}) = engine_.Get();
        local_.*GetMember(MetaClientMember{}) = client;
    }

    MmcLocalServiceDefault local_{"ssd_delete_test"};
    MmcRef<DeleteEventNetEngine> engine_;
    const MmcMemBlobDesc ssd_{kLocalRank, 0, kBlobSize, MEDIA_SSD};
    const MmcMemBlobDesc dram_{kLocalRank, kDramGva, kBlobSize, MEDIA_DRAM};
    const MmcMemBlobDesc hbm_{kLocalRank, kHbmGva, kBlobSize, MEDIA_HBM};
};

TEST_F(MmcLocalSsdDeleteTest, CleansWithoutMetaAndPreservesOtherReplicas)
{
    const auto otherRank = Backups().at("other-rank");
    Delete({"ssd-only", "mixed", "dram-only", "other-rank", "missing"});

    EXPECT_EQ(Backups().count("ssd-only"), 0U);
    EXPECT_EQ(Backups().count("missing"), 0U);
    EXPECT_EQ(Backups().at("mixed"), (std::vector<MmcMemBlobDesc>{dram_, hbm_}));
    EXPECT_EQ(Backups().at("dram-only"), (std::vector<MmcMemBlobDesc>{dram_}));
    EXPECT_EQ(Backups().at("other-rank"), otherRank);
}

TEST_F(MmcLocalSsdDeleteTest, CleansAndUnlocksBeforeBatchedRpc)
{
    AttachMeta();
    ASSERT_NE(engine_, nullptr);
    engine_->onCall = [this] {
        // Probe from another thread: try_lock on a mutex owned by this thread is not a valid lock test.
        auto observation = std::async(std::launch::async, [this] {
            std::unique_lock<std::mutex> guard(local_.*GetMember(BlobMutexMember{}), std::try_to_lock);
            if (!guard.owns_lock()) {
                return false;
            }
            return Backups().count("ssd-only") == 0 &&
                   Backups().at("mixed") == std::vector<MmcMemBlobDesc>{dram_, hbm_};
        });
        EXPECT_TRUE(observation.get());
    };
    const std::vector<std::string> keys{"ssd-only", "mixed"};
    Delete(keys);

    ASSERT_EQ(engine_->requests.size(), kOneRequest);
    EXPECT_EQ(engine_->requests.front().rank_, kLocalRank);
    EXPECT_EQ(engine_->requests.front().keys_, keys);
}

TEST_F(MmcLocalSsdDeleteTest, TransportFailureDoesNotRestoreLocalBackup)
{
    AttachMeta();
    ASSERT_NE(engine_, nullptr);
    engine_->callResult = MMC_ERROR;
    Delete({"ssd-only", "mixed"});

    EXPECT_EQ(engine_->requests.size(), kOneRequest);
    EXPECT_EQ(Backups().count("ssd-only"), 0U);
    EXPECT_EQ(Backups().at("mixed"), (std::vector<MmcMemBlobDesc>{dram_, hbm_}));
}

TEST_F(MmcLocalSsdDeleteTest, MetaFailureDoesNotRestoreLocalBackup)
{
    AttachMeta();
    ASSERT_NE(engine_, nullptr);
    engine_->responseResult = MMC_ERROR;
    Delete({"ssd-only", "mixed"});

    EXPECT_EQ(engine_->requests.size(), kOneRequest);
    EXPECT_EQ(Backups().count("ssd-only"), 0U);
    EXPECT_EQ(Backups().at("mixed"), (std::vector<MmcMemBlobDesc>{dram_, hbm_}));
}

TEST_F(MmcLocalSsdDeleteTest, RepeatedAndMissingKeysStillNotifyMeta)
{
    AttachMeta();
    ASSERT_NE(engine_, nullptr);
    const std::vector<std::string> keys{"ssd-only", "ssd-only", "missing"};
    Delete(keys);
    const auto afterFirstDelete = Backups();
    Delete(keys);

    EXPECT_EQ(Backups(), afterFirstDelete);
    EXPECT_EQ(Backups().count("ssd-only"), 0U);
    EXPECT_EQ(Backups().count("missing"), 0U);
    ASSERT_EQ(engine_->requests.size(), kTwoRequests);
    EXPECT_EQ(engine_->requests.back().keys_, keys);
}

TEST_F(MmcLocalSsdDeleteTest, EmptyBatchDoesNotMutateOrNotify)
{
    AttachMeta();
    ASSERT_NE(engine_, nullptr);
    const auto before = Backups();
    Delete({});

    EXPECT_EQ(Backups(), before);
    EXPECT_TRUE(engine_->requests.empty());
}

TEST_F(MmcLocalSsdDeleteTest, UnknownEventDoesNotMutateOrNotify)
{
    AttachMeta();
    ASSERT_NE(engine_, nullptr);
    const auto before = Backups();
    (local_.*GetMember(DeleteHandlerMember{}))(kUnknownEvent, {"ssd-only"});

    EXPECT_EQ(Backups(), before);
    EXPECT_TRUE(engine_->requests.empty());
}
} // namespace
} // namespace mmc
} // namespace ock
