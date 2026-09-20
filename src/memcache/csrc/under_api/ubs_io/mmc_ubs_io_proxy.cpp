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
#include <algorithm>
#include <numeric>
#include <vector>
#include <cstdint>
#include <string>
#include "mmc_logger.h"
#include "mmc_ptracer.h"
#include "dl_ubsio_api.h"
#include "mmc_ubs_io_proxy.h"

namespace ock {
namespace mmc {
namespace {
// UBSIO KVC 批接口的整体返回码，与逐 key 返回的 BoostIO CResult 属于不同错误域。
enum class UbsioKvcError : Result {
    OK = 0,
    ERROR = 1,
    INNER_ERROR = 2,
    INVALID_PARAM = 3,
    ALLOC_FAILED = 4,
    TRY_AGAIN = 5,
    BIO_ERROR = 6,
    NO_NDS = 7,
    MAX = 8,
};

// BatchStat、BatchDelete 等接口中每个 key 返回的 BoostIO CResult。
enum class UbsioCacheError : Result {
    OK = 0,
    PROTECTED = 1,
    ERROR = 2,
    INVALID_PARAM = 3,
    BUSY = 4,
    NEED_RETRY = 5,
    NOT_READY = 6,
    NOT_FOUND = 7,
    CONFLICT = 8,
    MISS = 9,
    NO_SPACE = 10,
    UNAVAILABLE = 11,
    EXCEED_QUOTA = 12,
    PARTITION_FAULT = 13,
    READ_EXCEED = 14,
    ALREADY_EXISTS = 15,
    DISK_FAULT = 16,
    UNDERFS_FAULT = 17,
    MAX = 18,
};

Result NormalizeUbsioItemResult(Result result)
{
    // UBSIO 使用正数 CResult，而 MemCache 公开 API 使用稳定的负数错误码。
    // 只有逐 key 的 NOT_FOUND 表示缓存未命中，其他正数均表示执行失败。
    const auto error = static_cast<UbsioCacheError>(result);
    if (error == UbsioCacheError::OK) {
        return MMC_OK;
    }
    return error == UbsioCacheError::NOT_FOUND ? MMC_UNMATCHED_KEY : MMC_ERROR;
}

Result NormalizeUbsioCallResult(Result result)
{
    if (result <= MMC_OK) {
        return result;
    }
    const auto error = static_cast<UbsioKvcError>(result);
    if (error == UbsioKvcError::INVALID_PARAM) {
        return MMC_INVALID_PARAM;
    }
    if (error == UbsioKvcError::BIO_ERROR) {
        return MMC_SSD_NOT_AVAILABLE;
    }
    return MMC_ERROR;
}
} // namespace

std::map<std::string, MmcRef<MmcUbsIoProxy>> MmcUbsIoProxyFactory::instances_;
std::mutex MmcUbsIoProxyFactory::instanceMutex_;

Result MmcUbsIoProxy::InitUbsIo(int32_t deviceId, const std::string &confPath)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_) {
        MMC_LOG_TRACE("MmcUbsIoProxy " << name_ << " already init");
        return MMC_OK;
    }

    Result result = DlUbsioApi::LoadLibrary();
    if (result != MMC_OK) {
        MMC_LOG_ERROR("Failed to load ubsio library, deviceId=" << deviceId << ", error: " << result);
        return result;
    }
    // Register meta event callback before UbsioClientInit to avoid missing recovery events
    if (metaEventCallback_ != nullptr) {
        result = RegisterMetaEventCallback();
        if (result != MMC_OK) {
            MMC_LOG_ERROR("Failed to register meta event callback, error: " << result);
            DlUbsioApi::CleanupLibrary();
            return result;
        }
    }
    result = DlUbsioApi::UbsioClientInit(deviceId, confPath);
    if (result != MMC_OK) {
        MMC_LOG_ERROR("Failed to init ubsio, deviceId=" << deviceId << ", error: " << result);
        DlUbsioApi::CleanupLibrary();
        return result;
    }

    started_ = true;
    MMC_LOG_TRACE("InitUbsIo success, deviceId=" << deviceId);
    return MMC_OK;
}

void MmcUbsIoProxy::DestroyUbsIo()
{
    if (started_) {
        DlUbsioApi::CleanupLibrary();
        started_ = false;
        MMC_LOG_TRACE("DestroyUbsIo completed");
    }
}

void MmcUbsIoProxy::StaticMetaEventCallback(void *context, const UbsioMetaEventC *events, uint32_t count)
{
    if (context == nullptr || events == nullptr || count == 0) {
        return;
    }
    auto *proxy = static_cast<MmcUbsIoProxy *>(context);
    if (proxy->metaEventCallback_ == nullptr) {
        return;
    }
    TP_TRACE_BEGIN(TP_MMC_UBS_IO_META_EVENT_CB);
    std::vector<std::string> deleteKeys;
    for (uint32_t i = 0; i < count; ++i) {
        if (events[i].type == UBSIO_META_DELETE_C) {
            deleteKeys.emplace_back(events[i].key, events[i].keyLen);
        }
    }
    if (!deleteKeys.empty()) {
        proxy->metaEventCallback_(UBSIO_META_DELETE_C, deleteKeys);
    }
    TP_TRACE_END(TP_MMC_UBS_IO_META_EVENT_CB, deleteKeys.size());
}

Result MmcUbsIoProxy::GetResourceInfo(UbsioResourceInfo &info) const
{
    return DlUbsioApi::UbsioGetResourceInfo(info);
}

Result MmcUbsIoProxy::RegisterMetaEventCallback()
{
    return DlUbsioApi::UbsioRegisterMetaEventCallback(&StaticMetaEventCallback, this);
}

Result MmcUbsIoProxy::Put(const std::string &key, void *buf, size_t length)
{
    MMC_ASSERT_LOG_AND_RETURN(started_, "started_ = " << started_, MMC_NOT_INITIALIZED);
    MMC_ASSERT_LOG_AND_RETURN(buf != nullptr, "buf is nullptr", MMC_INVALID_PARAM);
    MMC_ASSERT_LOG_AND_RETURN(!key.empty(), "key is empty", MMC_INVALID_PARAM);

    uint32_t flags = 0;
    TP_TRACE_BEGIN(TP_MMC_UBS_IO_PUT);
    int32_t ret = DlUbsioApi::UbsioPut(key.c_str(), buf, length, flags);
    TP_TRACE_END(TP_MMC_UBS_IO_PUT, ret);
    if (ret != MMC_OK) {
        MMC_LOG_ERROR("ubsIo Put failed, key=" << key << ", ret=" << ret);
    }
    return ret;
}

Result MmcUbsIoProxy::Get(const std::string &key, void *buf, size_t length)
{
    MMC_ASSERT_LOG_AND_RETURN(started_, "started_ = " << started_, MMC_NOT_INITIALIZED);
    MMC_ASSERT_LOG_AND_RETURN(buf != nullptr, "buf is nullptr", MMC_INVALID_PARAM);
    MMC_ASSERT_LOG_AND_RETURN(!key.empty(), "key is empty", MMC_INVALID_PARAM);

    uint32_t flags = 0;
    TP_TRACE_BEGIN(TP_MMC_UBS_IO_GET);
    int32_t ret = DlUbsioApi::UbsioGet(key.c_str(), buf, length, flags);
    TP_TRACE_END(TP_MMC_UBS_IO_GET, ret);
    if (ret != MMC_OK) {
        MMC_LOG_ERROR("ubsIo Get failed, key=" << key << ", ret=" << ret);
    }
    return ret;
}

bool MmcUbsIoProxy::Exist(const std::string &key)
{
    MMC_ASSERT_LOG_AND_RETURN(started_, "started_ = " << started_, false);
    MMC_ASSERT_LOG_AND_RETURN(!key.empty(), "key is empty", false);

    uint32_t flags = 0;
    TP_TRACE_BEGIN(TP_MMC_UBS_IO_EXIST);
    bool exists = DlUbsioApi::UbsioExist(key.c_str(), flags);
    TP_TRACE_END(TP_MMC_UBS_IO_EXIST, exists ? MMC_OK : MMC_UNMATCHED_KEY);
    return exists;
}

Result MmcUbsIoProxy::Delete(const std::string &key)
{
    MMC_ASSERT_LOG_AND_RETURN(started_, "started_ = " << started_, MMC_NOT_INITIALIZED);
    MMC_ASSERT_LOG_AND_RETURN(!key.empty(), "key is empty", MMC_INVALID_PARAM);

    uint32_t flags = 0;
    TP_TRACE_BEGIN(TP_MMC_UBS_IO_DELETE);
    int32_t ret = DlUbsioApi::UbsioDelete(key.c_str(), flags);
    TP_TRACE_END(TP_MMC_UBS_IO_DELETE, ret);
    if (ret != MMC_OK) {
        MMC_LOG_ERROR("ubsIo Delete failed, key=" << key << ", ret=" << ret);
    }
    return ret;
}

Result MmcUbsIoProxy::GetLength(const std::string &key, size_t &length)
{
    MMC_ASSERT_LOG_AND_RETURN(started_, "started_ = " << started_, MMC_NOT_INITIALIZED);
    MMC_ASSERT_LOG_AND_RETURN(!key.empty(), "key is empty", MMC_INVALID_PARAM);

    uint32_t flags = 0;
    size_t tempLength = 0;
    TP_TRACE_BEGIN(TP_MMC_UBS_IO_LENGTH);
    int32_t ret = DlUbsioApi::UbsioGetLength(key.c_str(), &tempLength, flags);
    TP_TRACE_END(TP_MMC_UBS_IO_LENGTH, ret);
    if (ret == 0) {
        length = tempLength;
        return MMC_OK;
    }
    return MMC_ERROR;
}

Result MmcUbsIoProxy::BatchPut(const std::vector<std::string> &keys, const std::vector<void *> &bufs,
                               const std::vector<size_t> &lengths, std::vector<int> &results)
{
    MMC_ASSERT_LOG_AND_RETURN(started_, "started_ = " << started_, MMC_NOT_INITIALIZED);
    MMC_ASSERT_LOG_AND_RETURN(!keys.empty(), "keys are empty", MMC_INVALID_PARAM);
    MMC_ASSERT_LOG_AND_RETURN(keys.size() == bufs.size(),
                              "keys.size() = " << keys.size() << ", bufs.size() = " << bufs.size(), MMC_INVALID_PARAM);
    MMC_ASSERT_LOG_AND_RETURN(keys.size() == lengths.size(),
                              "keys.size() = " << keys.size() << ", lengths.size() = " << lengths.size(),
                              MMC_INVALID_PARAM);

    const uint32_t keysCount = static_cast<uint32_t>(keys.size());
    std::vector<const char *> keyPtrs;
    keyPtrs.reserve(keysCount);
    for (const auto &key : keys) {
        keyPtrs.emplace_back(key.c_str());
    }
    std::vector<void *> bufferPtrs(bufs.begin(), bufs.end());
    std::vector<size_t> lengthCopy(lengths.begin(), lengths.end());

    uint32_t flags = 0;

    TP_TRACE_BEGIN(TP_MMC_UBS_IO_BATCH_PUT);
    int32_t ret = DlUbsioApi::UbsioBatchPut(keyPtrs.data(), keysCount, bufferPtrs.data(), lengthCopy.data(),
                                            results.data(), flags);
    TP_TRACE_END(TP_MMC_UBS_IO_BATCH_PUT, ret);
    return ret;
}

Result MmcUbsIoProxy::BatchGet(const std::vector<std::string> &keys, void **bufs, std::vector<size_t> &lengths,
                               std::vector<int> &results)
{
    MMC_ASSERT_LOG_AND_RETURN(started_, "started_ = " << started_, MMC_NOT_INITIALIZED);
    MMC_ASSERT_LOG_AND_RETURN(!keys.empty(), "keys are empty", MMC_INVALID_PARAM);
    MMC_ASSERT_LOG_AND_RETURN(keys.size() == lengths.size(),
                              "keys.size() = " << keys.size() << ", lengths.size() = " << lengths.size(),
                              MMC_INVALID_PARAM);

    const uint32_t keysCount = static_cast<uint32_t>(keys.size());
    std::vector<const char *> keyPtrs;
    keyPtrs.reserve(keysCount);
    for (const auto &key : keys) {
        keyPtrs.emplace_back(key.c_str());
    }

    uint32_t flags = 0;

    TP_TRACE_BEGIN(TP_MMC_UBS_IO_BATCH_GET);
    int32_t ret = DlUbsioApi::UbsioBatchGet(keyPtrs.data(), keysCount, bufs, lengths.data(), results.data(), flags);
    TP_TRACE_END(TP_MMC_UBS_IO_BATCH_GET, ret);
    return ret;
}

Result MmcUbsIoProxy::BatchGetWithHBM(const std::vector<std::string> &keys,
                                      std::vector<std::vector<void *>> &npuBufAddrs,
                                      std::vector<std::vector<size_t>> &npuBufLengths, std::vector<int> &results)
{
    MMC_ASSERT_LOG_AND_RETURN(started_, "started_ = " << started_, MMC_NOT_INITIALIZED);
    MMC_ASSERT_LOG_AND_RETURN(!keys.empty(), "keys are empty", MMC_INVALID_PARAM);
    MMC_ASSERT_LOG_AND_RETURN(keys.size() == npuBufAddrs.size() && keys.size() == npuBufLengths.size(),
                              "keys.size() = " << keys.size() << ", npuBufAddrs.size() = " << npuBufAddrs.size()
                                               << ", npuBufLengths.size() = " << npuBufLengths.size(),
                              MMC_INVALID_PARAM);
    uint32_t lengthsRows = keys.size();
    uint32_t lengthsCols = npuBufAddrs[0].size();
    for (uint32_t i = 1; i < lengthsRows; i++) {
        MMC_ASSERT_LOG_AND_RETURN(lengthsCols == npuBufAddrs[i].size(),
                                  "lengthsCols = " << lengthsCols << ", npuBufAddrs[" << i
                                                   << "].size() = " << npuBufAddrs[i].size(),
                                  MMC_INVALID_PARAM);
    }
    const uint32_t keysCount = static_cast<uint32_t>(keys.size());
    std::vector<const char *> keyPtrs;
    keyPtrs.reserve(keysCount);
    for (const auto &key : keys) {
        keyPtrs.emplace_back(key.c_str());
    }
    void ***bufs = new (std::nothrow) void **[lengthsRows];
    if (bufs == nullptr) {
        MMC_LOG_ERROR("alloc buf failed");
        return MMC_ERROR;
    }
    size_t **lengths = new (std::nothrow) size_t *[lengthsRows];
    if (lengths == nullptr) {
        MMC_LOG_ERROR("alloc length failed");
        delete[] bufs;
        return MMC_ERROR;
    }
    for (uint32_t i = 0; i < lengthsRows; i++) {
        bufs[i] = npuBufAddrs[i].data();
        lengths[i] = npuBufLengths[i].data();
    }

    uint32_t flags = 0;
    TP_TRACE_BEGIN(TP_MMC_UBS_IO_BATCH_GET);
    int32_t ret = DlUbsioApi::UbsioBatchGetWithHBM(keyPtrs.data(), keysCount, bufs, lengths, lengthsRows, lengthsCols,
                                                   results.data(), flags);
    TP_TRACE_END(TP_MMC_UBS_IO_BATCH_GET, ret);
    delete[] bufs;
    delete[] lengths;
    return ret;
}

Result MmcUbsIoProxy::BatchGetFree(void **bufs, int keysCount)
{
    MMC_ASSERT_LOG_AND_RETURN(started_, "started_ = " << started_, MMC_NOT_INITIALIZED);

    TP_TRACE_BEGIN(TP_MMC_UBS_IO_BATCH_FREE);
    int32_t ret = DlUbsioApi::UbsioBatchFreeAddress(bufs, keysCount);
    TP_TRACE_END(TP_MMC_UBS_IO_BATCH_FREE, ret);
    return ret;
}

Result MmcUbsIoProxy::BatchExist(const std::vector<std::string> &keys, bool *results)
{
    MMC_ASSERT_LOG_AND_RETURN(started_, "started_ = " << started_, MMC_NOT_INITIALIZED);
    MMC_ASSERT_LOG_AND_RETURN(!keys.empty(), "keys are empty", MMC_INVALID_PARAM);

    const uint32_t keysCount = static_cast<uint32_t>(keys.size());
    std::vector<const char *> keyPtrs;
    keyPtrs.reserve(keysCount);
    for (const auto &key : keys) {
        keyPtrs.emplace_back(key.c_str());
    }

    uint32_t flags = 0;

    TP_TRACE_BEGIN(TP_MMC_UBS_IO_BATCH_EXIST);
    int32_t ret = DlUbsioApi::UbsioBatchExist(keyPtrs.data(), keysCount, results, flags);
    TP_TRACE_END(TP_MMC_UBS_IO_BATCH_EXIST, ret);
    return ret;
}

Result MmcUbsIoProxy::BatchDelete(const std::vector<std::string> &keys, std::vector<int32_t> &results)
{
    MMC_ASSERT_LOG_AND_RETURN(started_, "started_ = " << started_, MMC_NOT_INITIALIZED);
    MMC_ASSERT_LOG_AND_RETURN(!keys.empty(), "keys are empty", MMC_INVALID_PARAM);

    const uint32_t keysCount = static_cast<uint32_t>(keys.size());
    std::vector<const char *> keyPtrs;
    keyPtrs.reserve(keysCount);
    for (const auto &key : keys) {
        keyPtrs.emplace_back(key.c_str());
    }

    uint32_t flags = 0;

    TP_TRACE_BEGIN(TP_MMC_UBS_IO_BATCH_DELETE);
    int32_t ret = DlUbsioApi::UbsioBatchDelete(keyPtrs.data(), keysCount, results.data(), flags);
    TP_TRACE_END(TP_MMC_UBS_IO_BATCH_DELETE, ret);
    return ret;
}

Result MmcUbsIoProxy::BatchGetLength(const std::vector<std::string> &keys, std::vector<size_t> &lengths,
                                     std::vector<int32_t> &results)
{
    MMC_ASSERT_LOG_AND_RETURN(started_, "started_ = " << started_, MMC_NOT_INITIALIZED);
    MMC_ASSERT_LOG_AND_RETURN(!keys.empty(), "keys are empty", MMC_INVALID_PARAM);

    const uint32_t keysCount = static_cast<uint32_t>(keys.size());
    std::vector<const char *> keyPtrs;
    keyPtrs.reserve(keysCount);
    for (const auto &key : keys) {
        keyPtrs.emplace_back(key.c_str());
    }

    lengths.assign(keysCount, 0);
    uint32_t flags = 0;

    TP_TRACE_BEGIN(TP_MMC_UBS_IO_BATCH_LENGTH);
    int32_t ret = DlUbsioApi::UbsioBatchGetLength(keyPtrs.data(), keysCount, lengths.data(), results.data(), flags);
    TP_TRACE_END(TP_MMC_UBS_IO_BATCH_LENGTH, ret);
    return ret;
}

Result MmcUbsIoProxy::BatchStat(const std::vector<std::string> &keys, std::vector<uint64_t> &sizes,
                                std::vector<Result> &results)
{
    MMC_ASSERT_LOG_AND_RETURN(started_, "started_ = " << started_, MMC_NOT_INITIALIZED);
    MMC_ASSERT_LOG_AND_RETURN(!keys.empty(), "keys are empty", MMC_INVALID_PARAM);

    const uint32_t keysCount = static_cast<uint32_t>(keys.size());
    std::vector<const char *> keyPtrs;
    keyPtrs.reserve(keysCount);
    for (const auto &key : keys) {
        keyPtrs.emplace_back(key.c_str());
    }

    sizes.assign(keysCount, 0);
    results.assign(keysCount, MMC_ERROR);
    constexpr uint32_t flags = 0;
    std::vector<UbsioKvBatchStat> stats(keysCount);
    TP_TRACE_BEGIN(TP_MMC_UBS_IO_BATCH_STAT);
    Result rawRet = DlUbsioApi::UbsioBatchStat(keyPtrs.data(), keysCount, stats.data(), flags);
    TP_TRACE_END(TP_MMC_UBS_IO_BATCH_STAT, rawRet);
    Result ret = NormalizeUbsioCallResult(rawRet);
    if (ret != MMC_OK) {
        std::fill(results.begin(), results.end(), ret);
        return ret;
    }
    for (size_t index = 0; index < stats.size(); ++index) {
        sizes[index] = stats[index].size;
        results[index] = NormalizeUbsioItemResult(stats[index].result);
    }
    return MMC_OK;
}
} // namespace mmc
} // namespace ock
