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
#ifndef MEM_FABRIC_MMC_META_SERVICE_DEFAULT_H
#define MEM_FABRIC_MMC_META_SERVICE_DEFAULT_H

#include <atomic>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "mmc_def.h"
#include "mmc_global_allocator.h"
#include "mmc_locality_distribution.h"
#include "mmc_meta_net_server.h"
#include "mmc_meta_mgr_proxy.h"
#include "mmc_periodic_task.h"
#include "smem_config_store_api.h"
#include "kv_event/mmc_kv_event_runtime.h"

namespace ock {
namespace mmc {
class MmcMetaService : public MmcReferable {
public:
    explicit MmcMetaService(const std::string &name) : name_(name), options_{} {}

    Result Start(const mmc_meta_service_config_t &options);

    // Hot-reload meta service tunables (lease TTL, evict thresholds, rewarm watermark, prefetch)
    // without restart. Validates and pushes to MmcMetaMgrProxy; options_ is updated only on success.
    Result UpdateConfig(const mmc_meta_service_config_t &options);

    // Hot-reload the defrag eviction waste percent threshold independently of the evict watermarks.
    Result UpdateDefragEvictThreshold(uint16_t defragEvictThreshold);

    void Stop();

    // Register a memory-contributing node (LocalService/BM): add its per-medium pools (bm) to cluster allocatable
    // space,
    // and record rank -> backendId for local_first node-affinity allocation to identify writer/memory topology.
    // Parameters:
    // rank          Global rank ID of this LocalService process (memory-provider identity).
    // mediaType     Pool media types (HBM/DDR/SSD...), corresponding one-to-one with bm/capacity.
    // bm            Base addresses of the memory pools; same length as mediaType.
    // capacity      Pool capacities; same length as mediaType.
    // blobList      Existing blocks supplied at registration for metadata recovery.
    // storageEnabled Whether to enable the SSD storage pool.
    // backendId     Node backendId, usually connection IP/domain. Ideally, LocalServices on the same machine
    // share one backendId. Used for 1) target selection at backendId granularity in node-affinity allocation;
    //               2) kvevent key storage/deletion events and broadcasts.
    // enableMaxValueMode Whether to enable padding mode to address external fragmentation.
    // role          Node role: "writer" (for example, P, writes data) / "memory" (for example, D,
    //               contributes memory without writing). Node-affinity distribution tables are built by role
    //               to distribute writers' data evenly to memory nodes.
    Result BmRegister(uint32_t rank, std::vector<uint16_t> mediaType, std::vector<uint64_t> bm,
                      std::vector<uint64_t> capacity, std::vector<std::pair<std::string, MmcMemBlobDesc>> &blobList,
                      bool storageEnabled = false, const std::string &backendId = "", const std::string &role = "");

    Result BmUnregister(uint32_t rank, uint16_t mediaType);

    Result ClearResource(uint32_t rank);

    Result ClearAllResources();

    const std::string &Name() const;

    const mmc_meta_service_config_t &Options() const;

    const MmcMetaMgrProxyPtr &GetMetaMgrProxy() const;

    bool IsConfigStoreReady() const;

    Result GetMetadata(const std::string &key, std::string &value, int64_t timeoutMs = 0);

    Result PutMetadata(const std::string &key, const std::string &value);

    Result DeleteMetadata(const std::string &key);

    void SetPublishActive(bool active);
    void SetPeriodicEvictActive(bool active);
    bool KvEventsEnabled() const;
    kv_event::KvEventStats GetKvEventStats() const;

    void OnBecomeLeader();
    void OnBecomeStandby();

    std::string GetBackendIdForRank(uint32_t rank);
    std::vector<uint32_t> GetRanksForBackendId(const std::string &backendId);
    std::vector<std::string> GetAllBackendIds();
    // Node-affinity topology: backendId roles ("writer"/"memory") and writer distributions, rebuilt on
    // registration/unregistration.
    std::string GetBackendRole(const std::string &backendId);
    // Node affinity: look up caller rank distribution and select a weighted backendId; return empty if
    // unconfigured/non-writer for random fallback.
    std::string SelectTargetBackendForRank(uint32_t callerRank);
    // Node affinity (ALLOC_LOCAL_FIRST): expand options.preferredRank_ to all target-backend ranks and shuffle.
    // callerRank is explicit in request rankId_, independent of options.preferredRank_; return immediately unless
    // LOCAL_FIRST.
    void ExpandLocalFirstRanks(AllocOptions &options, uint32_t callerRank);

private:
    void RebuildWriteDistributionTable();
    void RegisterHaLeaderCallback();
    Result StartMetaNetServer();
    void StopMetaNetServer();
    Result BmUnregisterWithoutLock(uint32_t rank, uint16_t mediaType, bool deadRank = false);
    void PublishClearedForRanks(const std::vector<uint32_t> &ranks);

private:
    MetaNetServerPtr metaNetServer_;
    MmcMetaMgrProxyPtr metaMgrProxy_;
    MMCMetaBackUpMgrPtr metaBackUpMgrPtr_;

    std::mutex mutex_;
    bool started_ = false;
    bool metaNetServerPending_{false};
    std::string metaServiceAddr_;
    std::string name_;
    mmc_meta_service_config_t options_;
    NetEngineOptions netOptions_;
    std::unordered_map<uint32_t, std::unordered_set<uint16_t>> rankMediaTypeMap_;
    // Shared read/write lock protects 1) backendId/rank mappings, 2) role and affinity tables (frequent shared reads,
    // exclusive writes).
    std::shared_mutex rankBackendIdMapLock_;
    std::unordered_map<uint32_t, std::string> rankBackendIdMap_;
    std::unordered_map<std::string, std::unordered_set<uint32_t>> backendIdRankMap_;
    std::unordered_map<std::string, std::string> backendIdRoleMap_; // backendId -> "writer"/"memory"
    // Node-affinity cache, rebuilt when LocalServices register/unregister.
    std::vector<BackendDistribution> writeDistributionTable_;
    std::unordered_map<std::string, std::string> metadata_;
    ock::smem::StorePtr confStore_ = nullptr;
    MmcKvEventRuntime kvEvents_;
    bool kvEventsPublishActive_{false};
    std::atomic<bool> periodicEvictActive_{false};
    void StartMetricsReportTask();
    bool IsSsdAvailable(uint32_t rank) const;
    void OnAsyncFlushComplete(uint32_t rank, const std::vector<std::pair<std::string, MmcMemBlobDesc>> &blobs);
};
inline const std::string &MmcMetaService::Name() const
{
    return name_;
}

inline const mmc_meta_service_config_t &MmcMetaService::Options() const
{
    return options_;
}

inline const MmcMetaMgrProxyPtr &MmcMetaService::GetMetaMgrProxy() const
{
    return metaMgrProxy_;
}

inline bool MmcMetaService::IsConfigStoreReady() const
{
    return confStore_ != nullptr;
}

using MmcMetaServiceDefaultPtr = MmcRef<MmcMetaService>;
} // namespace mmc
} // namespace ock

#endif // MEM_FABRIC_MMC_META_SERVICE_DEFAULT_H
