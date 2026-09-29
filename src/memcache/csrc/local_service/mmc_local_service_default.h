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
#ifndef MEM_FABRIC_MMC_LOCAL_SERVICE_DEFAULT_H
#define MEM_FABRIC_MMC_LOCAL_SERVICE_DEFAULT_H

#include <ctime>
#include <utility>

#include "mmc_meta_net_client.h"
#include "mmc_msg_client_meta.h"
#include "mmc_local_service.h"
#include "mmc_bm_proxy.h"
#include "mmc_ubs_io_proxy.h"
#include "mmc_blob_common.h"
#include "mmc_def.h"
#include "mmc_thread_pool.h"
#include "mmc_periodic_task.h"
#include "mmc_storage_mode.h"

namespace ock {
namespace mmc {
constexpr int TIMEOUT_THOUSAND = 1000;
constexpr int UBSIO_EVENT_POOL_SIZE = 4;
// Best-effort timeout for BM unregister during shutdown. MetaService auto-cleans
// registration on link break (ClearResource), so a short timeout suffices.
constexpr int BM_UNREGISTER_TIMEOUT_SECOND = 5;

class MmcLocalServiceDefault : public MmcLocalService {
public:
    explicit MmcLocalServiceDefault(const std::string &name) : name_(name), options_() {}

    ~MmcLocalServiceDefault() override;

    Result Start(const mmc_local_service_config_t &config) override;

    void Stop() override;

    Result RegisterBm();

    Result ReconnectBm();

    Result BuildMeta();

    Result InitBm();

    Result InitUbsIo(int32_t deviceId, const std::string &confPath = "");

    Result DestroyBm();

    Result UpdateMetaBackup(const std::vector<uint32_t> &ops, const std::vector<std::string> &keys,
                            const std::vector<MmcMemBlobDesc> &blobs, std::vector<Result> &keyResults);

    Result CopyBlob(const std::string &key, const MmcMemBlobDesc &src, const MmcMemBlobDesc &dst);

    std::vector<Result> BatchCopyBlob(const std::vector<std::string> &keys, const std::vector<MmcMemBlobDesc> &srcBlobs,
                                      const std::vector<MmcMemBlobDesc> &dstBlobs);

    Result BatchStorageStat(const std::vector<std::string> &keys, std::vector<Result> &results,
                            std::vector<uint64_t> &sizes);

    Result BlobDelete(const std::string &key, const MmcMemBlobDesc &blob);

    const std::string &Name() const override;

    const mmc_local_service_config_t &Options() const override;

    template<typename REQ, typename RESP>
    Result SyncCallMeta(const REQ &req, RESP &resp, int32_t timeoutInSecond)
    {
        return metaNetClient_->SyncCall(req, resp, timeoutInSecond * TIMEOUT_THOUSAND);
    }

    inline MetaNetClientPtr GetMetaClient() const;

private:
    Result GetExternalConfig();

    struct BatchIoParams {
        std::vector<std::string> keys;
        std::vector<void *> vas;
        std::vector<size_t> sizes;
        std::vector<size_t> validIdx;
    };

    // Start meta net client and connect to the MetaService leader. In etcd HA mode the
    // leader address is dynamically assigned and registered with a short etcd lease, so
    // both discovery and connect failures are retried (re-discovering each round).
    Result ConnectMetaNetServer(const NetEngineOptions &netOptions);

    void CollectBatchIoParams(const std::vector<std::string> &keys, const std::vector<MmcMemBlobDesc> &srcBlobs,
                              const std::vector<MmcMemBlobDesc> &dstBlobs, bool srcIsSsd, std::vector<Result> &results,
                              BatchIoParams &out);

    void ExecuteBatchIo(BatchIoParams &params, bool srcIsSsd, std::vector<Result> &results);

    void HandleUbsIoMetaEvents(int type, const std::vector<std::string> &keys);

    size_t RemoveLocalSsdBackups(const std::vector<std::string> &keys);

    std::vector<size_t> ProcessBackupMetadata(const std::vector<uint32_t> &ops, const std::vector<std::string> &keys,
                                              const std::vector<MmcMemBlobDesc> &blobs, size_t length,
                                              std::vector<Result> &keyResults);

    void CollectFlushParams(const std::vector<size_t> &indices, const std::vector<std::string> &keys,
                            const std::vector<MmcMemBlobDesc> &blobs, std::vector<Result> &keyResults,
                            BatchIoParams &out);

    size_t ExecuteFlushBatch(BatchIoParams &params, const std::vector<MmcMemBlobDesc> &blobs,
                             std::vector<Result> &keyResults);

    void BatchFlushToSsd(const std::vector<size_t> &indices, const std::vector<std::string> &keys,
                         const std::vector<MmcMemBlobDesc> &blobs, std::vector<Result> &keyResults);

    Result RecoverBlob(const MmcUbsIoProxyPtr &ubsIoProxy);

    std::pair<size_t, size_t>
    UpdateSsdRebuildBatch(std::map<std::string, std::vector<MmcMemBlobDesc>>::iterator &it,
                          const std::map<std::string, std::vector<MmcMemBlobDesc>>::iterator &batchEnd,
                          const bool *exists);

    void RegisterBaseInfo(BmRegisterRequest &req);

    Result RebuildBm();
    MetaNetClientPtr metaNetClient_;
    MmcBmProxyPtr bmProxyPtr_;
    MmcUbsIoProxyPtr ubsIoProxyPtr_;
    int32_t pid_ = 0;
    std::mutex mutex_;
    std::mutex blobMutex_;
    bool started_ = false;
    std::atomic<bool> stopRequested_{false};
    std::string name_;
    mmc_local_service_config_t options_;
    SsdMetadataMode ssdMetadataMode_{MMC_SSD_METADATA_AGGREGATED};
    std::map<std::string, std::vector<MmcMemBlobDesc>> blobMap_;
    const int32_t blobRebuildSendMaxCount = 10240;

    std::string ResolveBackendId();
    MmcThreadPoolPtr ubsioEventPool_;

    // Dynamic config polling
    static constexpr const char *configPollTaskName = "DynamicConfigPolling";
    time_t lastConfigMtime_{0};

    // HA leader 变更通知（libsmem 重选举线程回调，纯通知不带数据）。经 C 边界
    // （函数指针 + userData）传入；本层只负责注册与下发，地址发现（discoverUrlFunc_）、
    // 幂等比较与重连切换由 MetaNetClient::HandleServerChangedNotify 完成（假死场景
    // 下无 TCP 断链事件时的唯一主动切换路径）。
    static void OnLeaderChanged(void *userData);

    // 经 C API 从 config store 读取当前 leader 登记的 MetaService 地址（"tcp://ip:port"）。
    // 非 etcd 模式或地址不可用时返回空串。供 discoverUrlFunc 注册使用（重连路径经
    // discoverUrlFunc_ 间接复用本逻辑）。
    std::string DiscoverMetaServiceUrl() const;

    void StartConfigPolling();
    void StopConfigPolling();
    Result UpdateConfig();
    void ReResolveStoreUrl();
};

inline const std::string &MmcLocalServiceDefault::Name() const
{
    return name_;
}

inline const mmc_local_service_config_t &MmcLocalServiceDefault::Options() const
{
    return options_;
}

inline MetaNetClientPtr MmcLocalServiceDefault::GetMetaClient() const
{
    return (getpid() == pid_) ? metaNetClient_ : nullptr;
}
} // namespace mmc
} // namespace ock

#endif // MEM_FABRIC_MMC_LOCAL_SERVICE_DEFAULT_H
