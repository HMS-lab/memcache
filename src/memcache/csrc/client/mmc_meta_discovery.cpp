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
#include "mmc_meta_discovery.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "mmc_logger.h"
#include "smem_bm_api.h"

namespace ock {
namespace mmc {
namespace {
// 与 LocalService 的 etcd 发现策略保持一致：2s 间隔、最长 60s。
constexpr int K_DISCOVERY_RETRY_INTERVAL_SEC = 2;
constexpr int K_DISCOVERY_MAX_WAIT_SEC = 60;
constexpr size_t K_META_IP_BUF_SIZE = 64;

template<size_t N>
void CopyToFixed(char (&dst)[N], const char *src)
{
    if (src == nullptr) {
        return;
    }
    std::snprintf(dst, N, "%s", src);
}
} // namespace

bool IsEtcdStoreUrl(const char *storeUrl)
{
    // 免去 std::string 构造：该函数在发现重试循环中被反复调用。
    constexpr char kEtcdScheme[] = "etcd://";
    return storeUrl != nullptr && std::strncmp(storeUrl, kEtcdScheme, sizeof(kEtcdScheme) - 1) == 0;
}

std::string DiscoverMetaServiceUrlByStoreUrl(const std::string &storeUrl, const mmc_tls_config *tlsOption,
                                             uint32_t worldSize)
{
    if (!IsEtcdStoreUrl(storeUrl.c_str())) {
        return "";
    }
    char metaIp[K_META_IP_BUF_SIZE] = {0};
    uint16_t metaPort = 0;
    smem_tls_config tls{};
    if (tlsOption != nullptr) {
        tls.tlsEnable = tlsOption->tlsEnable;
        CopyToFixed(tls.caPath, tlsOption->caPath);
        CopyToFixed(tls.crlPath, tlsOption->crlPath);
        CopyToFixed(tls.certPath, tlsOption->certPath);
        CopyToFixed(tls.keyPath, tlsOption->keyPath);
        CopyToFixed(tls.keyPassPath, tlsOption->keyPassPath);
        CopyToFixed(tls.packagePath, tlsOption->packagePath);
        CopyToFixed(tls.decrypterLibPath, tlsOption->decrypterLibPath);
    }
    auto ret = MFSmemApi::SmemBmGetBindableLeaderAddressByStoreUrl(storeUrl.c_str(), &tls, worldSize, metaIp,
                                                                   sizeof(metaIp), &metaPort);
    if (ret != MMC_OK || metaIp[0] == '\0' || metaPort == 0) {
        MMC_LOG_DEBUG("discover MetaService by store url failed, ret: " << ret << ", storeUrl: " << storeUrl);
        return "";
    }
    return "tcp://" + std::string(metaIp) + ":" + std::to_string(metaPort);
}

std::string DiscoverMetaServiceUrlWithRetry(const std::string &storeUrl, const mmc_tls_config *tlsOption,
                                            uint32_t worldSize, uint32_t rankForLog, const std::atomic<bool> *stopFlag)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(K_DISCOVERY_MAX_WAIT_SEC);
    while (std::chrono::steady_clock::now() < deadline) {
        if (stopFlag != nullptr && stopFlag->load(std::memory_order_relaxed)) {
            MMC_LOG_WARN("stop requested, abort MetaService discovery, rank=" << rankForLog);
            return "";
        }
        const std::string url = DiscoverMetaServiceUrlByStoreUrl(storeUrl, tlsOption, worldSize);
        if (!url.empty()) {
            return url;
        }
        MMC_LOG_WARN("MetaService addr not found in backend yet, rank=" << rankForLog << ", retry in "
                                                                        << K_DISCOVERY_RETRY_INTERVAL_SEC << "s");
        if (std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::seconds(K_DISCOVERY_RETRY_INTERVAL_SEC));
    }
    MMC_LOG_ERROR("discover MetaService addr timeout, rank=" << rankForLog << ", storeUrl: " << storeUrl << ", waited "
                                                             << K_DISCOVERY_MAX_WAIT_SEC << "s");
    return "";
}
} // namespace mmc
} // namespace ock
