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

#ifndef MF_HYBRID_MMC_META_LEASE_MANAGER_H
#define MF_HYBRID_MMC_META_LEASE_MANAGER_H

#include "mmc_define.h"
#include "mmc_logger.h"
#include "mmc_montotonic.h"
#include "mmc_types.h"

namespace ock {
namespace mmc {

class MmcMetaLeaseManager {
public:
    explicit MmcMetaLeaseManager(uint64_t defaultTtlMs = MMC_DATA_TTL_MS)
        : defaultTtlMs_(defaultTtlMs == 0 ? MMC_DATA_TTL_MS : defaultTtlMs)
    {}

    Result Add(uint32_t id, uint32_t requestId, uint64_t ttl);
    Result Wait();
    inline uint64_t RemainingLeaseTtlMs() const;
    inline uint64_t DefaultTtlMs() const;
    inline void SetDefaultTtlMs(uint64_t defaultTtlMs);

    friend std::ostream &operator<<(std::ostream &os, const MmcMetaLeaseManager &leaseMgr)
    {
        os << "lease={" << leaseMgr.lease_ << "}";
        return os;
    }

private:
    uint64_t defaultTtlMs_{MMC_DATA_TTL_MS};
    uint64_t lease_{0}; /* lease of the memory object */
};

inline uint64_t MmcMetaLeaseManager::RemainingLeaseTtlMs() const
{
    const uint64_t nowMs = ock::dagger::Monotonic::TimeUs() / 1000ULL;
    return lease_ > nowMs ? (lease_ - nowMs) : 0;
}

inline uint64_t MmcMetaLeaseManager::DefaultTtlMs() const
{
    return defaultTtlMs_;
}

inline void MmcMetaLeaseManager::SetDefaultTtlMs(uint64_t defaultTtlMs)
{
    defaultTtlMs_ = defaultTtlMs == 0 ? MMC_DATA_TTL_MS : defaultTtlMs;
}

} // namespace mmc
} // namespace ock

#endif // MF_HYBRID_MMC_META_LEASE_MANAGER_H
