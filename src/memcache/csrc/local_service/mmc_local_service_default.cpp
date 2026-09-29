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

#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <thread>

#include "smem_bm_api.h"
#include "mmc_meta_net_client.h"
#include "mmc_storage_mode.h"
#include "mmc_msg_client_meta.h"
#include "mmc_ptracer.h"
#include "mmc_functions.h"
#include "mmc_configuration.h"
#include "mmc_local_service_default.h"

namespace ock {
namespace mmc {
constexpr int TIMEOUT_THIRTY = 30;
constexpr int CLIENT_THREAD_COUNT = 2;
constexpr size_t kSsdRebuildQueryBatchSize = 1024;

namespace {
bool IsSsdMetadataSeparated(uint32_t mode)
{
    return mode == MMC_SSD_METADATA_SEPARATED;
}

bool IsEtcdStoreUrl(const char *storeUrl)
{
    return storeUrl != nullptr && (std::string(storeUrl).find("etcd://") == 0);
}
} // namespace

MmcLocalServiceDefault::~MmcLocalServiceDefault()
{
    StopConfigPolling();
}

Result MmcLocalServiceDefault::ConnectMetaNetServer(const NetEngineOptions &netOptions)
{
    if (metaNetClient_->Start(netOptions) != MMC_OK) {
        MMC_LOG_ERROR("Failed to start net server of local service, bmRankId=" << options_.rankId);
        return MMC_ERROR;
    }
    if (!IsEtcdStoreUrl(options_.bmIpPort)) {
        // 非 etcd 模式（进程级）：MetaService 监听 meta_service_url 配置端口，直接使用配置值。
        MMC_LOG_TRACE("non-etcd mode: connect MetaService via configured URL: " << options_.discoveryURL);
        if (metaNetClient_->Connect(options_.discoveryURL) != MMC_OK) {
            MMC_LOG_ERROR("Failed to start net server of local service, bmRankId=" << options_.rankId);
            return MMC_ERROR;
        }
        return MMC_OK;
    }
    // etcd HA 模式：MetaService 地址由 leader 动态分配并注册进 etcd（TTL lease）。
    // 链路故障窗口内 lease 会短暂消失（约一次选举周期），且端口随 leader 切换变化，
    // 因此发现失败或连接失败都必须回到发现步骤重试，禁止固定使用单次结果。
    constexpr int kRetryIntervalSec = 2;
    constexpr int kMaxWaitSec = 60;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kMaxWaitSec);
    while (std::chrono::steady_clock::now() < deadline) {
        if (stopRequested_.load(std::memory_order_relaxed)) {
            MMC_LOG_WARN("LocalService stop requested, abort MetaService discovery, rank=" << options_.rankId);
            return MMC_ERROR;
        }
        char metaIp[64] = {0};
        uint16_t metaPort = 0;
        if (ock::mmc::MFSmemApi::SmemBmGetMetaServiceInfo(metaIp, sizeof(metaIp), &metaPort) == 0 &&
            metaIp[0] != '\0') {
            const std::string connectUrl = "tcp://" + std::string(metaIp) + ":" + std::to_string(metaPort);
            MMC_LOG_TRACE("etcd HA: discovered MetaService leader via ConfigStore, " << connectUrl);
            if (metaNetClient_->Connect(connectUrl) == MMC_OK) {
                MMC_LOG_INFO("MetaNetClient connected to " << connectUrl << ", rank=" << options_.rankId);
                return MMC_OK;
            }
            MMC_LOG_WARN("MetaNetClient connect " << connectUrl << " failed, rank=" << options_.rankId
                                                  << ", will rediscover MetaService addr");
        } else {
            MMC_LOG_WARN("MetaService addr not found in backend yet, rank=" << options_.rankId << ", retry in "
                                                                            << kRetryIntervalSec << "s");
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::seconds(kRetryIntervalSec));
    }
    MMC_LOG_ERROR("Failed to start net server of local service, bmRankId=" << options_.rankId
                                                                           << ", discoveryURL=" << options_.discoveryURL
                                                                           << ", waited " << kMaxWaitSec << "s");
    return MMC_ERROR;
}

Result MmcLocalServiceDefault::Start(const mmc_local_service_config_t &config)
{
    MMC_LOG_TRACE("Starting meta service " << name_);
    std::lock_guard<std::mutex> guard(mutex_);
    if (started_) {
        MMC_LOG_TRACE("MetaService " << name_ << " already started");
        return MMC_OK;
    }
    stopRequested_.store(false, std::memory_order_relaxed);

    // 初始化BM，并更新bmRankId
    options_ = config;
    MMC_RETURN_ERROR(ock::mmc::MmcOutLogger::Instance().SetLogLevel(static_cast<LogLevel>(options_.logLevel)),
                     "failed to set log level " << options_.logLevel);
    if (options_.logFunc != nullptr) {
        ock::mmc::MmcOutLogger::Instance().SetExternalLogFunction(options_.logFunc);
    }
    MMC_RETURN_ERROR(InitBm(), "Failed to init bm of local service " << name_);

    metaNetClient_ = MetaNetClientFactory::GetInstance(this->options_.discoveryURL, "MetaClientCommon").Get();
    MMC_ASSERT_LOG_AND_RETURN(metaNetClient_.Get() != nullptr, "metaNetClient_.Get() is nullptr",
                              MMC_NEW_OBJECT_FAILED);
    if (!metaNetClient_->Status()) {
        NetEngineOptions options;
        options.name = name_;
        options.threadCount = CLIENT_THREAD_COUNT;
        options.rankId = options_.rankId;
        options.startListener = false;
        options.tlsOption = options_.accTlsConfig;
        options.logLevel = options_.logLevel;
        options.logFunc = options_.logFunc;

        if (ConnectMetaNetServer(options) != MMC_OK) {
            DestroyBm();
            metaNetClient_->Stop();
            return MMC_ERROR;
        }
    }

    Result configRet = GetExternalConfig();
    if (configRet != MMC_OK) {
        MMC_LOG_ERROR("Failed to query initialization config from MetaService, ret=" << configRet);
        DestroyBm();
        metaNetClient_->Stop();
        return configRet;
    }

    if (options_.storageEnabled) {
        ubsioEventPool_ = MmcMakeRef<MmcThreadPool>("io_evt_pool", UBSIO_EVENT_POOL_SIZE);
        MMC_ASSERT_LOG_AND_RETURN(ubsioEventPool_ != nullptr, "ubsioEventPool_ is nullptr", MMC_MALLOC_FAILED);
        MMC_RETURN_ERROR(ubsioEventPool_->Start(), "ubsio event pool start failed");

        if (InitUbsIo(config.deviceId, config.configFilePath) != MMC_OK) {
            MMC_LOG_ERROR("Failed to init ubsIo of local service " << name_);
            ubsioEventPool_->Destroy();
            DestroyBm();
            metaNetClient_->Stop();
            return MMC_ERROR;
        }
    } else {
        MMC_LOG_INFO("SSD storage disabled, rank=" << options_.rankId);
    }
    pid_ = getpid();

    if (RegisterBm() != MMC_OK) {
        MMC_LOG_ERROR("Failed to register bm, name=" << name_ << ", bmRankId=" << options_.rankId);
        if (ubsIoProxyPtr_ != nullptr) {
            ubsIoProxyPtr_->SetMetaEventCallback(nullptr);
            ubsIoProxyPtr_->DestroyUbsIo();
        }
        if (ubsioEventPool_ != nullptr) {
            ubsioEventPool_->Destroy();
        }
        DestroyBm();
        metaNetClient_->Stop();
        return MMC_ERROR;
    }
    metaNetClient_->SetBackupPool(ubsioEventPool_);
    metaNetClient_->RegisterRetryHandler(
        std::bind(&MmcLocalServiceDefault::ReconnectBm, this),
        std::bind(&MmcLocalServiceDefault::UpdateMetaBackup, this, std::placeholders::_1, std::placeholders::_2,
                  std::placeholders::_3, std::placeholders::_4),
        std::bind(&MmcLocalServiceDefault::CopyBlob, this, std::placeholders::_1, std::placeholders::_2,
                  std::placeholders::_3),
        std::bind(&MmcLocalServiceDefault::BlobDelete, this, std::placeholders::_1, std::placeholders::_2),
        std::bind(&MmcLocalServiceDefault::BatchCopyBlob, this, std::placeholders::_1, std::placeholders::_2,
                  std::placeholders::_3));
    metaNetClient_->RegisterStorageHandler(std::bind(&MmcLocalServiceDefault::BatchStorageStat, this,
                                                     std::placeholders::_1, std::placeholders::_2,
                                                     std::placeholders::_3));

    metaNetClient_->RegisterDiscoverUrlFunc([this]() -> std::string { return DiscoverMetaServiceUrl(); });

    // HA(etcd) 模式：注册 leader 变更回调。
    if (IsEtcdStoreUrl(options_.bmIpPort)) {
        auto regRet =
            ock::mmc::MFSmemApi::SmemBmRegisterLeaderChangeCallback(&MmcLocalServiceDefault::OnLeaderChanged, this);
        if (regRet != MMC_OK) {
            MMC_LOG_WARN("Register leader change callback failed, ret: "
                         << regRet << ", leader switch relies on link-broken only");
        }
    }
    started_ = true;
    if (options_.dynamicConfigEnable && options_.configFilePath[0] != '\0') {
        StartConfigPolling();
    }
    MMC_LOG_TRACE("Started LocalService (" << name_ << ") server " << options_.discoveryURL
                                           << ", rank: " << options_.rankId);
    return MMC_OK;
}

std::string MmcLocalServiceDefault::DiscoverMetaServiceUrl() const
{
    // 非 etcd 模式（进程级）：MetaService 地址固定为 meta_service_url 配置值，
    // 返回空让 MetaNetClient 使用默认（配置的 discoveryURL）。
    if (!IsEtcdStoreUrl(options_.bmIpPort)) {
        return "";
    }
    char metaIp[64] = {0};
    uint16_t metaPort = 0;
    if (ock::mmc::MFSmemApi::SmemBmGetMetaServiceInfo(metaIp, sizeof(metaIp), &metaPort) == 0 && metaIp[0] != '\0') {
        return "tcp://" + std::string(metaIp) + ":" + std::to_string(metaPort);
    }
    return "";
}

void MmcLocalServiceDefault::OnLeaderChanged(void *userData)
{
    auto *self = static_cast<MmcLocalServiceDefault *>(userData);
    if (self == nullptr) {
        return;
    }
    // 运行在 libsmem 派发的异步线程上：快照 metaNetClient_（避免与 Stop() 竞争），
    // Stop() 之后快照为空则静默返回。本层只负责注册与下发；地址发现（discoverUrlFunc_）、
    // 幂等比较、强制重连与 BM 重注册全部由 MetaNetClient::HandleServerChangedNotify 完成。
    MetaNetClientPtr client;
    {
        std::lock_guard<std::mutex> guard(self->mutex_);
        client = self->metaNetClient_;
    }
    if (client == nullptr) {
        MMC_LOG_TRACE("Leader changed but local service stopped, skip");
        return;
    }
    (void)client->HandleServerChangedNotify();
}

void MmcLocalServiceDefault::Stop()
{
    stopRequested_.store(true, std::memory_order_relaxed);
    std::lock_guard<std::mutex> guard(mutex_);
    if (!started_) {
        MMC_LOG_WARN("MmcLocalServiceDefault has not been started" << ", rank: " << options_.rankId);
        return;
    }
    started_ = false;
    StopConfigPolling();

    // 先解绑 UBSIO callback，防止 UBSIO 线程继续投递新事件
    if (ubsIoProxyPtr_ != nullptr) {
        ubsIoProxyPtr_->SetMetaEventCallback(nullptr);
    }

    // 排空已入队的事件
    if (ubsioEventPool_ != nullptr) {
        ubsioEventPool_->Destroy();
    }

    // 回调已解绑、事件已排空，安全停止 UBSIO
    if (ubsIoProxyPtr_ != nullptr) {
        ubsIoProxyPtr_->DestroyUbsIo();
        ubsIoProxyPtr_ = nullptr;
    }

    DestroyBm();
    if (metaNetClient_ != nullptr) {
        metaNetClient_->Stop();
        metaNetClient_ = nullptr;
    }
    std::lock_guard<std::mutex> guardBlob(blobMutex_);
    blobMap_.clear();
    MMC_LOG_TRACE("Stop MmcLocalServiceDefault (" << name_ << ") server " << options_.discoveryURL
                                                  << ", rank: " << options_.rankId);
}

Result MmcLocalServiceDefault::InitBm()
{
    mmc_bm_init_config_t initConfig = {.deviceId = options_.deviceId,
                                       .worldSize = options_.worldSize,
                                       .ipPort = options_.bmIpPort,
                                       .hcomUrl = options_.bmHcomUrl,
                                       .logLevel = options_.logLevel,
                                       .logFunc = options_.logFunc,
                                       .flags = options_.flags,
                                       .hcomTlsConfig = options_.hcomTlsConfig,
                                       .storeTlsConfig = options_.configStoreTlsConfig};

    uint32_t createFlags = options_.flags;
    if (options_.storageEnabled && options_.localDRAMSize > 0) {
        createFlags |= SMEM_BM_FLAG_DRAM_MAP_HOST_VA;
    }
    mmc_bm_create_config_t createConfig = {.id = options_.createId,
                                           .memberSize = options_.worldSize,
                                           .dataOpType = options_.dataOpType,
                                           .tag = options_.tag,
                                           .tagOpInfo = options_.tagOpInfo,
                                           .localDRAMSize = options_.localDRAMSize,
                                           .localMaxDRAMSize = options_.localMaxDRAMSize,
                                           .localHBMSize = options_.localHBMSize,
                                           .localMaxHBMSize = options_.localMaxHBMSize,
                                           .flags = createFlags};

    MmcBmProxyPtr bmProxy = MmcBmProxyFactory::GetInstance("bmProxyDefault");
    MMC_ASSERT_LOG_AND_RETURN(bmProxy != nullptr, "bmProxy is nullptr", MMC_ERROR);
    Result ret = bmProxy->InitBm(initConfig, createConfig);
    if (ret != MMC_OK) {
        return ret;
    }
    options_.rankId = bmProxy->RankId();
    bmProxyPtr_ = bmProxy;
    return ret;
}

Result MmcLocalServiceDefault::GetExternalConfig()
{
    GetExternalConfigRequest request;
    GetExternalConfigResponse response;
    Result ret = SyncCallMeta(request, response, TIMEOUT_THIRTY);
    if (ret != MMC_OK) {
        MMC_LOG_ERROR("Get external config RPC failed, rank=" << options_.rankId << ", ret=" << ret);
        return ret;
    }
    if (response.ret_ != MMC_OK) {
        MMC_LOG_ERROR("Get external config failed on MetaService, rank=" << options_.rankId
                                                                         << ", ret=" << response.ret_);
        return response.ret_;
    }
    if (response.ssdMetadataMode_ > MMC_SSD_METADATA_SEPARATED) {
        MMC_LOG_ERROR("Invalid SSD metadata mode returned by MetaService: " << response.ssdMetadataMode_);
        return MMC_INVALID_PARAM;
    }
    ssdMetadataMode_ = static_cast<SsdMetadataMode>(response.ssdMetadataMode_);
    MMC_LOG_TRACE("Resolved SSD metadata mode from MetaService: " << response.ssdMetadataMode_);
    return MMC_OK;
}

Result MmcLocalServiceDefault::DestroyBm()
{
    MMC_RETURN_ERROR(bmProxyPtr_ == nullptr, "bm proxy has not been initialized.");

    BmUnregisterRequest req;
    req.mediaType_.clear();
    req.rank_ = options_.rankId;
    for (MediaType type = MEDIA_HBM; type != MEDIA_NONE;) {
        if (bmProxyPtr_->GetGva(type) != 0 && bmProxyPtr_->GetCapacity(type) != 0) {
            req.mediaType_.emplace_back(type);
        }
        type = MoveDown(type);
    }
    Response resp;
    // Best-effort unregister: MetaService auto-cleans on link break, so don't block shutdown.
    Result ret = SyncCallMeta(req, resp, BM_UNREGISTER_TIMEOUT_SECOND);
    bmProxyPtr_->DestroyBm();
    bmProxyPtr_ = nullptr;
    if (ret || resp.ret_) {
        MMC_LOG_WARN("unregister ret: " << ret << ", respRet: " << resp.ret_);
    }
    return MMC_OK;
}

Result MmcLocalServiceDefault::RegisterBm()
{
    MMC_RETURN_ERROR(bmProxyPtr_ == nullptr, "bm proxy has not been initialized.");
    std::unique_lock<std::mutex> lockGuard(blobMutex_);
    return RebuildBm();
}

Result MmcLocalServiceDefault::RebuildBm()
{
    BmRegisterRequest req;
    RegisterBaseInfo(req);

    Response resp;
    for (const auto &backup : blobMap_) {
        for (const auto &desc : backup.second) {
            req.blobList_.push_back({backup.first, desc});
        }
        if (req.blobList_.size() >= static_cast<size_t>(blobRebuildSendMaxCount)) {
            MMC_LOG_DEBUG("mmc meta blob rebuild count " << req.blobList_.size());
            MMC_RETURN_ERROR(SyncCallMeta(req, resp, TIMEOUT_THIRTY), "bm register failed, bmRankId=" << req.rank_);
            MMC_RETURN_ERROR(resp.ret_, "bm register failed, bmRankId=" << req.rank_ << ", retCode=" << resp.ret_);
            req.blobList_.clear();
        }
    }

    if (!req.blobList_.empty()) {
        MMC_LOG_DEBUG("mmc meta blob rebuild count " << req.blobList_.size());
        MMC_RETURN_ERROR(SyncCallMeta(req, resp, TIMEOUT_THIRTY), "bm register failed, bmRankId=" << req.rank_);
        MMC_RETURN_ERROR(resp.ret_, "bm register failed, bmRankId=" << req.rank_ << ", retCode=" << resp.ret_);
        req.blobList_.clear();
    }

    // An empty registration marks completion after all key batches are processed and reported.
    MMC_RETURN_ERROR(SyncCallMeta(req, resp, TIMEOUT_THIRTY), "bm register failed, bmRankId=" << req.rank_);
    MMC_RETURN_ERROR(resp.ret_, "bm register failed, bmRankId=" << req.rank_ << ", retCode=" << resp.ret_);
    req.blobList_.clear();
    return MMC_OK;
}

std::pair<size_t, size_t> MmcLocalServiceDefault::UpdateSsdRebuildBatch(
    std::map<std::string, std::vector<MmcMemBlobDesc>>::iterator &it,
    const std::map<std::string, std::vector<MmcMemBlobDesc>>::iterator &batchEnd, const bool *exists)
{
    size_t removedCount = 0;
    size_t supplementedCount = 0;
    for (size_t index = 0; it != batchEnd; ++index) {
        auto &descs = it->second;
        for (auto descIt = descs.begin(); descIt != descs.end();) {
            if (descIt->mediaType_ == MEDIA_SSD && !exists[index]) {
                descIt = descs.erase(descIt);
                ++removedCount;
            } else {
                ++descIt;
            }
        }

        bool hasSsdDesc = false;
        for (const auto &desc : descs) {
            if (desc.mediaType_ == MEDIA_SSD) {
                hasSsdDesc = true;
                break;
            }
        }

        if (!hasSsdDesc && exists[index] && !descs.empty()) {
            MmcMemBlobDesc ssdDesc = descs.front();
            ssdDesc.gva_ = 0;
            ssdDesc.mediaType_ = MEDIA_SSD;
            ssdDesc.allocSize_ = 0; // SSD 副本不走 allocator 释放，无切块语义
            descs.push_back(ssdDesc);
            ++supplementedCount;
        }

        if (descs.empty()) {
            it = blobMap_.erase(it);
        } else {
            ++it;
        }
    }

    return {removedCount, supplementedCount};
}

Result MmcLocalServiceDefault::ReconnectBm()
{
    MMC_RETURN_ERROR(bmProxyPtr_ == nullptr, "bm proxy has not been initialized.");
    std::unique_lock<std::mutex> lockGuard(blobMutex_);
    if (ubsIoProxyPtr_ == nullptr || IsSsdMetadataSeparated(ssdMetadataMode_)) {
        return RebuildBm();
    }

    auto it = blobMap_.begin();
    const auto end = blobMap_.end();
    std::vector<std::string> queryKeys;
    queryKeys.reserve(kSsdRebuildQueryBatchSize);

    while (it != end) {
        queryKeys.clear();
        auto batchEnd = it;
        while (batchEnd != end && queryKeys.size() < kSsdRebuildQueryBatchSize) {
            queryKeys.push_back(batchEnd->first);
            ++batchEnd;
        }

        std::array<bool, kSsdRebuildQueryBatchSize> exists{};
        Result ret = ubsIoProxyPtr_->BatchExist(queryKeys, exists.data());
        if (ret != MMC_OK) {
            exists.fill(false);
            MMC_LOG_WARN("SSD rebuild BatchExist failed, treating batch as missing, keyCount=" << queryKeys.size()
                                                                                               << ", ret=" << ret);
        }

        const auto rebuildCounts = UpdateSsdRebuildBatch(it, batchEnd, exists.data());
        if (rebuildCounts.first != 0) {
            MMC_LOG_INFO("SSD rebuild removed stale backups, keyCount=" << queryKeys.size()
                                                                        << ", removedCount=" << rebuildCounts.first);
        }
        if (rebuildCounts.second != 0) {
            MMC_LOG_INFO("SSD rebuild supplemented backups, keyCount=" << queryKeys.size() << ", supplementedCount="
                                                                       << rebuildCounts.second);
        }
    }
    return RebuildBm();
}

void MmcLocalServiceDefault::RegisterBaseInfo(BmRegisterRequest &req)
{
    req.rank_ = options_.rankId;
    req.backendId_ = ResolveBackendId();
    req.storageEnabled_ = options_.storageEnabled;
    req.enableMaxValueMode_ = options_.enableMaxValueMode;
    for (MediaType type = MEDIA_HBM; type != MEDIA_NONE;) {
        uint64_t gva = bmProxyPtr_->GetGva(type);
        uint64_t capacity = bmProxyPtr_->GetCapacity(type);
        if (gva != 0 && capacity != 0) {
            req.addr_.emplace_back(gva);
            req.mediaType_.emplace_back(type);
            req.capacity_.emplace_back(capacity);
            MMC_LOG_INFO("mmc local register capacity:" << req.capacity_.back() << ", type:" << req.mediaType_.back());
        }
        type = MoveDown(type);
    }
    if (ubsIoProxyPtr_ == nullptr && options_.storageEnabled) {
        MMC_LOG_ERROR("SSD storage enabled but ubsIo proxy not initialized, rank=" << options_.rankId);
    }
    req.blobList_.clear();
}

Result MmcLocalServiceDefault::InitUbsIo(int32_t deviceId, const std::string &confPath)
{
    MmcUbsIoProxyPtr ubsIoProxy = MmcUbsIoProxyFactory::GetInstance("ubsIoProxyDefault");
    MMC_ASSERT_LOG_AND_RETURN(ubsIoProxy != nullptr, "ubsIoProxy is nullptr", MMC_ERROR);
    MMC_ASSERT_LOG_AND_RETURN(metaNetClient_ != nullptr && metaNetClient_->Status(),
                              "metaNetClient_ not ready when registering UBS IO callback", MMC_NOT_INITIALIZED);
    ubsIoProxyPtr_ = ubsIoProxy;

    // 只有 aggregated 模式才用 SSD 生命周期事件更新集中式元数据；separated 模式由 UBSIO 管理，
    // MetaService 本来就不保存 SSD 条目，因此无需更新或删除。
    if (!IsSsdMetadataSeparated(ssdMetadataMode_)) {
        ubsIoProxy->SetMetaEventCallback([this](int type, const std::vector<std::string> &keys) {
            ubsioEventPool_->Enqueue([this, type, keys]() { HandleUbsIoMetaEvents(type, keys); });
        });
    }

    auto ret = ubsIoProxy->InitUbsIo(deviceId, confPath);
    if (ret != MMC_OK) {
        ubsIoProxy->SetMetaEventCallback(nullptr);
        return ret;
    }

    if (!IsSsdMetadataSeparated(ssdMetadataMode_)) {
        ret = RecoverBlob(ubsIoProxy);
        if (ret != MMC_OK) {
            MMC_LOG_ERROR("Failed to recover blobMap from ubs, name=" << name_ << ", ret=" << ret);
            ubsIoProxy->SetMetaEventCallback(nullptr);
            ubsIoProxy->DestroyUbsIo();
            ubsIoProxyPtr_ = nullptr;
            return ret;
        }
    }
    return MMC_OK;
}

Result MmcLocalServiceDefault::RecoverBlob(const MmcUbsIoProxyPtr &ubsIoProxy)
{
    uint64_t recoveredCount = 0;
    bool hasMore = true;
    while (hasMore) {
        const UbsioKvKeyInfo *items = nullptr;
        uint64_t count = 0;
        Result ret = ubsIoProxy->ScanKvCache(items, count, hasMore);
        if (ret != MMC_OK) {
            MMC_LOG_WARN("Failed to scan persisted KV from ubs, skip blobMap recovery, ret=" << ret);
            return ret;
        }
        if (items == nullptr || count == 0) {
            break;
        }
        {
            std::lock_guard<std::mutex> guard(blobMutex_);
            for (uint64_t i = 0; i < count; ++i) {
                const UbsioKvKeyInfo &info = items[i];
                if (info.keyLen == 0 || info.keyLen >= UBSIO_KV_MAX_KEY_SIZE) {
                    MMC_LOG_WARN("Skip invalid persisted KV item, index=" << i);
                    continue;
                }
                std::string key(info.key, info.keyLen);
                MmcMemBlobDesc desc;
                desc.mediaType_ = MEDIA_SSD;
                desc.size_ = info.valueLen;
                desc.gva_ = 0;
                desc.rank_ = options_.rankId;
                auto &descs = blobMap_[key];
                descs.push_back(desc);
            }
        }
        ubsIoProxy->FreeScanKeyResult(items);
        recoveredCount += count;
    }

    MMC_LOG_INFO("Recovered " << recoveredCount << " persisted keys from ubs into blobMap");
    return MMC_OK;
}

Result MmcLocalServiceDefault::UpdateMetaBackup(const std::vector<uint32_t> &ops, const std::vector<std::string> &keys,
                                                const std::vector<MmcMemBlobDesc> &blobs,
                                                std::vector<Result> &keyResults)
{
    const auto opCount = ops.size();
    const auto keyCount = keys.size();
    const auto blobCount = blobs.size();
    auto length = keyCount;
    if (keyCount != blobCount || keyCount != opCount) {
        MMC_LOG_ERROR("Local service replicate error, length mismatch: opSize=" << opCount << ", keySize=" << keyCount
                                                                                << ", blobSize=" << blobCount);
        return MMC_INVALID_PARAM;
    }

    std::vector<size_t> flushTasks = ProcessBackupMetadata(ops, keys, blobs, length, keyResults);
    MMC_LOG_DEBUG("ProcessBackupMetadata done, flushTasks=" << flushTasks.size());

    BatchFlushToSsd(flushTasks, keys, blobs, keyResults);

    MMC_LOG_DEBUG("Handle " << length << " metas backup");
    return MMC_OK;
}

static bool AddDesc(std::vector<MmcMemBlobDesc> &descs, const MmcMemBlobDesc &blob)
{
    for (auto &d : descs) {
        if (d.mediaType_ == blob.mediaType_) {
            if (d != blob) {
                MMC_LOG_WARN("duplicate blob desc for same mediaType, old=" << d << ", new=" << blob);
                return false;
            }
            return true;
        }
    }
    descs.push_back(blob);
    return true;
}

static bool EraseDesc(std::map<std::string, std::vector<MmcMemBlobDesc>> &blobMap, const std::string &key,
                      const MmcMemBlobDesc &blob)
{
    auto it = blobMap.find(key);
    if (it == blobMap.end()) {
        MMC_LOG_WARN("backup remove key not found in blobMap, key=" << key);
        return false;
    }
    auto &descs = it->second;
    for (auto dit = descs.begin(); dit != descs.end(); ++dit) {
        if (*dit == blob) {
            descs.erase(dit);
            if (descs.empty()) {
                blobMap.erase(it);
            }
            return true;
        }
    }
    MMC_LOG_WARN("backup remove blob desc mismatch in blobMap, key=" << key << ", blob=" << blob);
    return false;
}

std::vector<size_t> MmcLocalServiceDefault::ProcessBackupMetadata(const std::vector<uint32_t> &ops,
                                                                  const std::vector<std::string> &keys,
                                                                  const std::vector<MmcMemBlobDesc> &blobs,
                                                                  size_t length, std::vector<Result> &keyResults)
{
    keyResults.resize(length, MMC_ERROR);
    std::vector<size_t> flushTasks;
    std::lock_guard<std::mutex> guard(blobMutex_);
    size_t unexpectedOps = 0;
    for (size_t i = 0; i < length; i++) {
        if (IsSsdMetadataSeparated(ssdMetadataMode_) && blobs[i].mediaType_ == MEDIA_SSD) {
            keyResults[i] = MMC_OK;
            continue;
        }
        if (ops[i] == META_BACKUP_ADD) {
            auto &descs = blobMap_[keys[i]];
            if (!AddDesc(descs, blobs[i])) {
                keyResults[i] = MMC_DUPLICATED_OBJECT;
                continue;
            }
            if (blobs[i].mediaType_ == MEDIA_DRAM && ubsIoProxyPtr_ != nullptr && bmProxyPtr_ != nullptr) {
                flushTasks.push_back(i);
            } else {
                // DRAM 未使能刷盘（ubs/bm 不可用）：仅记录元数据，不构造 SSD 副本；其他介质直接成功
                keyResults[i] = MMC_OK;
            }
        } else if (ops[i] == META_BACKUP_ADD_REWARM) {
            auto &descs = blobMap_[keys[i]];
            if (AddDesc(descs, blobs[i])) {
                keyResults[i] = MMC_OK;
            } else {
                keyResults[i] = MMC_DUPLICATED_OBJECT;
            }
        } else if (ops[i] == META_BACKUP_REMOVE) {
            bool removed = EraseDesc(blobMap_, keys[i], blobs[i]);
            keyResults[i] = removed ? MMC_OK : MMC_UNMATCHED_KEY;
        } else {
            ++unexpectedOps;
            MMC_LOG_ERROR("meta backup error, key: " << keys[i]);
        }
    }
    if (unexpectedOps > 0) {
        MMC_LOG_ERROR("ProcessBackupMetadata unexpected op count=" << unexpectedOps << "/" << length);
    }
    return flushTasks;
}

void MmcLocalServiceDefault::CollectFlushParams(const std::vector<size_t> &indices,
                                                const std::vector<std::string> &keys,
                                                const std::vector<MmcMemBlobDesc> &blobs,
                                                std::vector<Result> &keyResults, BatchIoParams &out)
{
    for (size_t idx : indices) {
        uint64_t srcVa = 0;
        TP_TRACE_BEGIN(TP_MMC_META_ASYNC_FLUSH_RESOLVE_VA);
        Result gvaRet = bmProxyPtr_->GvaToVa(blobs[idx].gva_, MEDIA_DRAM, srcVa);
        TP_TRACE_END(TP_MMC_META_ASYNC_FLUSH_RESOLVE_VA, gvaRet);
        if (gvaRet != MMC_OK) {
            MMC_LOG_WARN("Failed to resolve VA for SSD flush, key=" << keys[idx] << ", gva=" << blobs[idx].gva_);
            keyResults[idx] = gvaRet;
            continue;
        }
        out.keys.push_back(keys[idx]);
        out.vas.push_back(reinterpret_cast<void *>(srcVa));
        out.sizes.push_back(blobs[idx].size_);
        out.validIdx.push_back(idx);
    }
}

size_t MmcLocalServiceDefault::ExecuteFlushBatch(BatchIoParams &params, const std::vector<MmcMemBlobDesc> &blobs,
                                                 std::vector<Result> &keyResults)
{
    std::vector<int> batchResults(params.keys.size(), MMC_ERROR);
    TP_TRACE_BEGIN(TP_MMC_META_ASYNC_FLUSH_BATCH_PUT);
    Result batchRet = ubsIoProxyPtr_->BatchPut(params.keys, params.vas, params.sizes, batchResults);
    TP_TRACE_END(TP_MMC_META_ASYNC_FLUSH_BATCH_PUT, batchRet);

    size_t successCnt = 0;
    for (size_t i = 0; i < params.keys.size(); ++i) {
        if (batchRet == MMC_OK && batchResults[i] == 0) {
            if (IsSsdMetadataSeparated(ssdMetadataMode_)) {
                keyResults[params.validIdx[i]] = MMC_OK;
                ++successCnt;
                continue;
            }
            MmcMemBlobDesc ssdDesc = blobs[params.validIdx[i]];
            ssdDesc.mediaType_ = MEDIA_SSD;
            ssdDesc.gva_ = 0;
            ssdDesc.allocSize_ = 0; // SSD 副本不走 allocator 释放，无切块语义
            // 刷盘成功并写入 blobMap 后才返回成功，保证 keyResults 反映完整结果
            {
                std::lock_guard<std::mutex> guard(blobMutex_);
                auto &descs = blobMap_[params.keys[i]];
                if (AddDesc(descs, ssdDesc)) {
                    keyResults[params.validIdx[i]] = MMC_OK;
                    ++successCnt;
                } else {
                    keyResults[params.validIdx[i]] = MMC_DUPLICATED_OBJECT;
                    MMC_LOG_WARN("Failed to record SSD desc in blobMap, key=" << params.keys[i]);
                }
            }
        } else {
            MMC_LOG_WARN("Failed to flush blob to SSD, key=" << params.keys[i] << ", batchRet=" << batchRet
                                                             << ", result=" << batchResults[i]);
            // UBS 返回码与 MMC 错误码不是一个体系，统一映射为 MMC_ERROR，具体码留在日志中
            keyResults[params.validIdx[i]] = MMC_ERROR;
        }
    }
    return successCnt;
}

void MmcLocalServiceDefault::BatchFlushToSsd(const std::vector<size_t> &indices, const std::vector<std::string> &keys,
                                             const std::vector<MmcMemBlobDesc> &blobs, std::vector<Result> &keyResults)
{
    BatchIoParams params;
    CollectFlushParams(indices, keys, blobs, keyResults, params);
    if (params.keys.empty()) {
        if (!indices.empty()) {
            MMC_LOG_WARN("BatchFlushToSsd all GvaToVa failed, count=" << indices.size());
        }
        return;
    }

    size_t successCnt = ExecuteFlushBatch(params, blobs, keyResults);
    if (successCnt < indices.size()) {
        MMC_LOG_WARN("BatchFlushToSsd completed, success=" << successCnt << "/" << indices.size());
    } else {
        MMC_LOG_DEBUG("BatchFlushToSsd completed, flushed " << successCnt << " blobs");
    }
}

Result MmcLocalServiceDefault::CopyBlob(const std::string &key, const MmcMemBlobDesc &src, const MmcMemBlobDesc &dst)
{
    if (bmProxyPtr_ == nullptr) {
        MMC_LOG_ERROR("bm proxy is null, src=" << src << ", dst=" << dst);
        return MMC_ERROR;
    }

    if (src.mediaType_ == MEDIA_SSD) {
        // 单条 SSD->DRAM 拷贝已移除：回温统一走 BatchCopyBlob->BatchGet
        MMC_LOG_ERROR("single SSD->DRAM copy is not supported, key=" << key << ", src=" << src << ", dst=" << dst);
        return MMC_ERROR;
    }
    auto ret = bmProxyPtr_->Copy(src.gva_, dst.gva_, dst.size_, SMEMB_COPY_G2G);
    if (ret != MMC_OK) {
        MMC_LOG_ERROR("bm put failed:" << ret << ", src=" << src << ", dst=" << dst);
        return MMC_ERROR;
    }
    MMC_LOG_DEBUG("G2G copy ok, key=" << key << ", size=" << dst.size_);
    return MMC_OK;
}

Result MmcLocalServiceDefault::BlobDelete(const std::string &key, const MmcMemBlobDesc &blob)
{
    MMC_LOG_DEBUG("delete blob, key=" << key << ", rank=" << blob.rank_);

    if (ubsIoProxyPtr_ == nullptr) {
        MMC_LOG_ERROR("ubsIo proxy is null or ssd disabled, key=" << key);
        return MMC_SSD_NOT_AVAILABLE;
    }

    if (blob.mediaType_ != MEDIA_SSD) {
        MMC_LOG_ERROR("blob type is mismatch, expected SSD(" << MEDIA_SSD << "), got " << blob.mediaType_
                                                             << ", key=" << key);
        return MMC_ERROR;
    }

    Result ret = ubsIoProxyPtr_->Delete(key);
    if (ret != MMC_OK) {
        MMC_LOG_ERROR("ubsIo delete failed:" << ret << ", key=" << key << ", rank=" << blob.rank_);
        return ret;
    }
    MMC_LOG_DEBUG("Deleted blob successfully, key=" << key);
    return MMC_OK;
}

std::string MmcLocalServiceDefault::ResolveBackendId()
{
    if (options_.backendId[0] != '\0') {
        return std::string(options_.backendId);
    }

    std::string envBackendId = SafeGetEnv("MMC_LOCAL_SERVICE_BACKEND_ID");
    if (envBackendId.empty()) {
        MMC_LOG_WARN("backend_id is not set in config and MMC_LOCAL_SERVICE_BACKEND_ID is empty; "
                     "backend_id will be emitted as empty string");
    }
    return envBackendId;
}
std::vector<Result> MmcLocalServiceDefault::BatchCopyBlob(const std::vector<std::string> &keys,
                                                          const std::vector<MmcMemBlobDesc> &srcBlobs,
                                                          const std::vector<MmcMemBlobDesc> &dstBlobs)
{
    size_t count = keys.size();
    if (count == 0) {
        return {};
    }

    std::vector<Result> results(count, MMC_ERROR);
    if (count != srcBlobs.size() || count != dstBlobs.size()) {
        MMC_LOG_ERROR("size mismatch in batch copy, keys=" << count << ", srcBlobs=" << srcBlobs.size()
                                                           << ", dstBlobs=" << dstBlobs.size());
        return std::vector<Result>(count, MMC_INVALID_PARAM);
    }

    if (bmProxyPtr_ == nullptr) {
        MMC_LOG_ERROR("bm proxy is null in batch copy");
        return results;
    }

    if (srcBlobs.empty() || dstBlobs.empty()) {
        MMC_LOG_ERROR("src or dst blob list empty for batch copy");
        return results;
    }

    bool srcIsSsd = (srcBlobs[0].mediaType_ == MEDIA_SSD);
    bool dstIsSsd = (dstBlobs[0].mediaType_ == MEDIA_SSD);

    if (!srcIsSsd && !dstIsSsd) {
        for (size_t i = 0; i < count; ++i) {
            results[i] = CopyBlob(keys[i], srcBlobs[i], dstBlobs[i]);
        }
        return results;
    }

    if (ubsIoProxyPtr_ == nullptr) {
        MMC_LOG_ERROR("ubsIo proxy unavailable for batch copy");
        return std::vector<Result>(count, MMC_SSD_NOT_AVAILABLE);
    }

    BatchIoParams params;
    CollectBatchIoParams(keys, srcBlobs, dstBlobs, srcIsSsd, results, params);
    ExecuteBatchIo(params, srcIsSsd, results);

    return results;
}

Result MmcLocalServiceDefault::BatchStorageStat(const std::vector<std::string> &keys, std::vector<Result> &results,
                                                std::vector<uint64_t> &sizes)
{
    results.assign(keys.size(), MMC_ERROR);
    sizes.assign(keys.size(), 0);
    if (keys.empty()) {
        return MMC_OK;
    }

    if (!options_.storageEnabled || ubsIoProxyPtr_ == nullptr || !ubsIoProxyPtr_->IsReady()) {
        MMC_LOG_ERROR("SSD storage is unavailable for batch stat, rank=" << options_.rankId
                                                                         << ", keyCount=" << keys.size());
        std::fill(results.begin(), results.end(), MMC_SSD_NOT_AVAILABLE);
        return MMC_SSD_NOT_AVAILABLE;
    }

    TP_TRACE_BEGIN(TP_MMC_LOCAL_UBS_IO_BATCH_STAT);
    Result ret = ubsIoProxyPtr_->BatchStat(keys, sizes, results);
    TP_TRACE_END(TP_MMC_LOCAL_UBS_IO_BATCH_STAT, ret);
    if (sizes.size() != keys.size() || results.size() != keys.size()) {
        MMC_LOG_ERROR("UBSIO batch stat response size mismatch, keys=" << keys.size() << ", sizes=" << sizes.size()
                                                                       << ", results=" << results.size());
        results.assign(keys.size(), MMC_ERROR);
        sizes.assign(keys.size(), 0);
        return MMC_ERROR;
    }
    if (ret != MMC_OK) {
        results.assign(keys.size(), ret);
        return ret;
    }
    return MMC_OK;
}

void MmcLocalServiceDefault::CollectBatchIoParams(const std::vector<std::string> &keys,
                                                  const std::vector<MmcMemBlobDesc> &srcBlobs,
                                                  const std::vector<MmcMemBlobDesc> &dstBlobs, bool srcIsSsd,
                                                  std::vector<Result> &results, BatchIoParams &out)
{
    size_t count = keys.size();
    if (srcBlobs.size() < count || dstBlobs.size() < count || results.size() < count) {
        MMC_LOG_ERROR("size mismatch in batch copy, keys=" << count << ", srcBlobs=" << srcBlobs.size() << ", dstBlobs="
                                                           << dstBlobs.size() << ", results=" << results.size());
        return;
    }
    if (srcIsSsd) {
        for (size_t i = 0; i < count; ++i) {
            if (srcBlobs[i].size_ > dstBlobs[i].size_) {
                MMC_LOG_ERROR("src size " << srcBlobs[i].size_ << " exceeds dst size " << dstBlobs[i].size_
                                          << " in batch copy, key=" << keys[i]);
                continue;
            }
            uint64_t dstVa = 0;
            Result gvaRet =
                bmProxyPtr_->GvaToVa(dstBlobs[i].gva_, static_cast<MediaType>(dstBlobs[i].mediaType_), dstVa);
            if (gvaRet != MMC_OK) {
                MMC_LOG_ERROR("gva_to_va failed for dst gva=" << dstBlobs[i].gva_ << ", key=" << keys[i]
                                                              << ", ret=" << gvaRet);
                results[i] = gvaRet;
                continue;
            }
            out.keys.push_back(keys[i]);
            out.vas.push_back(reinterpret_cast<void *>(dstVa));
            out.sizes.push_back(srcBlobs[i].size_);
            out.validIdx.push_back(i);
        }
    } else {
        for (size_t i = 0; i < count; ++i) {
            if (srcBlobs[i].gva_ == 0 || srcBlobs[i].size_ == 0) {
                MMC_LOG_ERROR("invalid src gva=" << srcBlobs[i].gva_ << " or size=" << srcBlobs[i].size_
                                                 << " in batch copy, key=" << keys[i]);
                results[i] = MMC_INVALID_PARAM;
                continue;
            }
            uint64_t srcVa = 0;
            Result gvaRet =
                bmProxyPtr_->GvaToVa(srcBlobs[i].gva_, static_cast<MediaType>(srcBlobs[i].mediaType_), srcVa);
            if (gvaRet != MMC_OK) {
                MMC_LOG_ERROR("gva_to_va failed for src gva=" << srcBlobs[i].gva_ << ", key=" << keys[i]
                                                              << ", ret=" << gvaRet);
                results[i] = gvaRet;
                continue;
            }
            out.keys.push_back(keys[i]);
            out.vas.push_back(reinterpret_cast<void *>(srcVa));
            out.sizes.push_back(srcBlobs[i].size_);
            out.validIdx.push_back(i);
        }
    }
}

void MmcLocalServiceDefault::ExecuteBatchIo(BatchIoParams &params, bool srcIsSsd, std::vector<Result> &results)
{
    if (params.keys.empty()) {
        return;
    }
    std::vector<int> batchResults(params.keys.size(), MMC_ERROR);
    Result batchRet = MMC_ERROR;
    if (srcIsSsd) {
        TP_TRACE_BEGIN(TP_MMC_LOCAL_UBS_IO_BATCH_GET);
        batchRet = ubsIoProxyPtr_->BatchGet(params.keys, params.vas.data(), params.sizes, batchResults);
        TP_TRACE_END(TP_MMC_LOCAL_UBS_IO_BATCH_GET, batchRet);
    } else {
        TP_TRACE_BEGIN(TP_MMC_LOCAL_UBS_IO_BATCH_PUT);
        batchRet = ubsIoProxyPtr_->BatchPut(params.keys, params.vas, params.sizes, batchResults);
        TP_TRACE_END(TP_MMC_LOCAL_UBS_IO_BATCH_PUT, batchRet);
    }
    for (size_t j = 0; j < params.keys.size(); ++j) {
        if (params.validIdx[j] >= results.size()) {
            MMC_LOG_ERROR("validIdx out of range, idx=" << params.validIdx[j] << ", results.size=" << results.size());
            continue;
        }
        if (batchRet == MMC_OK && batchResults[j] == 0) {
            results[params.validIdx[j]] = MMC_OK;
            MMC_LOG_DEBUG("batch copy ok, key=" << params.keys[j] << ", size=" << params.sizes[j]);
        } else {
            MMC_LOG_ERROR("batch copy failed, key=" << params.keys[j] << " batchRet: " << batchRet
                                                    << " indexRet: " << batchResults[j]);
        }
    }
}
size_t MmcLocalServiceDefault::RemoveLocalSsdBackups(const std::vector<std::string> &keys)
{
    size_t removedCount = 0;
    std::lock_guard<std::mutex> guard(blobMutex_);
    for (const auto &key : keys) {
        auto it = blobMap_.find(key);
        if (it == blobMap_.end()) {
            continue;
        }
        auto &descs = it->second;
        const auto oldSize = descs.size();
        descs.erase(std::remove_if(descs.begin(), descs.end(),
                                   [this](const MmcMemBlobDesc &desc) {
                                       return desc.mediaType_ == MEDIA_SSD && desc.rank_ == options_.rankId;
                                   }),
                    descs.end());
        removedCount += oldSize - descs.size();
        if (descs.empty()) {
            blobMap_.erase(it);
        }
    }
    return removedCount;
}

void MmcLocalServiceDefault::HandleUbsIoMetaEvents(int type, const std::vector<std::string> &keys)
{
    if (keys.empty()) {
        return;
    }
    if (type != UBSIO_META_DELETE) {
        MMC_LOG_ERROR("unknown UBS IO meta event type=" << type << ", keyCount=" << keys.size());
        return;
    }

    // UBS IO has already deleted the data. Clean the recovery descriptors even when Meta is unavailable,
    // and release blobMutex_ before the RPC. A failed notification must not restore stale SSD descriptors.
    const auto removedCount = RemoveLocalSsdBackups(keys);
    MMC_LOG_DEBUG("UBS IO meta DELETE local cleanup, keyCount=" << keys.size() << ", removedCount=" << removedCount);
    if (metaNetClient_ == nullptr) {
        MMC_LOG_WARN("metaNetClient_ is nullptr, skipping UBS IO meta DELETE RPC, keyCount=" << keys.size());
        return;
    }

    UbsIoMetaDeleteRequest request;
    request.rank_ = options_.rankId;
    request.keys_ = keys;
    UbsIoMetaDeleteResponse response;
    TP_TRACE_BEGIN(TP_MMC_UBS_IO_META_DELETE_RPC);
    Result ret = SyncCallMeta(request, response, TIMEOUT_THIRTY);
    TP_TRACE_END(TP_MMC_UBS_IO_META_DELETE_RPC, ret);
    if (ret != MMC_OK || response.ret_ != MMC_OK) {
        MMC_LOG_WARN("UBS IO meta DELETE RPC failed, ret=" << ret << ", resp=" << response.ret_
                                                           << ", keyCount=" << keys.size());
    } else {
        MMC_LOG_DEBUG("UBS IO meta DELETE RPC success, keyCount=" << keys.size());
    }
}

void MmcLocalServiceDefault::StartConfigPolling()
{
    struct stat fileStat;
    if (stat(options_.configFilePath, &fileStat) == 0) {
        lastConfigMtime_ = fileStat.st_mtime;
    } else {
        MMC_LOG_WARN("Failed to stat config file: " << options_.configFilePath);
    }

    auto periodicTask = MmcPeriodicTaskFactory::GetInstance();
    if (periodicTask == nullptr) {
        MMC_LOG_ERROR("Failed to get periodic task instance for config polling");
        return;
    }

    auto ret = periodicTask->RegisterTask(configPollTaskName, options_.dynamicConfigInterval, [this]() {
        struct stat fileStat {};
        if (stat(options_.configFilePath, &fileStat) != 0) {
            MMC_LOG_WARN("Failed to stat config file: " << options_.configFilePath);
            return;
        }
        const bool fileModified = (fileStat.st_mtime != lastConfigMtime_);
        lastConfigMtime_ = fileStat.st_mtime;
        if (fileModified) {
            // Full reload: re-read the file, re-resolve all URL domains, apply changes.
            MMC_LOG_TRACE("Config file modified, reloading...");
            auto ret = UpdateConfig();
            if (ret != MMC_OK) {
                MMC_LOG_WARN("Failed to update config, will retry next cycle, ret=" << ret);
            }
            return;
        }
        // File unchanged: only re-resolve config_store_url from the preserved
        // domain so a DNS repoint (no file change) is still picked up and fed to
        // the BM via UpdateStoreUrl. Cheap: a single getaddrinfo, no file read.
        ReResolveStoreUrl();
    });
    if (!ret) {
        MMC_LOG_ERROR("Failed to register config polling task");
        return;
    }
    if (!periodicTask->IsRunning() && !periodicTask->Start()) {
        MMC_LOG_ERROR("Failed to start periodic task scheduler for config polling");
        return;
    }
    MMC_LOG_TRACE("Config polling task registered, interval: " << options_.dynamicConfigInterval
                                                               << "s, file: " << options_.configFilePath);
}

void MmcLocalServiceDefault::StopConfigPolling()
{
    auto periodicTask = MmcPeriodicTaskFactory::GetInstance();
    if (periodicTask != nullptr) {
        periodicTask->UnregisterTask(configPollTaskName);
    }
}

Result MmcLocalServiceDefault::UpdateConfig()
{
    ClientConfig newConfig;
    if (!newConfig.LoadFromFile(options_.configFilePath)) {
        MMC_LOG_ERROR("Failed to load config from file: " << options_.configFilePath);
        return MMC_ERROR;
    }

    mmc_local_service_config_t newServiceConfig{};
    newConfig.GetLocalServiceConfig(newServiceConfig);

    // Compare and update meta_service_url (discoveryURL)
    std::string oldMetaUrl(options_.discoveryURL);
    std::string newMetaUrl(newServiceConfig.discoveryURL);
    if (oldMetaUrl != newMetaUrl) {
        MMC_LOG_TRACE("meta_service_url changed from " << oldMetaUrl << " to " << newMetaUrl);
        if (metaNetClient_ != nullptr) {
            if (auto ret = metaNetClient_->UpdateServerUrl(newMetaUrl); ret == MMC_OK) {
                SafeCopy(newMetaUrl, options_.discoveryURL, DISCOVERY_URL_SIZE);
            } else {
                MMC_LOG_ERROR("Failed to update meta net client server URL, ret=" << ret << ", url=" << newMetaUrl);
            }
        }
    }

    // Compare and update config_store_url (bmIpPort)
    std::string oldStoreUrl(options_.bmIpPort);
    std::string newStoreUrl(newServiceConfig.bmIpPort);
    // Always refresh the preserved domain so subsequent ReResolveStoreUrl cycles
    // re-resolve the latest configured domain (covers a domain change in the file).
    SafeCopy(newServiceConfig.bmIpPortDomain, options_.bmIpPortDomain, DISCOVERY_URL_SIZE);
    if (oldStoreUrl != newStoreUrl) {
        MMC_LOG_TRACE("config_store_url changed from " << oldStoreUrl << " to " << newStoreUrl);
        if (bmProxyPtr_ != nullptr) {
            if (auto ret = bmProxyPtr_->UpdateStoreUrl(newStoreUrl); ret == MMC_OK) {
                SafeCopy(newStoreUrl, options_.bmIpPort, DISCOVERY_URL_SIZE);
            } else {
                MMC_LOG_ERROR("Failed to update bm store URL, ret=" << ret << ", url=" << newStoreUrl);
            }
        }
    }

    // Compare and update log_level: process-wide MmcOutLogger is owned by the process-level
    // polling task; here we only push the new level to the BM/smem logger.
    if (options_.logLevel != newServiceConfig.logLevel) {
        MMC_LOG_TRACE("log_level changed from " << options_.logLevel << " to " << newServiceConfig.logLevel);
        if (bmProxyPtr_ == nullptr) {
            MMC_LOG_WARN("BM proxy not started, log level update deferred");
        } else if (auto ret = bmProxyPtr_->UpdateLogLevel(newServiceConfig.logLevel); ret == MMC_OK) {
            options_.logLevel = newServiceConfig.logLevel;
        } else {
            MMC_LOG_WARN("Failed to update bm log level to " << newServiceConfig.logLevel << ", ret=" << ret);
        }
    }

    return MMC_OK;
}

void MmcLocalServiceDefault::ReResolveStoreUrl()
{
    // No domain preserved (config_store_url was a literal IP): nothing to re-resolve.
    if (options_.bmIpPortDomain[0] == '\0') {
        return;
    }
    const std::string domain(options_.bmIpPortDomain);
    const std::string resolved = Configuration::ResolveUrlField(domain, "config_store_url");
    // ResolveUrlField returns the original domain when resolution failed, or the
    // same value when it is already a literal IP; in both cases there is no new IP.
    if (resolved.empty() || resolved == domain || resolved == options_.bmIpPort) {
        return;
    }
    MMC_LOG_TRACE("config_store_url re-resolved from " << options_.bmIpPort << " to " << resolved);
    if (bmProxyPtr_ == nullptr || bmProxyPtr_->UpdateStoreUrl(resolved) != MMC_OK) {
        MMC_LOG_WARN("Failed to update bm store URL to " << resolved << ", will retry next cycle");
        return;
    }
    SafeCopy(resolved, options_.bmIpPort, DISCOVERY_URL_SIZE);
}

} // namespace mmc
} // namespace ock
