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

    // 注册一个内存贡献节点（LocalService/BM）：将其提供的各介质内存池（bm）纳入集群可分配空间，
    // 并登记 rank -> backendId 关联，供节点亲和分配（local_first）识别写者/内存节点拓扑。
    // 参数说明：
    //   rank          该 LocalService 进程的全局 rank id（内存提供者身份）
    //   mediaType     各内存池介质类型（HBM/DDR/SSD...），与 bm/capacity 一一对应
    //   bm            各介质内存池起始地址（base mem），长度与 mediaType 一致
    //   capacity      各介质内存池容量，长度与 mediaType 一致
    //   blobList      已存在的内存块列表（注册时随附，供元数据恢复）
    //   storageEnabled 是否启用 SSD 存储池
    //   backendId     节点的 backendId，通常取节点连接 IP/域名；理想情况下同机器的多个
    //                 LocalService share 同一 backendId。用途：1) 节点亲和分配按 backendId 维度
    //                 选择目标；2) kvevent key存入删除事件/广播。
    //   enableMaxValueMode 是否启用padding模式，用于解决外部碎片问题
    //   role          节点角色："writer"（写者，如 P，会有数据写入）/ "memory"（纯内存提供者，如 D，
    //                 只贡献内存不参与写入）。节点亲和分配时，写者的分布表按角色构建，
    //                 让写者均匀地把数据分流到内存节点上。
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
    // 节点亲和分配拓扑：backendId 角色("writer"/"memory") 与写者分布表，注册/注销时自动重算。
    std::string GetBackendRole(const std::string &backendId);
    // 节点亲和分配：按 caller rank 查分布表，权重选出一个目标 backendId；无配置/非写者返回空串（调用方回退随机）。
    std::string SelectTargetBackendForRank(uint32_t callerRank);
    // 节点亲和分配（ALLOC_LOCAL_FIRST）：将 options.preferredRank_ 扩展为目标 backendId 的全部 ranks 并随机打乱。
    // callerRank 由请求的 rankId_ 字段显式携带，不依赖 options.preferredRank_ 内容；非 LOCAL_FIRST 直接返回。
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
    // 共用读写锁：1. 保护 backendId/rank 映射; 2.backendId 角色表与节点亲和分布表（读多写少，读共享、写独占）
    std::shared_mutex rankBackendIdMapLock_;
    std::unordered_map<uint32_t, std::string> rankBackendIdMap_;
    std::unordered_map<std::string, std::unordered_set<uint32_t>> backendIdRankMap_;
    std::unordered_map<std::string, std::string> backendIdRoleMap_; // backendId -> "writer"/"memory"
    std::vector<BackendDistribution> writeDistributionTable_; // 节点亲和分配缓存，LocalService注册/注销时重算
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
