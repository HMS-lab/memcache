/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
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
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

// Load dependencies before changing access to the client under test.
#include "mmc_common_includes.h"
#include "mmc_meta_net_client.h"
#include "mmc_def.h"
#include "mmc_bm_proxy.h"
#include "mmc_interval_map.h"
#include "mmc_client_local_gva_blob_tracker.h"
#include "mmc_ubs_io_proxy.h"
#include "mmc_thread_pool.h"
#include "mmc_msg_client_meta.h"
#include "mmc_bandwidth_collector.h"

#define private public
#include "mmc_client_default.h"
#undef private

using namespace ock::mmc;

constexpr uint64_t TEST_GVA = 4096;
constexpr uint64_t TEST_SIZE = 4096;
constexpr uint64_t TEST_LEASE_TTL_MS = 5000;
constexpr uint64_t TEST_OPERATE_ID = 1;

TEST(ClientBatchAlloc, ReadableSsdOnlyReturnsSentinelWithoutRegistration)
{
    for (uint64_t ssdGva : {uint64_t{0}, UINT64_MAX}) {
        MmcClientDefault client("batch_alloc_test");
        const std::vector<MmcMemBlobDesc> blobs{{0, ssdGva, TEST_SIZE, MEDIA_SSD, READABLE, 0}};
        uintptr_t gva = 0;
        EXPECT_EQ(client.RegisterBatchAllocBlobs("ssd_key", blobs, TEST_OPERATE_ID, gva), MMC_OK);
        EXPECT_EQ(gva, static_cast<uintptr_t>(UINT64_MAX));
        LocalGvaBlobInfo info;
        EXPECT_EQ(client.gvaBlobTracker_.FindBlobByKey("ssd_key", info), MMC_UNMATCHED_KEY);
    }
}

TEST(ClientBatchAlloc, MixedMediaSelectsResidentBlobRegardlessOfOrder)
{
    const MmcMemBlobDesc ssdBlob{0, 0, TEST_SIZE, MEDIA_SSD, READABLE, 0};
    for (MediaType media : {MEDIA_DRAM, MEDIA_HBM}) {
        const MmcMemBlobDesc memoryBlob{0,         TEST_GVA,         TEST_SIZE, static_cast<uint16_t>(media),
                                        ALLOCATED, TEST_LEASE_TTL_MS};
        for (const auto &blobs :
             {std::vector<MmcMemBlobDesc>{ssdBlob, memoryBlob}, std::vector<MmcMemBlobDesc>{memoryBlob, ssdBlob}}) {
            MmcClientDefault client("batch_alloc_test");
            uintptr_t gva = 0;
            ASSERT_EQ(client.RegisterBatchAllocBlobs("memory_key", blobs, TEST_OPERATE_ID, gva), MMC_OK);
            EXPECT_EQ(gva, static_cast<uintptr_t>(TEST_GVA));
            LocalGvaBlobInfo info;
            ASSERT_EQ(client.gvaBlobTracker_.FindWritable(TEST_GVA, TEST_SIZE, info), MMC_OK);
            EXPECT_EQ(info.blob.mediaType_, static_cast<uint16_t>(media));
        }
    }
}

TEST(ClientBatchAlloc, InvalidBlobsLeaveFailureGvaWithoutRegistration)
{
    const std::vector<MmcMemBlobDesc> invalidBlobs{
        {0, 0, TEST_SIZE, MEDIA_DRAM, ALLOCATED, TEST_LEASE_TTL_MS},
        {0, UINT64_MAX, TEST_SIZE, MEDIA_DRAM, ALLOCATED, TEST_LEASE_TTL_MS},
        {0, TEST_GVA, 0, MEDIA_DRAM, ALLOCATED, TEST_LEASE_TTL_MS},
        {0, 0, TEST_SIZE, MEDIA_SSD, ALLOCATED, 0},
        {0, 0, 0, MEDIA_SSD, READABLE, 0},
    };
    for (const auto &blob : invalidBlobs) {
        MmcClientDefault client("batch_alloc_test");
        uintptr_t gva = 0;
        EXPECT_NE(client.RegisterBatchAllocBlobs("invalid_key", {blob}, TEST_OPERATE_ID, gva), MMC_OK);
        EXPECT_EQ(gva, static_cast<uintptr_t>(0));
        LocalGvaBlobInfo info;
        EXPECT_EQ(client.gvaBlobTracker_.FindBlobByKey("invalid_key", info), MMC_UNMATCHED_KEY);
    }
}

TEST(ClientBatchAlloc, UninitializedClientReturnsZeroForEveryKey)
{
    MmcClientDefault client("batch_alloc_test");
    const std::vector<std::string> keys{"key_a", "key_b"};
    const std::vector<size_t> sizes(keys.size(), static_cast<size_t>(TEST_SIZE));
    std::vector<uintptr_t> gvas(keys.size(), static_cast<uintptr_t>(UINT64_MAX));
    const mmc_put_options options{};
    EXPECT_EQ(client.BatchMalloc(keys, sizes, options, TEST_LEASE_TTL_MS, gvas), MMC_CLIENT_NOT_INIT);
    EXPECT_EQ(gvas, std::vector<uintptr_t>(keys.size(), 0));
}
