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
#include "mmc_ubs_io_proxy.h"

using namespace ock::mmc;

class TestUbsIoProxy : public ::testing::Test {
protected:
    void SetUp() override
    {
        proxy_ = MmcUbsIoProxyFactory::GetInstance("test");
        ASSERT_NE(proxy_, nullptr);
        result_ = proxy_->InitUbsIo();
        ASSERT_EQ(result_, MMC_OK);
    }

    void TearDown() override
    {
        if (proxy_ != nullptr) {
            proxy_->DestroyUbsIo();
        }
    }

    MmcUbsIoProxyPtr proxy_;
    Result result_;
};

TEST_F(TestUbsIoProxy, InitAndDestroy)
{
    // 测试重复初始化
    result_ = proxy_->InitUbsIo();
    ASSERT_EQ(result_, MMC_OK);

    // 测试销毁
    proxy_->DestroyUbsIo();

    // 测试重新初始化
    result_ = proxy_->InitUbsIo();
    ASSERT_EQ(result_, MMC_OK);
}

TEST_F(TestUbsIoProxy, PutAndGet)
{
    // 测试基本的Put和Get操作
    std::string key = "test_key";
    std::string value = "test_value";

    // Put操作
    result_ = proxy_->Put(key, const_cast<char *>(value.c_str()), value.size());
    ASSERT_EQ(result_, MMC_OK);

    // Get操作
    char buffer[100] = {0};
    result_ = proxy_->Get(key, buffer, sizeof(buffer));
    ASSERT_EQ(result_, MMC_OK);
    ASSERT_STREQ(buffer, value.c_str());
}

TEST_F(TestUbsIoProxy, GetNonExistent)
{
    // 测试获取不存在的键
    std::string key = "non_existent_key";
    char buffer[100] = {0};
    result_ = proxy_->Get(key, buffer, sizeof(buffer));
    ASSERT_NE(result_, MMC_OK);
}

TEST_F(TestUbsIoProxy, Exist)
{
    // 测试Exist操作
    std::string key = "exist_test_key";
    std::string value = "exist_test_value";

    // 检查不存在
    result_ = proxy_->Exist(key);
    ASSERT_NE(result_, true);

    // Put操作
    result_ = proxy_->Put(key, const_cast<char *>(value.c_str()), value.size());
    ASSERT_EQ(result_, MMC_OK);

    // 检查存在
    result_ = proxy_->Exist(key);
    ASSERT_EQ(result_, true);
}

TEST_F(TestUbsIoProxy, Delete)
{
    // 测试Delete操作
    std::string key = "delete_test_key";
    std::string value = "delete_test_value";

    // Put操作
    result_ = proxy_->Put(key, const_cast<char *>(value.c_str()), value.size());
    ASSERT_EQ(result_, MMC_OK);

    // 检查存在
    result_ = proxy_->Exist(key);
    ASSERT_EQ(result_, true);

    // Delete操作
    result_ = proxy_->Delete(key);
    ASSERT_EQ(result_, MMC_OK);

    // 检查不存在
    result_ = proxy_->Exist(key);
    ASSERT_NE(result_, true);
}

TEST_F(TestUbsIoProxy, GetLength)
{
    // 测试GetLength操作
    std::string key = "length_test_key";
    std::string value = "length_test_value";
    size_t length = 0;

    // Put操作
    result_ = proxy_->Put(key, const_cast<char *>(value.c_str()), value.size());
    ASSERT_EQ(result_, MMC_OK);

    // GetLength操作
    result_ = proxy_->GetLength(key, length);
    ASSERT_EQ(result_, MMC_OK);
    ASSERT_EQ(length, value.size());
}

TEST_F(TestUbsIoProxy, BatchPutAndGet)
{
    // 测试批量Put和Get操作
    std::vector<std::string> keys = {"batch_key1", "batch_key2", "batch_key3"};
    std::vector<std::string> values = {"batch_value1", "batch_value2", "batch_value3"};
    std::vector<void *> bufs;
    std::vector<size_t> lengths;
    std::vector<int> results(keys.size());

    // 准备数据
    for (const auto &value : values) {
        bufs.push_back(const_cast<char *>(value.c_str()));
        lengths.push_back(value.size());
    }

    // 批量Put操作
    result_ = proxy_->BatchPut(keys, bufs, lengths, results);
    ASSERT_EQ(result_, MMC_OK);
    for (int res : results) {
        ASSERT_EQ(res, 0);
    }

    // 批量Get操作
    std::vector<void *> get_bufs(keys.size(), nullptr);
    std::vector<size_t> get_lengths(keys.size(), 0);
    std::vector<int> get_results(keys.size());

    result_ = proxy_->BatchGet(keys, get_bufs.data(), get_lengths, get_results);
    ASSERT_EQ(result_, MMC_OK);
    for (int i = 0; i < get_results.size(); ++i) {
        ASSERT_EQ(get_results[i], 0);
        ASSERT_STREQ(static_cast<char *>(get_bufs[i]), values[i].c_str());
    }

    // 测试BatchGetFree操作（释放UBSIO分配的内存）
    result_ = proxy_->BatchGetFree(get_bufs.data(), get_bufs.size());
    ASSERT_EQ(result_, MMC_OK);
}

TEST_F(TestUbsIoProxy, BatchExist)
{
    // 测试批量Exist操作
    std::vector<std::string> keys = {"exist_key1", "exist_key2", "exist_key3"};
    std::vector<std::string> values = {"value1", "value2", "value3"};

    // 只Put前两个键
    for (int i = 0; i < 2; ++i) {
        result_ = proxy_->Put(keys[i], const_cast<char *>(values[i].c_str()), values[i].size());
        ASSERT_EQ(result_, MMC_OK);
    }

    // 批量Exist操作
    bool exist_results[3] = {false};
    result_ = proxy_->BatchExist(keys, exist_results);
    ASSERT_EQ(result_, MMC_OK);
    ASSERT_TRUE(exist_results[0]);
    ASSERT_TRUE(exist_results[1]);
    ASSERT_FALSE(exist_results[2]);
}

TEST_F(TestUbsIoProxy, BatchDelete)
{
    // 测试批量Delete操作
    std::vector<std::string> keys = {"delete_key1", "delete_key2", "delete_key3"};
    std::vector<std::string> values = {"value1", "value2", "value3"};

    // Put所有键
    for (int i = 0; i < keys.size(); ++i) {
        result_ = proxy_->Put(keys[i], const_cast<char *>(values[i].c_str()), values[i].size());
        ASSERT_EQ(result_, MMC_OK);
    }

    // 批量Delete操作
    std::vector<int32_t> delete_results(keys.size());
    result_ = proxy_->BatchDelete(keys, delete_results);
    ASSERT_EQ(result_, MMC_OK);
    for (int32_t res : delete_results) {
        ASSERT_EQ(res, 0);
    }

    // 检查所有键都不存在
    for (const auto &key : keys) {
        result_ = proxy_->Exist(key);
        ASSERT_NE(result_, true);
    }
}

TEST_F(TestUbsIoProxy, BatchGetLength)
{
    // 测试批量GetLength操作
    std::vector<std::string> keys = {"length_key1", "length_key2"};
    std::vector<std::string> values = {"short", "longer_value"};

    // Put所有键
    for (int i = 0; i < keys.size(); ++i) {
        result_ = proxy_->Put(keys[i], const_cast<char *>(values[i].c_str()), values[i].size());
        ASSERT_EQ(result_, MMC_OK);
    }

    // 批量GetLength操作
    std::vector<size_t> lengths(keys.size());
    std::vector<int32_t> results(keys.size());
    result_ = proxy_->BatchGetLength(keys, lengths, results);
    ASSERT_EQ(result_, MMC_OK);
    for (int i = 0; i < results.size(); ++i) {
        ASSERT_EQ(results[i], 0);
        ASSERT_EQ(lengths[i], values[i].size());
    }
}

TEST_F(TestUbsIoProxy, BatchStatReturnsSizesAndPerKeyResults)
{
    const std::vector<std::string> keys = {"batch_stat_present", "batch_stat_missing"};
    std::string value = "batch_stat_value";
    result_ = proxy_->Put(keys.front(), value.data(), value.size());
    ASSERT_EQ(result_, MMC_OK);

    std::vector<uint64_t> sizes;
    std::vector<Result> results;
    result_ = proxy_->BatchStat(keys, sizes, results);

    ASSERT_EQ(result_, MMC_OK);
    ASSERT_EQ(sizes.size(), keys.size());
    ASSERT_EQ(results.size(), keys.size());
    EXPECT_EQ(results.front(), MMC_OK);
    EXPECT_EQ(sizes.front(), value.size());
    EXPECT_EQ(results.back(), MMC_UNMATCHED_KEY);
    EXPECT_EQ(sizes.back(), 0U);
}

namespace {
template<typename Tag, typename Tag::Type member>
struct MetadataProxyAccessor {
    friend typename Tag::Type MetadataMember(Tag)
    {
        return member;
    }
};
struct MetadataQuerySymbol {
    using Type = UbsioGetUnderFsTypeFunc *;
    friend Type MetadataMember(MetadataQuerySymbol);
};
struct MetadataInitSymbol {
    using Type = ubsio_client_initFunc *;
    friend Type MetadataMember(MetadataInitSymbol);
};
struct MetadataCallbackSymbol {
    using Type = ubsio_register_meta_event_callbackFunc *;
    friend Type MetadataMember(MetadataCallbackSymbol);
};
struct MetadataExitSymbol {
    using Type = ubsio_kv_cache_exitFunc *;
    friend Type MetadataMember(MetadataExitSymbol);
};
template struct MetadataProxyAccessor<MetadataQuerySymbol, &DlUbsioApi::pUbsioGetUnderFsType>;
template struct MetadataProxyAccessor<MetadataInitSymbol, &DlUbsioApi::pUbsioClientInit>;
template struct MetadataProxyAccessor<MetadataCallbackSymbol, &DlUbsioApi::pUbsioRegisterMetaEventCallback>;
template struct MetadataProxyAccessor<MetadataExitSymbol, &DlUbsioApi::pUbsioKvCacheExit>;
UbsioMetaEventCallbackC gMetadataCallback = nullptr;
void *gMetadataContext = nullptr;
UbsioUnderFsType gUnderFsType = UBSIO_UNDERFS_NONE;
Result gMetadataQueryResult = MMC_OK;
bool gMetadataInitAttempted = false;
size_t gMetadataExitCount = 0;
constexpr uint32_t kMetadataEventCount = 1;

void SendMetadataDelete(const std::string &key)
{
    UbsioMetaEventC event{UBSIO_META_DELETE_C, key.c_str(), static_cast<uint32_t>(key.size())};
    if (gMetadataCallback != nullptr) {
        gMetadataCallback(gMetadataContext, &event, kMetadataEventCount);
    }
}

int32_t MetadataInit(int32_t)
{
    gMetadataInitAttempted = true;
    return MMC_OK;
}

int32_t MetadataQuery(UbsioUnderFsType *type)
{
    if (type == nullptr) {
        return MMC_INVALID_PARAM;
    }
    *type = gUnderFsType;
    return gMetadataQueryResult;
}

int32_t MetadataRegisterCallback(UbsioMetaEventCallbackC callback, void *context)
{
    EXPECT_FALSE(gMetadataInitAttempted);
    gMetadataCallback = callback;
    gMetadataContext = context;
    return MMC_OK;
}

void MetadataExit()
{
    ++gMetadataExitCount;
    gMetadataCallback = nullptr;
    gMetadataContext = nullptr;
}

void InstallMetadataProxyStubs()
{
    *MetadataMember(MetadataInitSymbol{}) = MetadataInit;
    *MetadataMember(MetadataQuerySymbol{}) = MetadataQuery;
    *MetadataMember(MetadataCallbackSymbol{}) = MetadataRegisterCallback;
    *MetadataMember(MetadataExitSymbol{}) = MetadataExit;
    gMetadataQueryResult = MMC_OK;
    gMetadataInitAttempted = false;
    gMetadataExitCount = 0;
}

} // namespace

TEST(MetadataProxyTest, RegistersDeleteCallbackForAllUnderFsTypes)
{
    for (auto type :
         {UBSIO_UNDERFS_NONE, UBSIO_UNDERFS_CEPH, UBSIO_UNDERFS_HDFS, UBSIO_UNDERFS_LOCAL, UBSIO_UNDERFS_SHARED}) {
        const bool separated = type == UBSIO_UNDERFS_SHARED;
        ASSERT_EQ(DlUbsioApi::LoadLibrary(), MMC_OK);
        InstallMetadataProxyStubs();
        gUnderFsType = type;
        MmcUbsIoProxy proxy("metadata_events");
        std::vector<std::vector<std::string>> batches;
        proxy.SetMetaEventCallback([&](int type, const std::vector<std::string> &keys) {
            EXPECT_EQ(type, UBSIO_META_DELETE_C);
            batches.push_back(keys);
        });
        EXPECT_EQ(proxy.InitUbsIo(), MMC_OK);
        EXPECT_EQ(proxy.IsSsdMetadataSeparated(), separated);
        EXPECT_NE(gMetadataCallback, nullptr);
        SendMetadataDelete("runtime");
        ASSERT_EQ(batches.size(), kMetadataEventCount);
        EXPECT_EQ(batches.front(), (std::vector<std::string>{"runtime"}));
        proxy.SetMetaEventCallback(nullptr);
        proxy.DestroyUbsIo();
        EXPECT_FALSE(proxy.IsSsdMetadataSeparated());
        EXPECT_EQ(gMetadataExitCount, kMetadataEventCount);
    }
}

TEST(MetadataProxyTest, InitializesWithoutMetaEventCallback)
{
    for (auto type : {UBSIO_UNDERFS_NONE, UBSIO_UNDERFS_LOCAL, UBSIO_UNDERFS_SHARED}) {
        ASSERT_EQ(DlUbsioApi::LoadLibrary(), MMC_OK);
        InstallMetadataProxyStubs();
        gUnderFsType = type;
        MmcUbsIoProxy proxy("metadata_no_callback");
        EXPECT_EQ(proxy.InitUbsIo(), MMC_OK);
        EXPECT_TRUE(proxy.IsReady());
        EXPECT_TRUE(gMetadataInitAttempted);
        EXPECT_EQ(proxy.IsSsdMetadataSeparated(), type == UBSIO_UNDERFS_SHARED);
        EXPECT_EQ(gMetadataCallback, nullptr);
        EXPECT_EQ(gMetadataContext, nullptr);
        proxy.DestroyUbsIo();
        EXPECT_EQ(gMetadataExitCount, kMetadataEventCount);
    }
}

TEST(MetadataProxyTest, QueryFailureUsesDefaultAggregatedMetadata)
{
    for (const auto queryResult : {MMC_ERROR, MMC_NOT_INITIALIZED}) {
        ASSERT_EQ(DlUbsioApi::LoadLibrary(), MMC_OK);
        InstallMetadataProxyStubs();
        gUnderFsType = UBSIO_UNDERFS_SHARED;
        gMetadataQueryResult = queryResult;
        if (queryResult == MMC_NOT_INITIALIZED) {
            *MetadataMember(MetadataQuerySymbol{}) = nullptr;
        }
        MmcUbsIoProxy proxy("metadata_failure");
        bool callbackInvoked = false;
        proxy.SetMetaEventCallback([&](int, const std::vector<std::string> &) { callbackInvoked = true; });
        EXPECT_EQ(proxy.InitUbsIo(), MMC_OK);
        EXPECT_TRUE(proxy.IsReady());
        EXPECT_FALSE(proxy.IsSsdMetadataSeparated());
        EXPECT_FALSE(callbackInvoked);
        EXPECT_NE(gMetadataCallback, nullptr);
        EXPECT_EQ(gMetadataExitCount, 0U);
        SendMetadataDelete("runtime");
        EXPECT_TRUE(callbackInvoked);
        proxy.SetMetaEventCallback(nullptr);
        proxy.DestroyUbsIo();
        EXPECT_EQ(gMetadataExitCount, kMetadataEventCount);
    }
}

TEST(MetadataProxyTest, UnknownUnderFsTypeUsesAggregatedMetadata)
{
    constexpr auto beyondRange = static_cast<UbsioUnderFsType>(UBSIO_UNDERFS_BUTT + 1);
    for (auto unknownType : {UBSIO_UNDERFS_BUTT, beyondRange}) {
        ASSERT_EQ(DlUbsioApi::LoadLibrary(), MMC_OK);
        InstallMetadataProxyStubs();
        gUnderFsType = unknownType;
        UbsioUnderFsType type = UBSIO_UNDERFS_LOCAL;
        EXPECT_EQ(DlUbsioApi::UbsioGetUnderFsType(type), MMC_OK);
        EXPECT_EQ(type, unknownType);
        MmcUbsIoProxy proxy("unknown_underfs_type");
        std::vector<std::string> deletedKeys;
        proxy.SetMetaEventCallback([&](int, const std::vector<std::string> &keys) {
            deletedKeys.insert(deletedKeys.end(), keys.begin(), keys.end());
        });
        EXPECT_EQ(proxy.InitUbsIo(), MMC_OK);
        EXPECT_TRUE(proxy.IsReady());
        EXPECT_FALSE(proxy.IsSsdMetadataSeparated());
        SendMetadataDelete("runtime");
        EXPECT_EQ(deletedKeys, (std::vector<std::string>{"runtime"}));
        proxy.SetMetaEventCallback(nullptr);
        proxy.DestroyUbsIo();
        EXPECT_FALSE(proxy.IsReady());
        EXPECT_EQ(gMetadataExitCount, kMetadataEventCount);
    }
}
