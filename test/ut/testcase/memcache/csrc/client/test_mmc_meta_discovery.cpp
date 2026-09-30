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
#include <gtest/gtest.h>

#include "mmc_meta_discovery.h"
#include "mmc_types.h"

using namespace ock::mmc;

namespace {
constexpr char K_ETCD_URL[] = "etcd://127.0.0.1:12379";
constexpr char K_TCP_URL[] = "tcp://127.0.0.1:7777";
constexpr char K_ETCD_URL_NO_KEY[] = "etcd://127.0.0.1:12380"; // mock 固定的入口无效时用于负例
} // namespace

TEST(TestMetaDiscovery, IsEtcdStoreUrlDistinguishesSchemes)
{
    EXPECT_TRUE(IsEtcdStoreUrl(K_ETCD_URL));
    EXPECT_FALSE(IsEtcdStoreUrl(K_TCP_URL));
    EXPECT_FALSE(IsEtcdStoreUrl(nullptr));
}

TEST(TestMetaDiscovery, DiscoverByStoreUrlReturnsPublishedAddress)
{
    // mock libmf_smem 固定发布 127.0.0.1:19001
    const std::string url = DiscoverMetaServiceUrlByStoreUrl(K_ETCD_URL, nullptr, 256);
    EXPECT_EQ(url, "tcp://127.0.0.1:19001");
}

TEST(TestMetaDiscovery, DiscoverByStoreUrlSkipsNonEtcd)
{
    EXPECT_TRUE(DiscoverMetaServiceUrlByStoreUrl(K_TCP_URL, nullptr, 256).empty());
}

TEST(TestMetaDiscovery, DiscoverWithRetryHonorsStopFlag)
{
    std::atomic<bool> stop{true};
    EXPECT_TRUE(DiscoverMetaServiceUrlWithRetry(K_ETCD_URL_NO_KEY, nullptr, 256, 0, &stop).empty());
}
