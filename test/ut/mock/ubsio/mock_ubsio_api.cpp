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

#include <cstring>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <algorithm>

#include "mmc_ubs_io_types.h"

constexpr int32_t K_MOCK_UBSIO_ERROR = -1;
constexpr int32_t K_MOCK_UBSIO_NOT_FOUND = 7;
constexpr uint32_t K_NULL_TERMINATOR_SIZE = 1;

// 全局存储模拟
std::map<std::string, std::string> gUbsioStorage;
std::mutex gUbsioMutex;

// 存储分配的内存，以便后续释放
std::vector<void *> gAllocatedBuffers;
std::mutex gBufferMutex;

// UBS IO meta event types
typedef struct {
    int32_t type;
    const char *key;
    uint32_t keyLen;
} UbsioMetaEventC;

typedef void (*UbsioMetaEventCallbackC)(void *context, const UbsioMetaEventC *events, uint32_t count);

// 初始化函数
extern "C" int32_t UbsioKvCacheInit(int32_t deviceId)
{
    (void)deviceId;
    return 0;
}

// 注册元数据事件回调
extern "C" int32_t UbsioKvCacheRegisterMetaEventCallback(UbsioMetaEventCallbackC callback, void *context)
{
    (void)callback;
    (void)context;
    return 0;
}

// 写入函数
extern "C" int32_t UbsioKvCachePut(const char *key, void *buf, size_t length, uint32_t flags)
{
    (void)flags;
    if (key == nullptr || buf == nullptr) {
        return -1;
    }

    std::lock_guard<std::mutex> lock(gUbsioMutex);
    std::string keyStr(key);
    std::string valueStr(static_cast<char *>(buf), length);
    gUbsioStorage[keyStr] = valueStr;
    return 0;
}

// 读取函数
extern "C" int32_t UbsioKvCacheGet(const char *key, void *buf, size_t length, uint32_t flags)
{
    (void)flags;
    if (key == nullptr || buf == nullptr) {
        return -1;
    }

    std::lock_guard<std::mutex> lock(gUbsioMutex);
    std::string keyStr(key);
    auto it = gUbsioStorage.find(keyStr);
    if (it == gUbsioStorage.end()) {
        return -1;
    }

    const std::string &value = it->second;
    if (value.size() > length) {
        return -1;
    }

    memcpy(buf, value.c_str(), value.size());
    return 0;
}

// 检查存在函数
extern "C" bool UbsioKvCacheExist(const char *key, uint32_t flags)
{
    (void)flags;
    if (key == nullptr) {
        return false;
    }

    std::lock_guard<std::mutex> lock(gUbsioMutex);
    std::string keyStr(key);
    return gUbsioStorage.find(keyStr) != gUbsioStorage.end();
}

// 删除函数
extern "C" int32_t UbsioKvCacheDelete(const char *key, uint32_t flags)
{
    (void)flags;
    if (key == nullptr) {
        return -1;
    }

    std::lock_guard<std::mutex> lock(gUbsioMutex);
    std::string keyStr(key);
    size_t erased = gUbsioStorage.erase(keyStr);
    return erased > 0 ? 0 : -1;
}

// 获取长度函数
extern "C" int32_t UbsioKvCacheGetLength(const char *key, size_t *length, uint32_t flags)
{
    (void)flags;
    if (key == nullptr || length == nullptr) {
        return -1;
    }

    std::lock_guard<std::mutex> lock(gUbsioMutex);
    std::string keyStr(key);
    auto it = gUbsioStorage.find(keyStr);
    if (it == gUbsioStorage.end()) {
        return -1;
    }

    *length = it->second.size();
    return 0;
}

// 批量写入函数
extern "C" int32_t UbsioKvCacheBatchPut(const char **keys, uint32_t keys_count, void **bufs, size_t *lengths,
                                        int *results, uint32_t flags)
{
    (void)flags;
    if (keys == nullptr || bufs == nullptr || lengths == nullptr || results == nullptr) {
        return -1;
    }

    std::lock_guard<std::mutex> lock(gUbsioMutex);
    for (uint32_t i = 0; i < keys_count; ++i) {
        if (keys[i] == nullptr || bufs[i] == nullptr) {
            results[i] = -1;
            continue;
        }

        std::string keyStr(keys[i]);
        std::string valueStr(static_cast<char *>(bufs[i]), lengths[i]);
        gUbsioStorage[keyStr] = valueStr;
        results[i] = 0;
    }
    return 0;
}

// 批量读取函数
extern "C" int32_t UbsioKvCacheBatchGet(const char **keys, uint32_t keys_count, void **bufs, size_t *lengths,
                                        int *results, uint32_t flags)
{
    (void)flags;
    if (keys == nullptr || bufs == nullptr || lengths == nullptr || results == nullptr) {
        return -1;
    }

    std::lock_guard<std::mutex> lock(gUbsioMutex);
    for (uint32_t i = 0; i < keys_count; ++i) {
        if (keys[i] == nullptr) {
            results[i] = -1;
            continue;
        }

        std::string keyStr(keys[i]);
        auto it = gUbsioStorage.find(keyStr);
        if (it == gUbsioStorage.end()) {
            results[i] = -1;
            continue;
        }

        const std::string &value = it->second;
        // UBSIO为buf分配内存
        void *allocatedBuf = malloc(value.size() + 1);
        if (allocatedBuf == nullptr) {
            results[i] = -1;
            continue;
        }

        memcpy(allocatedBuf, value.c_str(), value.size());
        static_cast<char *>(allocatedBuf)[value.size()] = '\0';

        // 存储分配的内存
        {
            std::lock_guard<std::mutex> bufLock(gBufferMutex);
            gAllocatedBuffers.push_back(allocatedBuf);
        }

        // 设置返回值
        bufs[i] = allocatedBuf;
        lengths[i] = value.size();
        results[i] = 0;
    }
    return 0;
}

// 批量检查存在函数
extern "C" int32_t UbsioKvCacheBatchExist(const char **keys, uint32_t keys_count, bool *results, uint32_t flags)
{
    (void)flags;
    if (keys == nullptr || results == nullptr) {
        return -1;
    }

    std::lock_guard<std::mutex> lock(gUbsioMutex);
    for (uint32_t i = 0; i < keys_count; ++i) {
        if (keys[i] == nullptr) {
            results[i] = false;
            continue;
        }

        std::string keyStr(keys[i]);
        results[i] = (gUbsioStorage.find(keyStr) != gUbsioStorage.end());
    }
    return 0;
}

// 批量删除函数
extern "C" int32_t UbsioKvCacheBatchDelete(const char **keys, uint32_t keys_count, int32_t *results, uint32_t flags)
{
    (void)flags;
    if (keys == nullptr || results == nullptr) {
        return -1;
    }

    std::lock_guard<std::mutex> lock(gUbsioMutex);
    for (uint32_t i = 0; i < keys_count; ++i) {
        if (keys[i] == nullptr) {
            results[i] = -1;
            continue;
        }

        std::string keyStr(keys[i]);
        size_t erased = gUbsioStorage.erase(keyStr);
        results[i] = (erased > 0) ? 0 : -1;
    }
    return 0;
}

// 批量获取长度函数
extern "C" int32_t UbsioKvCacheBatchGetLength(const char **keys, uint32_t keys_count, size_t *lengths, int32_t *results,
                                              uint32_t flags)
{
    (void)flags;
    if (keys == nullptr || lengths == nullptr || results == nullptr) {
        return -1;
    }

    std::lock_guard<std::mutex> lock(gUbsioMutex);
    for (uint32_t i = 0; i < keys_count; ++i) {
        if (keys[i] == nullptr) {
            results[i] = -1;
            continue;
        }

        std::string keyStr(keys[i]);
        auto it = gUbsioStorage.find(keyStr);
        if (it == gUbsioStorage.end()) {
            results[i] = -1;
            continue;
        }

        lengths[i] = it->second.size();
        results[i] = 0;
    }
    return 0;
}

extern "C" int32_t UbsioKvCacheBatchStat(const char **keys, uint32_t keysCount, UbsioKvBatchStat *stats, uint32_t flags)
{
    (void)flags;
    if (keys == nullptr || stats == nullptr) {
        return K_MOCK_UBSIO_ERROR;
    }

    std::lock_guard<std::mutex> lock(gUbsioMutex);
    for (uint32_t index = 0; index < keysCount; ++index) {
        stats[index] = {};
        if (keys[index] == nullptr) {
            stats[index].result = K_MOCK_UBSIO_ERROR;
            continue;
        }
        std::strncpy(stats[index].key, keys[index], UBSIO_KV_MAX_KEY_SIZE - K_NULL_TERMINATOR_SIZE);
        auto iter = gUbsioStorage.find(keys[index]);
        if (iter == gUbsioStorage.end()) {
            stats[index].result = K_MOCK_UBSIO_NOT_FOUND;
            continue;
        }
        stats[index].size = static_cast<uint32_t>(iter->second.size());
        stats[index].result = 0;
    }
    return 0;
}

// 批量释放地址函数
extern "C" int32_t UbsioKvCacheBatchFree(void **bufs, uint32_t keys_count)
{
    if (bufs == nullptr) {
        return -1;
    }

    std::lock_guard<std::mutex> bufLock(gBufferMutex);
    for (uint32_t i = 0; i < keys_count; ++i) {
        if (bufs[i] != nullptr) {
            // 释放分配的内存
            free(bufs[i]);

            // 从跟踪列表中移除
            auto it = std::find(gAllocatedBuffers.begin(), gAllocatedBuffers.end(), bufs[i]);
            if (it != gAllocatedBuffers.end()) {
                gAllocatedBuffers.erase(it);
            }
        }
    }
    return 0;
}

// 退出清理函数
extern "C" int32_t UbsioKvCacheExit()
{
    return 0;
}

// 批量直接读取函数（带HBM）
extern "C" int32_t UbsioKvCacheBatchGetDirect(const char **keys, uint32_t keys_count, void ***bufs, size_t **lengths,
                                              uint32_t lengths_rows, uint32_t lengths_cols, int *results,
                                              uint32_t flags)
{
    (void)flags;
    (void)bufs;
    (void)lengths;
    (void)lengths_rows;
    (void)lengths_cols;
    if (keys == nullptr || results == nullptr) {
        return -1;
    }

    // 简化实现，标记所有为成功
    for (uint32_t i = 0; i < keys_count; ++i) {
        results[i] = 0;
    }
    return 0;
}

// UBS IO scan key result structures, ABI-compatible with dl_ubsio_api.h
constexpr uint32_t UBSIO_KV_MAX_KEY_SIZE_MOCK = UBSIO_KV_MAX_KEY_SIZE;
constexpr uint64_t UBSIO_DISK_CAP_BYTES_MOCK = 1024ULL * 1024 * 1024 * 1024;
constexpr uint64_t UBSIO_MEM_CAP_BYTES_MOCK = 80ULL * 1024 * 1024 * 1024;

typedef struct {
    char key[UBSIO_KV_MAX_KEY_SIZE_MOCK];
    uint32_t keyLen;
    uint32_t reserved;
    uint64_t valueLen;
} UbsioKvKeyInfo;

// 已分配的扫描结果，等待释放
std::vector<UbsioKvKeyInfo *> gScanKeyResults;
std::mutex gScanKeyMutex;

// 查询资源信息
extern "C" int32_t UbsioGetResourceInfo(UbsioResourceInfo *info)
{
    if (info == nullptr) {
        return -1;
    }
    memset(info, 0, sizeof(UbsioResourceInfo));
    info->diskCap = UBSIO_DISK_CAP_BYTES_MOCK;
    info->diskUsed = 0;
    info->memCap = UBSIO_MEM_CAP_BYTES_MOCK;
    info->memUsed = 0;
    info->diskNum = 1;
    info->faultDiskNum = 0;
    return 0;
}

// 扫描 KV 缓存中的所有 key，一次性返回，hasMore 置 false
extern "C" int32_t UbsioKvCacheScanKey(const UbsioKvKeyInfo **items, uint64_t *count, bool *hasMore)
{
    if (items == nullptr || count == nullptr || hasMore == nullptr) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(gUbsioMutex);
    auto *result = static_cast<UbsioKvKeyInfo *>(calloc(gUbsioStorage.size(), sizeof(UbsioKvKeyInfo)));
    if (result == nullptr && !gUbsioStorage.empty()) {
        return -1;
    }
    uint64_t index = 0;
    for (const auto &entry : gUbsioStorage) {
        size_t keyLen = entry.first.size();
        if (keyLen >= UBSIO_KV_MAX_KEY_SIZE_MOCK) {
            keyLen = UBSIO_KV_MAX_KEY_SIZE_MOCK - 1;
        }
        memcpy(result[index].key, entry.first.c_str(), keyLen);
        result[index].key[keyLen] = '\0';
        result[index].keyLen = static_cast<uint32_t>(keyLen);
        result[index].reserved = 0;
        result[index].valueLen = entry.second.size();
        ++index;
    }
    *items = result;
    *count = gUbsioStorage.size();
    *hasMore = false;
    {
        std::lock_guard<std::mutex> scanLock(gScanKeyMutex);
        gScanKeyResults.push_back(result);
    }
    return 0;
}

// 释放扫描结果
extern "C" void UbsioKvCacheFreeScanKeyResult(const UbsioKvKeyInfo **items)
{
    if (items == nullptr || *items == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> scanLock(gScanKeyMutex);
    auto it = std::find(gScanKeyResults.begin(), gScanKeyResults.end(), *items);
    if (it != gScanKeyResults.end()) {
        free(*it);
        gScanKeyResults.erase(it);
    }
    *items = nullptr;
}
