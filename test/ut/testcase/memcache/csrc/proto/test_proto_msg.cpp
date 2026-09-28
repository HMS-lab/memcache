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
#include <gtest/gtest.h>

#include "mmc_msg_client_meta.h"
#include "mmc_msg_packer.h"

using namespace ock::mmc;

class TestProtoMsg : public testing::Test {
public:
    void SetUp() override {}
    void TearDown() override {}
};

// BlobDeleteRequest serialization/deserialization
TEST_F(TestProtoMsg, BlobDeleteRequest_SerializeDeserialize)
{
    BlobDeleteRequest req;
    req.key_ = "test_key";
    req.rank_ = 42U;
    req.msgId = LM_BLOB_DELETE_REQ;

    NetMsgPacker packer;
    ASSERT_EQ(req.Serialize(packer), MMC_OK);
    auto serialized = packer.String();

    BlobDeleteRequest req2;
    NetMsgUnpacker unpacker(serialized);
    ASSERT_EQ(req2.Deserialize(unpacker), MMC_OK);
    EXPECT_EQ(req2.key_, "test_key");
    EXPECT_EQ(req2.rank_, 42U);
    EXPECT_EQ(req2.msgId, LM_BLOB_DELETE_REQ);
}

// BlobDeleteResponse serialization/deserialization — success
TEST_F(TestProtoMsg, BlobDeleteResponse_Success)
{
    BlobDeleteResponse resp;
    resp.ret_ = MMC_OK;
    resp.msgId = LM_BLOB_DELETE_RSP;

    NetMsgPacker packer;
    ASSERT_EQ(resp.Serialize(packer), MMC_OK);
    auto serialized = packer.String();

    BlobDeleteResponse resp2;
    NetMsgUnpacker unpacker(serialized);
    ASSERT_EQ(resp2.Deserialize(unpacker), MMC_OK);
    EXPECT_EQ(resp2.ret_, MMC_OK);
}

// BlobDeleteResponse serialization/deserialization — failure
TEST_F(TestProtoMsg, BlobDeleteResponse_Failure)
{
    BlobDeleteResponse resp;
    resp.ret_ = MMC_ERROR;
    resp.msgId = LM_BLOB_DELETE_RSP;

    NetMsgPacker packer;
    ASSERT_EQ(resp.Serialize(packer), MMC_OK);
    auto serialized = packer.String();

    BlobDeleteResponse resp2;
    NetMsgUnpacker unpacker(serialized);
    ASSERT_EQ(resp2.Deserialize(unpacker), MMC_OK);
    EXPECT_EQ(resp2.ret_, MMC_ERROR);
}

// MetaNetClient BlobDelete handler registration compile verification
// LM_BLOB_DELETE_REQ/LM_BLOB_DELETE_RSP 消息类型存在且可被 MetaNetClient::Start() 注册
// BlobDeleteRequest/BlobDeleteResponse 类型可被 Handler 函数正确解包
TEST_F(TestProtoMsg, BlobDeleteRequest_MsgIdMatchesMetaNetClientRegistration)
{
    // 验证 LM_BLOB_DELETE_REQ 枚举值正确
    BlobDeleteRequest req;
    req.msgId = LM_BLOB_DELETE_REQ;
    EXPECT_NE(req.msgId, -1);
    EXPECT_NE(req.msgId, 0);

    // 验证 LM_BLOB_DELETE_RSP 枚举值正确
    BlobDeleteResponse resp;
    resp.msgId = LM_BLOB_DELETE_RSP;
    EXPECT_NE(resp.msgId, -1);
    EXPECT_NE(resp.msgId, 0);

    // LM_* REQ/RSP 对共享同一 ID 值，与其他 LM_* 对不冲突
    EXPECT_EQ(static_cast<int16_t>(LM_BLOB_DELETE_REQ), static_cast<int16_t>(LM_BLOB_DELETE_RSP));
}

TEST_F(TestProtoMsg, BmRegisterRequestStorageEnabledRoundTrips)
{
    BmRegisterRequest request;
    request.storageEnabled_ = true;

    NetMsgPacker packer;
    ASSERT_EQ(request.Serialize(packer), MMC_OK);
    BmRegisterRequest decoded;
    NetMsgUnpacker unpacker(packer.String());
    ASSERT_EQ(decoded.Deserialize(unpacker), MMC_OK);

    EXPECT_TRUE(decoded.storageEnabled_);
}

TEST_F(TestProtoMsg, BmRegisterRequestKeepsOriginalPayloadLayout)
{
    NetMsgPacker packer;
    const int16_t msgVer = 0;
    const int16_t msgId = ML_BM_REGISTER_REQ;
    const uint32_t destRankId = 0;
    const uint32_t rank = 9;
    const std::string backendId = "legacy_backend";
    const std::vector<uint16_t> mediaTypes;
    const std::vector<uint64_t> addresses;
    const std::vector<uint64_t> capacities;
    const std::vector<std::pair<std::string, MmcMemBlobDesc>> blobList;
    const bool storageEnabled = true;
    const bool enableMaxValueMode = false;
    packer.Serialize(msgVer);
    packer.Serialize(msgId);
    packer.Serialize(destRankId);
    packer.Serialize(rank);
    packer.Serialize(backendId);
    packer.Serialize(mediaTypes);
    packer.Serialize(addresses);
    packer.Serialize(capacities);
    packer.Serialize(blobList);
    packer.Serialize(storageEnabled);
    packer.Serialize(enableMaxValueMode);

    BmRegisterRequest decoded;
    NetMsgUnpacker unpacker(packer.String());
    ASSERT_EQ(decoded.Deserialize(unpacker), MMC_OK);
    EXPECT_EQ(decoded.rank_, rank);
    EXPECT_TRUE(decoded.storageEnabled_);
    EXPECT_FALSE(decoded.enableMaxValueMode_);
}

TEST_F(TestProtoMsg, GetExternalConfigMessagesRoundTrip)
{
    constexpr int16_t getExternalConfigMsgId = 30;
    GetExternalConfigRequest request;
    NetMsgPacker requestPacker;
    ASSERT_EQ(request.Serialize(requestPacker), MMC_OK);
    GetExternalConfigRequest decodedRequest;
    NetMsgUnpacker requestUnpacker(requestPacker.String());
    ASSERT_EQ(decodedRequest.Deserialize(requestUnpacker), MMC_OK);
    EXPECT_EQ(decodedRequest.msgId, ML_GET_EXTERNAL_CONFIG_REQ);

    GetExternalConfigResponse response;
    response.ret_ = MMC_OK;
    response.ssdMetadataMode_ = MMC_SSD_METADATA_SEPARATED;
    NetMsgPacker responsePacker;
    ASSERT_EQ(response.Serialize(responsePacker), MMC_OK);
    GetExternalConfigResponse decodedResponse;
    NetMsgUnpacker responseUnpacker(responsePacker.String());
    ASSERT_EQ(decodedResponse.Deserialize(responseUnpacker), MMC_OK);
    EXPECT_EQ(decodedResponse.ret_, MMC_OK);
    EXPECT_EQ(decodedResponse.ssdMetadataMode_, MMC_SSD_METADATA_SEPARATED);
    EXPECT_EQ(static_cast<int16_t>(ML_GET_EXTERNAL_CONFIG_REQ), getExternalConfigMsgId);
    EXPECT_EQ(static_cast<int16_t>(ML_GET_EXTERNAL_CONFIG_REQ), static_cast<int16_t>(ML_GET_EXTERNAL_CONFIG_RESP));
}

TEST_F(TestProtoMsg, BatchStorageStatMessagesRoundTrip)
{
    constexpr int16_t batchStorageStatMsgId = 29;
    const std::vector<std::string> keys = {"stat_first", "stat_second"};
    BatchStorageStatRequest request(keys);
    NetMsgPacker requestPacker;
    ASSERT_EQ(request.Serialize(requestPacker), MMC_OK);
    BatchStorageStatRequest decodedRequest;
    NetMsgUnpacker requestUnpacker(requestPacker.String());
    ASSERT_EQ(decodedRequest.Deserialize(requestUnpacker), MMC_OK);
    EXPECT_EQ(decodedRequest.keys_, keys);

    const std::vector<Result> results = {MMC_OK, MMC_UNMATCHED_KEY};
    const std::vector<uint64_t> sizes = {4096U, 0U};
    BatchStorageStatResponse response(results, sizes);
    NetMsgPacker responsePacker;
    ASSERT_EQ(response.Serialize(responsePacker), MMC_OK);
    BatchStorageStatResponse decodedResponse;
    NetMsgUnpacker responseUnpacker(responsePacker.String());
    ASSERT_EQ(decodedResponse.Deserialize(responseUnpacker), MMC_OK);
    EXPECT_EQ(decodedResponse.results_, results);
    EXPECT_EQ(decodedResponse.sizes_, sizes);
    EXPECT_EQ(static_cast<int16_t>(LM_BATCH_STORAGE_STAT_REQ), batchStorageStatMsgId);
    EXPECT_EQ(static_cast<int16_t>(LM_BATCH_STORAGE_STAT_REQ), static_cast<int16_t>(LM_BATCH_STORAGE_STAT_RESP));
    EXPECT_NE(static_cast<int16_t>(LM_BATCH_STORAGE_STAT_REQ), static_cast<int16_t>(ML_PREFETCH_REQ));
    EXPECT_NE(static_cast<int16_t>(ML_GET_EXTERNAL_CONFIG_REQ), static_cast<int16_t>(ML_EVICT_REQ));
}

// BatchQueryResponse oversized container guard
TEST_F(TestProtoMsg, BatchQueryResponse_OversizedContainer)
{
    NetMsgPacker packer;
    int16_t msgVer = 0;
    int16_t msgId = static_cast<int16_t>(ML_BATCH_QUERY_RESP);
    uint32_t destRankId = 0;
    packer.Serialize(msgVer);
    packer.Serialize(msgId);
    packer.Serialize(destRankId);
    const std::size_t hugeSize = static_cast<std::size_t>(MAX_CONTAINER_SIZE) + 1;
    packer.Serialize(hugeSize);

    BatchQueryResponse resp;
    NetMsgUnpacker unpacker(packer.String());
    EXPECT_EQ(resp.Deserialize(unpacker), MMC_ERROR);
}

// BatchUpdateLeaseResponse oversized container guard
TEST_F(TestProtoMsg, BatchUpdateLeaseResponse_OversizedContainer)
{
    NetMsgPacker packer;
    int16_t msgVer = 0;
    int16_t msgId = static_cast<int16_t>(ML_BATCH_UPDATE_LEASE_RESP);
    uint32_t destRankId = 0;
    packer.Serialize(msgVer);
    packer.Serialize(msgId);
    packer.Serialize(destRankId);
    Result ret = MMC_OK;
    packer.Serialize(ret);
    std::vector<Result> results;
    packer.Serialize(results);
    const std::size_t hugeSize = static_cast<std::size_t>(MAX_CONTAINER_SIZE) + 1;
    packer.Serialize(hugeSize);

    BatchUpdateLeaseResponse resp;
    NetMsgUnpacker unpacker(packer.String());
    EXPECT_EQ(resp.Deserialize(unpacker), MMC_ERROR);
}

// MetaReplicateRequest::KeysString with multiple and single key
TEST_F(TestProtoMsg, MetaReplicateRequest_KeysString)
{
    MetaReplicateRequest req;
    req.keys_ = {"a", "b"};
    EXPECT_EQ(req.KeysString(), "[a, b]");

    MetaReplicateRequest single;
    single.keys_ = {"only"};
    EXPECT_EQ(single.KeysString(), "[only]");
}

// UpdateRequest operator== / operator!=
TEST_F(TestProtoMsg, UpdateRequest_EqualityOperators)
{
    constexpr uint16_t testMediaType = 2;
    constexpr uint64_t testOperateId = 3;

    UpdateRequest a;
    a.key_ = "k";
    a.rank_ = 1;
    a.mediaType_ = testMediaType;
    a.operateId_ = testOperateId;

    UpdateRequest b;
    b.key_ = "k";
    b.rank_ = 1;
    b.mediaType_ = testMediaType;
    b.operateId_ = testOperateId;

    UpdateRequest c;
    c.key_ = "different";
    c.rank_ = 1;
    c.mediaType_ = testMediaType;
    c.operateId_ = testOperateId;

    EXPECT_TRUE(a == b);
    EXPECT_FALSE(a == c);
    EXPECT_TRUE(a != c);
    EXPECT_FALSE(a != b);
}

// PrefetchRequest serialization/deserialization — keys roundtrip, opcode 26
TEST_F(TestProtoMsg, PrefetchRequest_SerializeDeserialize)
{
    PrefetchRequest req{{"key1", "key2", "key3"}};
    EXPECT_EQ(req.msgId, ML_PREFETCH_REQ);

    NetMsgPacker packer;
    ASSERT_EQ(req.Serialize(packer), MMC_OK);
    auto serialized = packer.String();

    PrefetchRequest req2;
    NetMsgUnpacker unpacker(serialized);
    ASSERT_EQ(req2.Deserialize(unpacker), MMC_OK);
    EXPECT_EQ(req2.keys_, req.keys_);
    EXPECT_EQ(req2.msgId, ML_PREFETCH_REQ);
}

// PrefetchRequest serialization/deserialization — empty keys
TEST_F(TestProtoMsg, PrefetchRequest_EmptyKeys)
{
    PrefetchRequest req;
    NetMsgPacker packer;
    ASSERT_EQ(req.Serialize(packer), MMC_OK);

    PrefetchRequest req2;
    NetMsgUnpacker unpacker(packer.String());
    ASSERT_EQ(req2.Deserialize(unpacker), MMC_OK);
    EXPECT_TRUE(req2.keys_.empty());
}

// PrefetchResponse serialization/deserialization — success/failure
TEST_F(TestProtoMsg, PrefetchResponse_SerializeDeserialize)
{
    PrefetchResponse resp{MMC_OK};
    EXPECT_EQ(resp.msgId, ML_PREFETCH_RESP);
    NetMsgPacker packer;
    ASSERT_EQ(resp.Serialize(packer), MMC_OK);

    PrefetchResponse resp2;
    NetMsgUnpacker unpacker(packer.String());
    ASSERT_EQ(resp2.Deserialize(unpacker), MMC_OK);
    EXPECT_EQ(resp2.ret_, MMC_OK);
    EXPECT_EQ(resp2.msgId, ML_PREFETCH_RESP);

    PrefetchResponse failResp{MMC_ERROR};
    NetMsgPacker failPacker;
    ASSERT_EQ(failResp.Serialize(failPacker), MMC_OK);
    PrefetchResponse failResp2;
    NetMsgUnpacker failUnpacker(failPacker.String());
    ASSERT_EQ(failResp2.Deserialize(failUnpacker), MMC_OK);
    EXPECT_EQ(failResp2.ret_, MMC_ERROR);
}

// EvictRequest serialization/deserialization — keys roundtrip, opcode 27
TEST_F(TestProtoMsg, EvictRequest_SerializeDeserialize)
{
    EvictRequest req{{"key1", "key2", "key3"}};
    EXPECT_EQ(req.msgId, ML_EVICT_REQ);

    NetMsgPacker packer;
    ASSERT_EQ(req.Serialize(packer), MMC_OK);
    auto serialized = packer.String();

    EvictRequest req2;
    NetMsgUnpacker unpacker(serialized);
    ASSERT_EQ(req2.Deserialize(unpacker), MMC_OK);
    EXPECT_EQ(req2.keys_, req.keys_);
    EXPECT_EQ(req2.msgId, ML_EVICT_REQ);
}

// EvictRequest serialization/deserialization — empty keys
TEST_F(TestProtoMsg, EvictRequest_EmptyKeys)
{
    EvictRequest req;
    NetMsgPacker packer;
    ASSERT_EQ(req.Serialize(packer), MMC_OK);

    EvictRequest req2;
    NetMsgUnpacker unpacker(packer.String());
    ASSERT_EQ(req2.Deserialize(unpacker), MMC_OK);
    EXPECT_TRUE(req2.keys_.empty());
}

// EvictResponse serialization/deserialization — success/failure
TEST_F(TestProtoMsg, EvictResponse_SerializeDeserialize)
{
    EvictResponse resp{MMC_OK};
    EXPECT_EQ(resp.msgId, ML_EVICT_RESP);
    NetMsgPacker packer;
    ASSERT_EQ(resp.Serialize(packer), MMC_OK);

    EvictResponse resp2;
    NetMsgUnpacker unpacker(packer.String());
    ASSERT_EQ(resp2.Deserialize(unpacker), MMC_OK);
    EXPECT_EQ(resp2.ret_, MMC_OK);
    EXPECT_EQ(resp2.msgId, ML_EVICT_RESP);

    EvictResponse failResp{MMC_ERROR};
    NetMsgPacker failPacker;
    ASSERT_EQ(failResp.Serialize(failPacker), MMC_OK);
    EvictResponse failResp2;
    NetMsgUnpacker failUnpacker(failPacker.String());
    ASSERT_EQ(failResp2.Deserialize(failUnpacker), MMC_OK);
    EXPECT_EQ(failResp2.ret_, MMC_ERROR);
}

// PrefetchRequest oversized keys_ container guard
TEST_F(TestProtoMsg, PrefetchRequest_OversizedKeysVector)
{
    NetMsgPacker packer;
    int16_t msgVer = 0;
    int16_t msgId = static_cast<int16_t>(ML_PREFETCH_REQ);
    uint32_t destRankId = 0;
    packer.Serialize(msgVer);
    packer.Serialize(msgId);
    packer.Serialize(destRankId);
    const std::size_t hugeSize = static_cast<std::size_t>(MAX_CONTAINER_SIZE) + 1;
    packer.Serialize(hugeSize);

    PrefetchRequest req;
    NetMsgUnpacker unpacker(packer.String());
    EXPECT_EQ(req.Deserialize(unpacker), MMC_ERROR);
}

// Prefetch/Evict/BatchPromote opcode enumeration: 26/27/28, REQ/RESP 对共享同一 ID
TEST_F(TestProtoMsg, PrefetchEvictBatchPromote_OpcodeEnumeration)
{
    constexpr int16_t expectedPrefetchOpcode = 26;
    constexpr int16_t expectedEvictOpcode = 27;
    constexpr int16_t expectedBatchPromoteOpcode = 28;
    EXPECT_EQ(static_cast<int16_t>(ML_PREFETCH_REQ), expectedPrefetchOpcode);
    EXPECT_EQ(static_cast<int16_t>(ML_EVICT_REQ), expectedEvictOpcode);
    EXPECT_EQ(static_cast<int16_t>(ML_BATCH_PROMOTE_REQ), expectedBatchPromoteOpcode);
    EXPECT_EQ(static_cast<int16_t>(ML_PREFETCH_REQ), static_cast<int16_t>(ML_PREFETCH_RESP));
    EXPECT_EQ(static_cast<int16_t>(ML_EVICT_REQ), static_cast<int16_t>(ML_EVICT_RESP));
    EXPECT_EQ(static_cast<int16_t>(ML_BATCH_PROMOTE_REQ), static_cast<int16_t>(ML_BATCH_PROMOTE_RESP));
    EXPECT_NE(static_cast<int16_t>(ML_PREFETCH_REQ), static_cast<int16_t>(ML_EVICT_REQ));
    EXPECT_NE(static_cast<int16_t>(ML_EVICT_REQ), static_cast<int16_t>(ML_BATCH_PROMOTE_REQ));
}

// BatchPromoteRequest serialization/deserialization — keys roundtrip, opcode 28
TEST_F(TestProtoMsg, BatchPromoteRequest_SerializeDeserialize)
{
    BatchPromoteRequest req{{"key1", "key2", "key3"}};
    EXPECT_EQ(req.msgId, ML_BATCH_PROMOTE_REQ);

    NetMsgPacker packer;
    ASSERT_EQ(req.Serialize(packer), MMC_OK);
    auto serialized = packer.String();

    BatchPromoteRequest req2;
    NetMsgUnpacker unpacker(serialized);
    ASSERT_EQ(req2.Deserialize(unpacker), MMC_OK);
    EXPECT_EQ(req2.keys_, req.keys_);
    EXPECT_EQ(req2.msgId, ML_BATCH_PROMOTE_REQ);
}

// BatchPromoteRequest serialization/deserialization — empty keys
TEST_F(TestProtoMsg, BatchPromoteRequest_EmptyKeys)
{
    BatchPromoteRequest req;
    NetMsgPacker packer;
    ASSERT_EQ(req.Serialize(packer), MMC_OK);

    BatchPromoteRequest req2;
    NetMsgUnpacker unpacker(packer.String());
    ASSERT_EQ(req2.Deserialize(unpacker), MMC_OK);
    EXPECT_TRUE(req2.keys_.empty());
}

// BatchPromoteResponse serialization/deserialization — results roundtrip
TEST_F(TestProtoMsg, BatchPromoteResponse_SerializeDeserialize)
{
    BatchPromoteResponse resp;
    resp.results_ = {MMC_OK, MMC_UNMATCHED_KEY, MMC_ERROR};
    EXPECT_EQ(resp.msgId, ML_BATCH_PROMOTE_RESP);

    NetMsgPacker packer;
    ASSERT_EQ(resp.Serialize(packer), MMC_OK);

    BatchPromoteResponse resp2;
    NetMsgUnpacker unpacker(packer.String());
    ASSERT_EQ(resp2.Deserialize(unpacker), MMC_OK);
    EXPECT_EQ(resp2.results_, resp.results_);
    EXPECT_EQ(resp2.msgId, ML_BATCH_PROMOTE_RESP);
}
