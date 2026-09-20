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
#include "mmc_meta_service.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "mmc_logger.h"
#include "mmc_ref.h"
#include "mmc_client_metric_store.h"
#include "mmc_meta_mgr_proxy.h"
#include "mmc_meta_backup_mgr_factory.h"
#include "mmc_meta_metric_manager.h"
#include "mmc_meta_net_server.h"
#include "mmc_config_const.h"
#include "mmc_rest_api_facade.h"
#include "mmc_smem_bm_helper.h"
#include "spdlogger4c.h"
#include "spdlogger.h"

namespace ock {
namespace mmc {

namespace {

std::vector<uint32_t> CollectRanks(const std::unordered_map<uint32_t, std::unordered_set<uint16_t>> &rankMediaTypeMap)
{
    std::vector<uint32_t> ranks;
    ranks.reserve(rankMediaTypeMap.size());
    for (const auto &entry : rankMediaTypeMap) {
        ranks.push_back(entry.first);
    }
    return ranks;
}

} // namespace

Result MmcMetaService::Start(const mmc_meta_service_config_t &options)
{
    const int threadCountBase = 4;
    std::unique_lock<std::mutex> lock(mutex_);
    if (started_) {
        MMC_LOG_TRACE("MetaService " << name_ << " already started");
        return MMC_OK;
    }
    options_ = options;
    MMC_VALIDATE_RETURN(options.evictThresholdHigh > options.evictThresholdLow,
                        "invalid param, evictThresholdHigh must large than evictThresholdLow", MMC_INVALID_PARAM);
    options_.leaseTtlMs = options.leaseTtlMs == 0 ? MMC_DATA_TTL_MS : options.leaseTtlMs;
    MMC_VALIDATE_RETURN(options.asyncFlushIntervalMs >= MIN_FLUSH_INTERVAL_MS &&
                            options.asyncFlushIntervalMs <= MAX_FLUSH_INTERVAL_MS,
                        "invalid param, asyncFlushIntervalMs must be in range [" +
                            std::to_string(MIN_FLUSH_INTERVAL_MS) + ", " + std::to_string(MAX_FLUSH_INTERVAL_MS) + "]",
                        MMC_INVALID_PARAM);
    options_.asyncFlushIntervalMs = options.asyncFlushIntervalMs;
    MMC_VALIDATE_RETURN(options.asyncFlushBatchLimit >= MIN_FLUSH_BATCH_LIMIT &&
                            options.asyncFlushBatchLimit <= MAX_FLUSH_BATCH_LIMIT,
                        "invalid param, asyncFlushBatchLimit must be in range [" +
                            std::to_string(MIN_FLUSH_BATCH_LIMIT) + ", " + std::to_string(MAX_FLUSH_BATCH_LIMIT) + "]",
                        MMC_INVALID_PARAM);
    options_.asyncFlushBatchLimit = options.asyncFlushBatchLimit;
    MMC_VALIDATE_RETURN(options_.leaseTtlMs > 0, "invalid param, leaseTtlMs must be greater than 0", MMC_INVALID_PARAM);

    metaNetServer_ = MmcMakeRef<MetaNetServer>(this, name_ + "_MetaServer").Get();
    MMC_ASSERT_LOG_AND_RETURN(metaNetServer_.Get() != nullptr, "metaNetServer_.Get() is nullptr",
                              MMC_NEW_OBJECT_FAILED);
    /* init engine options — saved for later use in HA mode */
    std::string url{options_.discoveryURL};
    NetEngineOptions::ExtractIpPortFromUrl(url, netOptions_);
    netOptions_.name = name_;
    netOptions_.threadCount = threadCountBase;
    netOptions_.rankId = 0;
    netOptions_.startListener = true;
    netOptions_.tlsOption = options_.accTlsConfig;
    netOptions_.logFunc = SPDLOG_LogMessage;
    netOptions_.logLevel = options_.logLevel;

    metaMgrProxy_ = MmcMakeRef<MmcMetaMgrProxy>(metaNetServer_).Get();
    MmcMetaExtConfig extConfig{};
    extConfig.prefetchEnabled = options.prefetchEnabled;
    extConfig.pendingWaitTimeoutMs = options_.pendingWaitTimeoutMs;
    MMC_RETURN_ERROR(metaMgrProxy_->Start(options_.leaseTtlMs, options.evictThresholdHigh, options.evictThresholdLow,
                                          extConfig, options.evictYieldInterval, options.evictYieldDurationUs),
                     "Failed to start meta mgr proxy of meta service " << name_);

    metaBackUpMgrPtr_ = MMCMetaBackUpMgrFactory::GetInstance("DefaultMetaBackup");
    auto backupConf = MmcMakeRef<MMCMetaBackUpConfDefault>(metaNetServer_);
    MMC_ASSERT_LOG_AND_RETURN(backupConf.Get() != nullptr, "backupConf.Get() is nullptr", MMC_MALLOC_FAILED);
    MMC_ASSERT_LOG_AND_RETURN(metaBackUpMgrPtr_ != nullptr, "metaBackUpMgrPtr_ is nullptr", MMC_MALLOC_FAILED);
    backupConf->acquireReadLease = [this](const std::string &key, const MmcMemBlobDesc &source,
                                          BackupReadLease &lease) {
        return metaMgrProxy_->GetMetaManager()->AcquireBackupReadLease(key, source, lease);
    };
    backupConf->releaseReadLease = [this](const BackupReadLease &lease) {
        metaMgrProxy_->GetMetaManager()->ReleaseBackupReadLease(lease);
    };
    // backup 通道默认启动，服务于异步刷盘和 Rewarm
    MMC_LOG_TRACE("Meta backup channel starting, ha=" << options_.haEnable
                                                      << ", async_flush_interval=" << options_.asyncFlushIntervalMs);
    backupConf->Setup(
        options.asyncFlushIntervalMs, options.asyncFlushBatchLimit,
        std::bind(&MmcMetaService::IsSsdAvailable, this, std::placeholders::_1),
        std::bind(&MmcMetaService::OnAsyncFlushComplete, this, std::placeholders::_1, std::placeholders::_2));
    MMCMetaBackUpConfPtr defaultPtr = Convert<MMCMetaBackUpConfDefault, MMCMetaBackUpConf>(backupConf);
    MMC_RETURN_ERROR(metaBackUpMgrPtr_->Start(defaultPtr), "metaBackUpMgr start failed");

    NetEngineOptions configStoreOpt{};
    // etcd:// 等分布式后端由 MF 侧解析，此处仅解析 tcp/http/https。
    const std::string configStoreUrl(options_.configStoreURL);
    if (configStoreUrl.find("tcp://") == 0 || configStoreUrl.find("http://") == 0 ||
        configStoreUrl.find("https://") == 0) {
        NetEngineOptions::ExtractIpPortFromUrl(options_.configStoreURL, configStoreOpt);
    }
    smem::StoreFactory::SetTlsInfo(MmcSmemBmHelper::TransSmemTlsConfig(options_.configStoreTlsConfig));
    confStore_ =
        ock::smem::StoreFactory::CreateStoreByUrl(options_.configStoreURL, ock::smem::ConfigStoreModel::CSM_SERVER);
    MMC_VALIDATE_RETURN(confStore_ != nullptr, "Failed to start config store server", MMC_ERROR);

    // 注册心跳超时回调：mf 的 AccStoreServer 检测到 rank 心跳超时后，
    // 通过 GetRankIdByLinkId 映射到 rankId，触发 ClearResource 踢出集群
    confStore_->RegisterServerBrokenHandler([this](uint32_t linkId, ock::smem::StoreBackendPtr) {
        if (confStore_ == nullptr) {
            MMC_LOG_DEBUG("HeartBeat timeout after config store stopped, linkId=" << linkId);
            return MMC_OK;
        }
        uint32_t rankId = confStore_->GetRankIdByLinkId(linkId);
        if (rankId != UINT32_MAX) {
            MMC_LOG_WARN("HeartBeat timeout for rank=" << rankId << ", linkId=" << linkId << ", clearing resource");
            (void)ClearResource(rankId);
        } else {
            MMC_LOG_DEBUG("HeartBeat timeout for unknown linkId=" << linkId);
        }
        return MMC_OK;
    });
    MMC_LOG_TRACE("Config store server started, broken handler registered");

    // 注册回调可能同步重发 leader 状态并重新加锁，先解锁避免死锁。
    lock.unlock();
    RegisterHaLeaderCallback();
    lock.lock();

    MmcMetaManager *metaManager = nullptr;
    if (metaMgrProxy_ != nullptr && metaMgrProxy_->GetMetaManager() != nullptr) {
        metaManager = metaMgrProxy_->GetMetaManager().Get();
    }
    kvEvents_.Start(options_, [this](uint32_t rank) { return GetBackendIdForRank(rank); });
    if (metaManager != nullptr) {
        MmcMetaChangeCallbacks callbacks;
        callbacks.stored = [this](const std::string &key, uint32_t rank, uint16_t mediaType) {
            kvEvents_.OnMetaStored(key, rank, mediaType);
        };
        callbacks.removed = [this](const std::string &key, uint32_t rank, uint16_t mediaType) {
            kvEvents_.OnMetaRemoved(key, rank, mediaType);
        };
        callbacks.cleared = [this](uint32_t rank, uint16_t mediaType) { kvEvents_.OnMetaCleared(rank, mediaType); };
        metaManager->SetChangeCallbacks(callbacks);
    }
    kvEvents_.SetPublishActive(!options.haEnable);
    kvEventsPublishActive_ = !options.haEnable;
    periodicEvictActive_.store(!options.haEnable, std::memory_order_relaxed);

    if (!metaNetServerPending_) {
        MMC_RETURN_ERROR(StartMetaNetServer(), "Failed to start MetaNetServer in non-HA mode");
    }

    started_ = true;
    MMC_LOG_TRACE("Started MetaService (" << name_ << ") at " << options_.discoveryURL
                                          << ", metaNetServerPending=" << metaNetServerPending_);

    StartMetricsReportTask();
    if (options_.evictCheckIntervalSeconds == 0) {
        MMC_LOG_INFO("Periodic eviction check disabled by config");
    } else {
        const uint32_t intervalSeconds = options_.evictCheckIntervalSeconds;
        const MmcMetaManagerPtr metaManagerPtr = metaMgrProxy_->GetMetaManager();
        const Result periodicEvictRet = RegisterPeriodicTask("evict_check", intervalSeconds, [this, metaManagerPtr]() {
            if (!periodicEvictActive_.load(std::memory_order_relaxed)) {
                return;
            }
            metaManagerPtr->CheckAndEvict(MEDIA_NONE, 0);
        });
        if (periodicEvictRet != MMC_OK) {
            MMC_LOG_ERROR("Failed to start periodic eviction check task, ret: " << periodicEvictRet);
        }
    }
    return MMC_OK;
}

Result MmcMetaService::BmRegister(uint32_t rank, std::vector<uint16_t> mediaType, std::vector<uint64_t> bm,
                                  std::vector<uint64_t> capacity,
                                  std::vector<std::pair<std::string, MmcMemBlobDesc>> &blobList, bool storageEnabled,
                                  const std::string &backendId, bool enableMaxValueMode)
{
    std::lock_guard<std::mutex> guard(mutex_);
    if (!started_) {
        MMC_LOG_ERROR("MetaService (" << name_ << ") is not started");
        return MMC_NOT_STARTED;
    }

    if (mediaType.size() != bm.size() || bm.size() != capacity.size()) {
        MMC_LOG_ERROR("size invalid, media size:" << mediaType.size() << ", bm size:" << bm.size()
                                                  << ", capacity size:" << capacity.size());
        return MMC_INVALID_PARAM;
    }

    std::vector<MmcLocation> locs;
    std::vector<MmcLocalMemlInitInfo> infos;
    size_t typeNum = mediaType.size();
    for (size_t i = 0; i < typeNum; i++) {
        locs.emplace_back(rank, static_cast<MediaType>(mediaType[i]));
        MmcLocalMemlInitInfo locInfo{bm[i], capacity[i]};
        infos.emplace_back(locInfo);
    }
    MMC_ASSERT_LOG_AND_RETURN(metaBackUpMgrPtr_ != nullptr, "metaBackUpMgrPtr_ is nullptr", MMC_MALLOC_FAILED);
    MMC_ASSERT_LOG_AND_RETURN(metaMgrProxy_ != nullptr, "metaMgrProxy_ is nullptr", MMC_MALLOC_FAILED);
    MMC_RETURN_ERROR(metaBackUpMgrPtr_->Load(blobList), "Mount loc { " << rank << " } load backup failed");
    MMC_RETURN_ERROR(metaMgrProxy_->Mount(locs, infos, blobList, storageEnabled, enableMaxValueMode),
                     "Mount loc { " << rank << " } failed");
    MMC_LOG_TRACE("Mount loc {rank:" << rank << ", rebuild size:" << blobList.size() << ", mediaNum:" << typeNum
                                     << "} finish");
    if (blobList.size() == 0) {
        if (rankMediaTypeMap_.find(rank) == rankMediaTypeMap_.end()) {
            rankMediaTypeMap_.insert({rank, {}});
        }

        for (size_t i = 0; i < typeNum; i++) {
            rankMediaTypeMap_[rank].insert(mediaType[i]);
        }
    }

    if (!backendId.empty()) {
        std::lock_guard<std::mutex> backendLock(rankBackendIdMapLock_);
        rankBackendIdMap_[rank] = backendId;
    }

    return MMC_OK;
}

Result MmcMetaService::BmUnregister(uint32_t rank, uint16_t mediaType)
{
    std::lock_guard<std::mutex> guard(mutex_);
    return BmUnregisterWithoutLock(rank, mediaType);
}

Result MmcMetaService::BmUnregisterWithoutLock(uint32_t rank, uint16_t mediaType)
{
    if (!started_) {
        MMC_LOG_ERROR("MetaService (" << name_ << ") is not started");
        return MMC_NOT_STARTED;
    }

    MmcLocation loc{rank, static_cast<MediaType>(mediaType)};
    MMC_RETURN_ERROR(metaMgrProxy_->Unmount(loc), "Unmount loc { " << rank << ", " << mediaType << " } failed");
    MMC_LOG_DEBUG("Unmount loc: " << loc << " finish");
    if (rankMediaTypeMap_.find(rank) != rankMediaTypeMap_.end() &&
        rankMediaTypeMap_[rank].find(mediaType) != rankMediaTypeMap_[rank].end()) {
        rankMediaTypeMap_[rank].erase(mediaType);
    }
    if (rankMediaTypeMap_.find(rank) != rankMediaTypeMap_.end() && rankMediaTypeMap_[rank].empty()) {
        rankMediaTypeMap_.erase(rank);
    }
    if (rankMediaTypeMap_.find(rank) == rankMediaTypeMap_.end()) {
        std::lock_guard<std::mutex> backendLock(rankBackendIdMapLock_);
        rankBackendIdMap_.erase(rank);
    }
    return MMC_OK;
}

Result MmcMetaService::ClearResource(uint32_t rank)
{
    // 持锁与 BmRegister 串行，避免清理期间重连恢复的 SSD 元数据被误清
    std::lock_guard<std::mutex> guard(mutex_);
    if (!started_) {
        MMC_LOG_ERROR("MetaService (" << name_ << ") is not started.");
        return MMC_NOT_STARTED;
    }
    std::unordered_set<uint16_t> mediaTypes;
    auto it = rankMediaTypeMap_.find(rank);
    if (it == rankMediaTypeMap_.end()) {
        MMC_LOG_DEBUG("Rank " << rank << " has no registered media types.");
    } else {
        mediaTypes = it->second;
    }

    for (const auto &mediaType : mediaTypes) {
        MMC_LOG_TRACE("Clear resource {rank, mediaType} -> { " << rank << ", " << mediaType << " }");
        (void)BmUnregisterWithoutLock(rank, mediaType);
    }

    // SSD 数据由 UBS IO 管理，未注册为 allocator segment；断链时需单独清理容器中的 SSD blob
    if (metaMgrProxy_ != nullptr) {
        Result ret = metaMgrProxy_->CleanSsdBlobs(rank);
        if (ret != MMC_OK) {
            MMC_LOG_WARN("Clean SSD blobs failed for rank=" << rank << ", ret=" << ret);
        }
    }
    // 断链后客户端已不在，清除其上报的指标，ssd_used 等聚合立即归零，不再保留旧值
    MmcClientMetricStore::GetInstance().Remove(rank);
    MMC_LOG_TRACE("Remove client metrics for rank=" << rank);
    return MMC_OK;
}

Result MmcMetaService::ClearAllResources()
{
    std::lock_guard<std::mutex> guard(mutex_);
    if (!started_) {
        MMC_LOG_ERROR("MetaService (" << name_ << ") is not started.");
        return MMC_NOT_STARTED;
    }

    std::vector<uint32_t> ranks;
    for (const auto &entry : rankMediaTypeMap_) {
        ranks.push_back(entry.first);
    }

    for (uint32_t rank : ranks) {
        auto it = rankMediaTypeMap_.find(rank);
        if (it == rankMediaTypeMap_.end()) {
            continue;
        }
        std::unordered_set<uint16_t> mediaTypes = it->second;

        for (const auto &mediaType : mediaTypes) {
            Result ret = BmUnregisterWithoutLock(rank, mediaType);
            if (ret != MMC_OK) {
                MMC_LOG_WARN("ClearAllResources unmount failed, rank=" << rank << ", mediaType=" << mediaType
                                                                       << ", ret=" << ret);
            }
        }

        if (metaMgrProxy_ != nullptr) {
            Result ret = metaMgrProxy_->CleanSsdBlobs(rank);
            if (ret != MMC_OK) {
                MMC_LOG_WARN("Clean SSD blobs failed for rank=" << rank << ", ret=" << ret);
            }
        }
        MmcClientMetricStore::GetInstance().Remove(rank);
    }

    MMC_LOG_INFO("ClearAllResources cleared all " << ranks.size() << " ranks");
    return MMC_OK;
}

void MmcMetaService::SetPublishActive(bool active)
{
    std::lock_guard<std::mutex> guard(mutex_);
    kvEvents_.SetPublishActive(active);
    kvEventsPublishActive_ = active;
}

void MmcMetaService::SetPeriodicEvictActive(bool active)
{
    periodicEvictActive_.store(active, std::memory_order_relaxed);
}

Result MmcMetaService::UpdateConfig(const mmc_meta_service_config_t &options)
{
    std::lock_guard<std::mutex> guard(mutex_);
    if (!started_) {
        MMC_LOG_WARN("MmcMetaService not started, cannot update config");
        return MMC_NOT_STARTED;
    }
    if (options.evictThresholdHigh <= options.evictThresholdLow) {
        MMC_LOG_ERROR("invalid evict thresholds, high=" << options.evictThresholdHigh
                                                        << ", low=" << options.evictThresholdLow
                                                        << ", high must be greater than low");
        return MMC_INVALID_PARAM;
    }
    const uint64_t newLeaseTtlMs = options.leaseTtlMs == 0 ? MMC_DATA_TTL_MS : options.leaseTtlMs;
    if (newLeaseTtlMs == 0) {
        MMC_LOG_ERROR("invalid leaseTtlMs, must be greater than 0, got " << options.leaseTtlMs);
        return MMC_INVALID_PARAM;
    }

    if (metaMgrProxy_ == nullptr) {
        MMC_LOG_WARN("Meta mgr proxy not initialized, cannot update config");
        return MMC_NOT_INITIALIZED;
    }
    Result ret = metaMgrProxy_->UpdateMetaConfig(newLeaseTtlMs, options.evictThresholdHigh, options.evictThresholdLow);
    if (ret != MMC_OK) {
        MMC_LOG_WARN("Failed to update meta manager config, ret=" << ret);
        return ret;
    }
    options_ = options;
    options_.leaseTtlMs = newLeaseTtlMs;
    MMC_LOG_TRACE("Meta service config hot-reloaded, leaseTtlMs="
                  << options_.leaseTtlMs << ", evictThresholdHigh=" << options_.evictThresholdHigh
                  << ", evictThresholdLow=" << options_.evictThresholdLow);
    return MMC_OK;
}

bool MmcMetaService::KvEventsEnabled() const
{
    return kvEvents_.Enabled();
}

kv_event::KvEventStats MmcMetaService::GetKvEventStats() const
{
    return kvEvents_.GetStats();
}

void MmcMetaService::Stop()
{
    // 持锁阶段：仅做状态翻转与快读，随即释锁。必须在 join 之前置 started_=false，
    // 使 worker 线程在 ClearResource(213行)/BmUnregister(189行) 检查到未启动后早退，
    // 避免持锁等待 join 而 worker 又反过等同一把 mutex_ 的死锁。
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (!started_) {
            MMC_LOG_WARN("MmcMetaService has not been started");
            return;
        }
        periodicEvictActive_.store(false, std::memory_order_relaxed);
        UnregisterPeriodicTask("evict_check");
        UnregisterPeriodicTask("metrics_report");
        PublishClearedForRanks(CollectRanks(rankMediaTypeMap_));
        kvEventsPublishActive_ = false;
        MmcMetaManager *metaManager = nullptr;
        if (metaMgrProxy_ != nullptr && metaMgrProxy_->GetMetaManager() != nullptr) {
            metaManager = metaMgrProxy_->GetMetaManager().Get();
        }
        if (metaManager != nullptr) {
            metaManager->SetChangeCallbacks({});
        }
        started_ = false;
    }
    // 无锁阶段：执行含 join 的停止。metaNetServer_->Stop() 会 join net worker，
    // 若上方仍持 mutex_，worker 处理断链时在 ClearResource:219 等 mutex_ 即永久挂死。
    metaBackUpMgrPtr_->Stop();
    metaMgrProxy_->Stop();
    metaNetServer_->Stop();
    kvEvents_.Shutdown();
    // 反注册 broken handler：置空 confStore_ 前先清掉 MF 侧 externalBrokenHandler_，
    // 避免 worker 线程在析构期间仍回调本对象的 lambda（访问 this->confStore_）触发崩溃。
    if (confStore_ != nullptr) {
        confStore_->RegisterServerBrokenHandler(nullptr);
    }
    confStore_ = nullptr;
    metadata_.clear();
    ock::smem::StoreFactory::DestroyStore(options_.configStoreURL);
    MMC_LOG_TRACE("Stop MmcMetaServiceDefault (" << name_ << ") at " << options_.discoveryURL);
}

void MmcMetaService::PublishClearedForRanks(const std::vector<uint32_t> &ranks)
{
    for (const auto &rank : ranks) {
        kvEvents_.PublishCleared(rank);
    }
}

std::string MmcMetaService::GetBackendIdForRank(uint32_t rank)
{
    std::lock_guard<std::mutex> backendLock(rankBackendIdMapLock_);
    const auto it = rankBackendIdMap_.find(rank);
    return (it != rankBackendIdMap_.end()) ? it->second : std::string();
}

void MmcMetaService::StartMetricsReportTask()
{
    if (options_.metricsReportIntervalSeconds == 0) {
        MMC_LOG_INFO("Metrics report task disabled by config");
        return;
    }
    const uint32_t intervalSeconds = options_.metricsReportIntervalSeconds;
    const Result ret = RegisterPeriodicTask("metrics_report", intervalSeconds, [this]() {
        if (metaMgrProxy_ == nullptr) {
            MMC_LOG_WARN("Skip metrics report task because metaMgrProxy is null");
            return;
        }
        MmcRestApiFacade facade(this, metaMgrProxy_);
        std::string metricsSummary;
        if (facade.BuildMetricsSummary(true, metricsSummary) == MMC_OK) {
            MMC_AUDIT_LOG("Metrics summary: " + metricsSummary);
        } else {
            MMC_LOG_WARN("Unable to build periodic metrics summary");
        }
    });
    if (ret != MMC_OK) {
        MMC_LOG_ERROR("Failed to start metrics report task");
    }
}

bool MmcMetaService::IsSsdAvailable(uint32_t rank) const
{
    return metaMgrProxy_.Get() != nullptr && metaMgrProxy_->GetMetaManager().Get() != nullptr &&
           metaMgrProxy_->GetMetaManager()->IsSsdAvailable(rank);
}

void MmcMetaService::OnAsyncFlushComplete(uint32_t rank,
                                          const std::vector<std::pair<std::string, MmcMemBlobDesc>> &blobs)
{
    auto &metricMgr = MmcMetaMetricManager::GetInstance();
    if (metaMgrProxy_.Get() != nullptr && metaMgrProxy_->GetMetaManager().Get() != nullptr) {
        for (const auto &[key, desc] : blobs) {
            metaMgrProxy_->GetMetaManager()->AddSsdBlob(key, desc);
            metricMgr.IncrementAsyncFlushBlobAdded(rank, desc.size_);
        }
    }
}

Result MmcMetaService::GetMetadata(const std::string &key, std::string &value, int64_t timeoutMs)
{
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = metadata_.find(key);
    if (it == metadata_.end()) {
        return ock::smem::StoreErrorCode::NOT_EXIST;
    } else {
        value = it->second;
        return ock::smem::StoreErrorCode::SUCCESS;
    }
}

Result MmcMetaService::PutMetadata(const std::string &key, const std::string &value)
{
    std::lock_guard<std::mutex> guard(mutex_);
    metadata_[key] = value;
    return ock::smem::StoreErrorCode::SUCCESS;
}

Result MmcMetaService::DeleteMetadata(const std::string &key)
{
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = metadata_.find(key);
    if (it == metadata_.end()) {
        return ock::smem::StoreErrorCode::NOT_EXIST;
    } else {
        metadata_.erase(it);
        return ock::smem::StoreErrorCode::SUCCESS;
    }
}

void MmcMetaService::RegisterHaLeaderCallback()
{
    // etcd:// 模式下 confStore_ 本身就是 HaConfigStore，直接做顶层转换，
    // 而非 GetCoreStore()（其返回底层 TcpConfigStore，用于 join/leave 路径）。
    auto *haStore = dynamic_cast<ock::smem::HaConfigStore *>(confStore_.Get());
    if (haStore == nullptr) {
        MMC_LOG_TRACE("ConfigStore is not HaConfigStore, HA leader callback not registered");
        return;
    }

    metaNetServerPending_ = true;
    haStore->RegisterLeaderChangeCallback([this](const ock::smem::HaConfigStore::LeaderAddresses &addrs) {
        if (addrs.isLeader) {
            metaServiceAddr_ = addrs.metaServiceAddr;
            OnBecomeLeader();
        } else {
            OnBecomeStandby();
        }
    });
    MMC_LOG_TRACE("Registered HA leader change callback on HaConfigStore");
}

Result MmcMetaService::StartMetaNetServer()
{
    if (metaNetServer_ == nullptr) {
        MMC_LOG_ERROR("metaNetServer_ is nullptr, cannot start MetaNetServer");
        return MMC_NOT_INITIALIZED;
    }

    if (!metaServiceAddr_.empty()) {
        auto ret = NetEngineOptions::ExtractIpPortFromUrl("tcp://" + metaServiceAddr_, netOptions_);
        if (ret != MMC_OK) {
            MMC_LOG_ERROR("Failed to extract ip and port from metaServiceAddr "
                          << metaServiceAddr_ << ", fallback would bind " << netOptions_.ip << ":" << netOptions_.port);
            return ret;
        }
        MMC_LOG_TRACE("Starting MetaNetServer on dynamically assigned address: " << metaServiceAddr_);
    }

    MMC_LOG_INFO("MetaNetServer binding on " << netOptions_.ip << ":" << netOptions_.port);
    return metaNetServer_->Start(netOptions_);
}

void MmcMetaService::StopMetaNetServer()
{
    if (metaNetServer_ != nullptr) {
        metaNetServer_->Stop();
    }
}

void MmcMetaService::OnBecomeLeader()
{
    MMC_LOG_TRACE("HaConfigStore elected as leader, starting MetaNetServer and enabling KvEvents");
    if (StartMetaNetServer() != MMC_OK) {
        MMC_LOG_ERROR("Failed to start MetaNetServer after becoming leader");
        return;
    }
    SetPublishActive(true);
    SetPeriodicEvictActive(true);
    MMC_LOG_TRACE("MetaService leader ready, MetaNetServer listening on "
                  << netOptions_.ip << ":" << netOptions_.port << ", discoveryURL: " << options_.discoveryURL);
}

void MmcMetaService::OnBecomeStandby()
{
    MMC_LOG_TRACE("HaConfigStore became standby/follower, stopping MetaNetServer and disabling KvEvents");
    SetPeriodicEvictActive(false);
    SetPublishActive(false);
    StopMetaNetServer();
    (void)ClearAllResources();
}

} // namespace mmc
} // namespace ock
