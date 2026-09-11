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

#ifndef MF_HYBRID_MMC_META_BACKUP_MGR_DEFAULT_H
#define MF_HYBRID_MMC_META_BACKUP_MGR_DEFAULT_H

#include <atomic>
#include <mutex>
#include <thread>
#include <functional>
#include <list>
#include <condition_variable>
#include <future>
#include <map>
#include "mmc_ref.h"
#include "mmc_logger.h"
#include "mmc_types.h"
#include "mmc_blob_common.h"
#include "mmc_thread_pool.h"
#include "mmc_meta_net_server.h"
#include "mmc_meta_backup_mgr.h"
#include "mmc_mem_obj_meta.h"

namespace ock {
namespace mmc {

constexpr int META_BACKUP_POOL_BASE = 32;

struct MetaBackUpOperate {
    uint32_t op_;
    std::string key_;
    MmcMemBlobDesc desc_;
    MetaBackUpOperate() {}
    MetaBackUpOperate(uint32_t op, const std::string &key, MmcMemBlobDesc &desc) : op_(op), key_(key), desc_(desc) {}
};

struct MMCMetaBackUpConfDefault : public MMCMetaBackUpConf {
    MetaNetServerPtr serverPtr_;
    uint32_t asyncFlushIntervalMs = 0;
    uint32_t asyncFlushBatchLimit = 8;
    std::function<bool(uint32_t)> isSsdAvailableFunc_;
    std::function<Result(const std::string &, const MmcMemBlobDesc &, BackupReadLease &)> acquireReadLease;
    std::function<void(const BackupReadLease &)> releaseReadLease;
    std::function<void(uint32_t, const std::vector<std::pair<std::string, MmcMemBlobDesc>> &)> onAsyncFlushComplete_;

    explicit MMCMetaBackUpConfDefault(MetaNetServerPtr serverPtr) : serverPtr_(serverPtr) {}

    void Setup(
        uint32_t intervalMs, uint32_t batchLimit, std::function<bool(uint32_t)> isSsdAvailableFunc,
        std::function<void(uint32_t, const std::vector<std::pair<std::string, MmcMemBlobDesc>> &)> onAsyncFlushComplete)
    {
        asyncFlushIntervalMs = intervalMs;
        asyncFlushBatchLimit = batchLimit;
        isSsdAvailableFunc_ = std::move(isSsdAvailableFunc);
        onAsyncFlushComplete_ = std::move(onAsyncFlushComplete);
    }
};
using MMCMetaBackUpConfDefaultPtr = MmcRef<MMCMetaBackUpConfDefault>;

class MMCMetaBackUpMgrDefault : public MMCMetaBackUpMgr {
public:
    explicit MMCMetaBackUpMgrDefault() {}

    ~MMCMetaBackUpMgrDefault() override
    {
        Stop();
    }

    Result Start(MMCMetaBackUpConfPtr &confPtr) override
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (started_) {
            MMC_LOG_TRACE("MMCMetaBackUpMgr already started");
            return MMC_OK;
        }
        MMCMetaBackUpConfDefaultPtr defaultPtr = Convert<MMCMetaBackUpConf, MMCMetaBackUpConfDefault>(confPtr);
        if (defaultPtr == nullptr) {
            MMC_LOG_ERROR("confPtr convert failed");
            return MMC_INVALID_PARAM;
        }
        metaNetServer_ = defaultPtr->serverPtr_;
        asyncFlushIntervalMs_ = defaultPtr->asyncFlushIntervalMs;
        asyncFlushBatchLimit_ = defaultPtr->asyncFlushBatchLimit;
        isSsdAvailableFunc_ = defaultPtr->isSsdAvailableFunc_;
        onAsyncFlushComplete_ = defaultPtr->onAsyncFlushComplete_;
        acquireReadLease_ = defaultPtr->acquireReadLease;
        releaseReadLease_ = defaultPtr->releaseReadLease;
        // Backup RPCs can block for up to 60 seconds. Keep them off the latency-sensitive rewarm pool.
        backupPool_ = MmcMakeRef<MmcThreadPool>("backup_pool", META_BACKUP_POOL_BASE);
        MMC_ASSERT_LOG_AND_RETURN(backupPool_ != nullptr, "backupPool_ is nullptr", MMC_MALLOC_FAILED);
        MMC_RETURN_ERROR(backupPool_->Start(), "backup thread pool start failed");
        started_ = true;
        backupThread_ = std::thread(std::bind(&MMCMetaBackUpMgrDefault::BackupThreadFunc, this));
        return MMC_OK;
    }

    void Stop() override
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (!started_) {
            return;
        }
        {
            std::lock_guard<std::mutex> lk(backupThreadLock_);
            started_ = false;
            backupThreadCv_.notify_all();
        }
        backupThread_.join();
        backupPool_->Destroy();
        backupPool_ = nullptr;
        metaNetServer_ = nullptr;
        backupList_.clear();
        MMC_LOG_TRACE("Stop MMCMetaBackUpMgr");
    }
    void BackupThreadFunc();

    Result Add(const std::string &key, MmcMemBlobDesc &blobDesc, uint32_t op) override
    {
        if (!started_) {
            return MMC_OK;
        }

        size_t size = 0;
        {
            std::lock_guard<std::mutex> lk(backupThreadLock_);
            backupList_.push_back({op, key, blobDesc});
            size = backupList_.size();
            if (ShouldNotify(size)) {
                backupThreadCv_.notify_all();
            }
        }
        return MMC_OK;
    }

    Result Remove(const std::string &key, MmcMemBlobDesc &blobDesc) override
    {
        if (!started_) {
            return MMC_OK;
        }
        size_t size = 0;
        {
            std::lock_guard<std::mutex> lk(backupThreadLock_);
            backupList_.push_back({META_BACKUP_REMOVE, key, blobDesc});
            size = backupList_.size();
            if (ShouldNotify(size)) {
                backupThreadCv_.notify_all();
            }
        }
        return MMC_OK;
    }

    Result Load(std::vector<std::pair<std::string, MmcMemBlobDesc>> &blobList) override
    {
        return MMC_OK;
    }

    bool ShouldNotify(size_t queueSize) const
    {
        return queueSize >= asyncFlushBatchLimit_;
    }

private:
    void SendBackup2Local();
    void SendBackupForRank(uint32_t rank, std::vector<MetaBackUpOperate> &entries);

    MetaNetServerPtr metaNetServer_;
    std::mutex mutex_;
    std::atomic<bool> started_{false};
    std::thread backupThread_;
    std::mutex backupThreadLock_;
    std::condition_variable backupThreadCv_;
    std::list<MetaBackUpOperate> backupList_;
    uint32_t asyncFlushIntervalMs_ = 0;
    uint32_t asyncFlushBatchLimit_ = 8;
    MmcThreadPoolPtr backupPool_;
    std::function<bool(uint32_t)> isSsdAvailableFunc_;
    std::function<Result(const std::string &, const MmcMemBlobDesc &, BackupReadLease &)> acquireReadLease_;
    std::function<void(const BackupReadLease &)> releaseReadLease_;
    std::function<void(uint32_t, const std::vector<std::pair<std::string, MmcMemBlobDesc>> &)> onAsyncFlushComplete_;
};
} // namespace mmc
} // namespace ock
#endif // MF_HYBRID_MMC_META_BACKUP_MGR_H
