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
#ifndef MEM_FABRIC_MMC_META_MANAGER_H
#define MEM_FABRIC_MMC_META_MANAGER_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <list>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "mmc_defrag_evict.h"
#include "mmc_global_allocator.h"
#include "mmc_storage_mode.h"
#include "mmc_mem_obj_meta.h"
#include "mmc_meta_container.h"
#include "mmc_meta_backup_mgr.h"
#include "mmc_meta_net_server.h"
#include "mmc_thread_pool.h"

namespace ock {
namespace mmc {

constexpr int METAMGR_POOL_BASE = 16;
constexpr int REWARM_POOL_BASE = 32;
constexpr int REMOVE_POOL_BASE = 16;

struct MmcMetaChangeCallbacks {
    using Callback = std::function<void(const std::string &key, uint32_t rank, uint16_t mediaType)>;
    using ClearedCallback = std::function<void(uint32_t rank, uint16_t mediaType)>;

    mutable std::mutex mutex;
    Callback stored;
    Callback removed;
    ClearedCallback cleared;
};

struct MmcMemMetaDesc {
    uint16_t prot_{0};
    uint8_t priority_{0};
    uint8_t numBlobs_{0};
    uint64_t size_{0};
    std::vector<MmcMemBlobDesc> blobs_;

    MmcMemMetaDesc() = default;
    MmcMemMetaDesc(const uint16_t &prot, const uint8_t &priority, const uint8_t &numBlobs, const uint64_t &size)
        : prot_(prot), priority_(priority), numBlobs_(numBlobs), size_(size)
    {}

    MmcMemMetaDesc(const uint16_t &prot, const uint8_t &priority, const uint8_t &numBlobs, const uint64_t &size,
                   const std::vector<MmcMemBlobPtr> &blobs)
        : prot_(prot), priority_(priority), numBlobs_(numBlobs), size_(size)
    {
        for (const auto &blob : blobs) {
            AddBlob(blob);
        }
    }

    void AddBlob(const MmcMemBlobPtr &blob)
    {
        blobs_.push_back(blob->GetDesc());
    }

    void AddBlobs(const std::vector<MmcMemBlobPtr> &blobs)
    {
        for (const auto &blob : blobs) {
            AddBlob(blob);
        }
    }

    void FillFrom(const MmcMemObjMetaPtr &memObj, const MmcMemBlobPtr &blob)
    {
        prot_ = memObj->Prot();
        priority_ = memObj->Priority();
        size_ = memObj->Size();
        blobs_.clear();
        blobs_.push_back(blob->GetDesc());
        numBlobs_ = static_cast<uint8_t>(blobs_.size());
    }

    uint16_t Prot()
    {
        return prot_;
    };

    uint8_t Priority()
    {
        return priority_;
    };

    uint8_t NumBlobs()
    {
        return numBlobs_;
    };

    uint64_t Size()
    {
        return size_;
    };

    void FillFromMeta(const MmcMemObjMetaPtr &memObj, std::vector<MmcMemBlobDesc> blobDescs)
    {
        prot_ = memObj->Prot();
        priority_ = memObj->Priority();
        size_ = memObj->Size();
        numBlobs_ = static_cast<uint8_t>(blobDescs.size());
        blobs_ = std::move(blobDescs);
    }
};

struct MmcMetaExtConfig {
    bool prefetchEnabled = false;
    uint64_t pendingWaitTimeoutMs = 300U;
};

class MmcMetaManager : public MmcReferable {
    friend class TestMmcMetaManager;

public:
    explicit MmcMetaManager(uint64_t defaultTtl, uint16_t evictThresholdHigh, uint16_t evictThresholdLow,
                            const MmcMetaExtConfig &extConfig = {}, uint32_t evictYieldInterval = 0,
                            uint32_t evictYieldDurationUs = 100,
                            uint32_t lruShardCount = MMC_DEFAULT_META_LRU_SHARD_COUNT,
                            MetaLruType lruType = MetaLruType::LRU)
        : defaultTtlMs_(defaultTtl == 0 ? MMC_DATA_TTL_MS : defaultTtl), evictThresholdHigh_(evictThresholdHigh),
          evictThresholdLow_(evictThresholdLow), extConfig_(extConfig), evictYieldInterval_(evictYieldInterval),
          evictYieldDurationUs_(evictYieldDurationUs), lruShardCount_(lruShardCount), lruType_(lruType)
    {}
    // evictYieldInterval: number of evicted objects between yields (0=disabled, default=0)
    // evictYieldDurationUs: sleep duration per yield in microseconds (default=100us)

    ~MmcMetaManager() override
    {
        Stop();
    }

    // Hot-reload lease TTL and evict high/low watermark without restart. Only applied when
    // the values actually differ. Validation mirrors MmcMetaService::Start. Returns
    // MMC_INVALID_PARAM and keeps current values on failure.
    Result UpdateConfig(uint64_t leaseTtlMs, uint16_t evictThresholdHigh, uint16_t evictThresholdLow);

    // Hot-reload the defrag eviction waste percent threshold independently of the evict watermarks.
    // Returns MMC_INVALID_PARAM and keeps the current value on failure.
    Result UpdateDefragEvictThreshold(uint16_t defragEvictThreshold);

    Result Start()
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (started_) {
            MMC_LOG_TRACE("MmcMetaMgrProxyDefault already started");
            return MMC_OK;
        }
        globalAllocator_ = MmcMakeRef<MmcGlobalAllocator>();
        MMC_ASSERT_LOG_AND_RETURN(globalAllocator_ != nullptr, "globalAllocator_ is nullptr", MMC_MALLOC_FAILED);
        auto GetTypeFunc = [](const MmcMemObjMetaPtr &objMeta) -> MediaType { return objMeta->GetBlobType(); };
        metaContainer_ =
            MmcMetaContainerFactory<std::string, MmcMemObjMetaPtr>::Create(lruType_, GetTypeFunc, lruShardCount_);
        MMC_ASSERT_LOG_AND_RETURN(metaContainer_ != nullptr, "metaContainer_ is nullptr", MMC_MALLOC_FAILED);
        threadPool_ = MmcMakeRef<MmcThreadPool>("metamgr_pool", METAMGR_POOL_BASE);
        MMC_ASSERT_LOG_AND_RETURN(threadPool_ != nullptr, "threadPool_ is nullptr", MMC_MALLOC_FAILED);
        MMC_RETURN_ERROR(threadPool_->Start(), "thread pool start failed");
        rewarmThreadPool_ = MmcMakeRef<MmcThreadPool>("rewarm_pool", REWARM_POOL_BASE);
        MMC_ASSERT_LOG_AND_RETURN(rewarmThreadPool_ != nullptr, "rewarmThreadPool_ is nullptr", MMC_MALLOC_FAILED);
        MMC_RETURN_ERROR(rewarmThreadPool_->Start(), "rewarm thread pool start failed");
        // Dedicated pool for evicting keys that still have active read leases (useClient non-empty).
        // FreeBlobs on such keys blocks in LeaseWait until lease timeout; isolating them avoids
        // starving the metamgr_pool used by Alloc/Get/Update/RPC paths.
        removeThreadPool_ = MmcMakeRef<MmcThreadPool>("remove_pool", REMOVE_POOL_BASE);
        MMC_ASSERT_LOG_AND_RETURN(removeThreadPool_ != nullptr, "removeThreadPool_ is nullptr", MMC_MALLOC_FAILED);
        MMC_RETURN_ERROR(removeThreadPool_->Start(), "remove thread pool start failed");
        // EraseIf 专用线程池：与 shard 数量对齐，上限 kShardLruPoolMax（可按需调整）。
        // 断链清理会一次锁住全部 shard，线程数与 shard 数一致时收益最大。
        constexpr uint32_t kShardLruPoolMax = 32;
        shardLruThreadPool_ =
            MmcMakeRef<MmcThreadPool>("shardlru_pool", std::min<uint32_t>(lruShardCount_, kShardLruPoolMax));
        MMC_ASSERT_LOG_AND_RETURN(shardLruThreadPool_ != nullptr, "shardLruThreadPool_ is nullptr", MMC_MALLOC_FAILED);
        MMC_RETURN_ERROR(shardLruThreadPool_->Start(), "shardlru thread pool start failed");
        metaContainer_->SetEraseIfPool(shardLruThreadPool_);
        // P4: 注册 FreeBlobs 的 SSD 预释放回调，在释放 SSD blob 前通过 RPC 删除远端数据
	// P4: Register an SSD pre-release callback for FreeBlobs to delete remote data
	// via RPC before the SSD blob is released.
        MmcMemBlob::ssdPreFreeHandler_ = [this](const std::string &key, const MmcMemBlobDesc &desc) {
            threadPool_->Enqueue([this, key, desc]() {
                Result ret = BlobDeleteRpc(key, desc);
                if (ret != MMC_OK) {
                    MMC_LOG_WARN("Unable to BlobDeleteRpc for key: " << key << ", rank: " << desc.rank_
                                                                     << ", ret: " << ret);
                }
            });
        };
        started_ = true;
        return MMC_OK;
    }

    void Stop()
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (!started_) {
            return;
        }
        // 先断开容器对线程池的引用，再销毁线程池：避免停止阶段的 EraseIf 走并行分支拿无效 future
        if (metaContainer_ != nullptr) {
            metaContainer_->SetEraseIfPool(nullptr);
        }
        threadPool_->Destroy();
        shardLruThreadPool_->Destroy();
        rewarmThreadPool_->Destroy();
        removeThreadPool_->Destroy();
        MmcMemBlob::ssdPreFreeHandler_ = nullptr;
        started_ = false;
        MMC_LOG_TRACE("Stop MmcMetaManager");
    }

    /**
     * @brief Get the meta object and extend the lease
     * @param key          [in] key of the meta object
     * @param objMeta      [out] the meta object obtained
     */
    Result Get(const std::string &key, uint64_t operateId, MmcBlobFilterPtr filterPtr, MmcMemMetaDesc &objMeta);

    /**
     * @brief Alloc the global memory space and create the meta object
     * @param key          [in] key of the meta object
     * @param metaInfo     [out] the meta object created
     */
    Result Alloc(const std::string &key, const AllocOptions &allocOpt, uint64_t operateId, uint64_t leaseTtlMs,
                 MmcMemMetaDesc &objMeta);

    /**
     * @brief Batch get with Meta classification and rewarm grouped by execution rank
     * @param keys         [in] keys of the meta objects
     * @param operateId    [in] operate id
     * @param objMetas     [out] meta descriptors per key
     * @param sizes        [in] optional sizes already returned by storage stat
     * @param attachReadLease [in] whether to hold a DRAM read lease for the caller
     */
    Result GetByRank(const std::vector<std::string> &keys, uint64_t operateId, std::vector<MmcMemMetaDesc> &objMetas,
                     const std::vector<uint64_t> &sizes = {}, bool attachReadLease = true);

    /**
     * @brief Update the state
     * @param req          [in] update state request
     */
    Result UpdateState(const std::string &key, const MmcLocation &loc, const BlobActionResult &actRet,
                       uint64_t operateId);

    /**
     * @brief remove the meta object
     * @param key          [in] key of the to-be-removed meta object
     */
    Result Remove(const std::string &key);

    /**
     * @brief remove all the keys
     */
    Result RemoveAll();

    /**
     * @brief unmount new mem pool contributor
     * @param loc               [in] location of the new mem pool contributor
     * @param localMemInitInfo  [in] info of the new mem pool contributor
     * @param blobList          [in] if not empty, the allocator will be rebuild from blobList
     */
    Result Mount(const MmcLocation &loc, const MmcLocalMemlInitInfo &localMemInitInfo,
                 std::vector<std::pair<std::string, MmcMemBlobDesc>> &blobList, bool enableMaxValueMode = false);

    Result Mount(const std::vector<MmcLocation> &locs, const std::vector<MmcLocalMemlInitInfo> &localMemInitInfos,
                 std::vector<std::pair<std::string, MmcMemBlobDesc>> &blobList, bool storageEnabled,
                 bool enableMaxValueMode = false);
    /**
     * @brief unmount the mempool contributor at given location
     * @param loc          [in] location of the mem pool contributor to be unmounted
     */
    Result Unmount(const MmcLocation &loc, bool deadRank = false);

    /**
     * @brief Clean SSD blobs of a rank from the meta container
     * @param rank [in] rank whose SSD blobs should be removed
     */
    Result CleanSsdBlobs(uint32_t rank);

    /**
     * @brief Get all segment info: rank, medium, size, used size...
     */
    nlohmann::json GetAllSegmentInfo() const;

    /**
     * @brief Prefetch keys from SSD to DRAM.
     * @param keys          [in] keys of the meta objects to be prefetched
     * @param srcMedia      [in] source media type, default MEDIA_SSD (reserved)
     * @param dstMedia      [in] destination media type, default MEDIA_DRAM (reserved)
     * @param flags         [in] reserved, not used yet
     */
    Result PrefetchKeys(const std::vector<std::string> &keys, MediaType srcMedia = MediaType::MEDIA_SSD,
                        MediaType dstMedia = MediaType::MEDIA_DRAM, uint32_t flags = 0);

    /**
     * @brief Prefetch keys from SSD to DRAM.
     * @param keys             [in] keys to prefetch
     * @param sizes            [in] storage value sizes aligned with keys
     */
    Result AsyncPrefetchKeys(const std::vector<std::string> &keys, const std::vector<uint64_t> &sizes = {});

    /**
     * @brief Prefetch a single key from SSD to DRAM.
     * @param key           [in] key of the meta object to be prefetched
     */
    Result PrefetchKey(const std::string &key)
    {
        return PrefetchKeys({key});
    }

    /**
     * @brief Evict keys from src media to dst media.
     *        Currently only supports DRAM to SSD, other combos are not supported yet.
     *        When SSD is unavailable or DRAM usage is above the rewarm watermark,
     *        the DRAM blob is removed directly.
     * @param keys          [in] keys of the meta objects to be evicted
     * @param srcMedia      [in] source media type, default MEDIA_DRAM (reserved)
     * @param dstMedia      [in] destination media type, default MEDIA_SSD (reserved)
     * @param flags         [in] reserved, not used yet
     */
    Result EvictKeys(std::vector<std::string> keys, MediaType srcMedia = MediaType::MEDIA_DRAM,
                     MediaType dstMedia = MediaType::MEDIA_SSD, uint32_t flags = 0);

    /**
     * @brief Evict a single key from DRAM to SSD.
     * @param key           [in] key of the meta object to be evicted
     */
    Result EvictKey(const std::string &key)
    {
        return EvictKeys({key});
    }

    /**
     * @brief Check if a meta object (key) is in memory
     * @param key          [in] key of the meta object
     */
    Result ExistKey(const std::string &key);
    /**
     * @brief Batch exist check; prefetch SSD-resident keys via aggregated rewarm
     * @param keys          [in] keys of the meta objects
     * @param results       [out] per-key exist results; MMC_OK if exist, MMC_UNMATCHED_KEY otherwise
     */
    Result BatchExist(const std::vector<std::string> &keys, std::vector<Result> &results);

    Result PromoteKey(const std::string &key);

    /**
     * @brief Rewarm blob from SSD to DRAM (P5: SSD→DRAM回温)
     * @param key           [in] key of the meta object
     * @param objMeta       [in] meta object (already looked up, lock held)
     */
    Result RewarmBlob(const std::string &key, const MmcMemObjMetaPtr &objMeta, std::unique_lock<std::mutex> &guard,
                      const MmcMemBlobDesc &srcDesc, MediaType dstMediaType, MmcMemBlobPtr &dstBlob);

    Result RewarmAllocBlob(const std::string &key, const MmcMemBlobDesc &srcDesc, MediaType dstMediaType,
                           const MmcMemObjMetaPtr &objMeta, MmcMemBlobPtr &outBlob, MmcMemBlobDesc &outDesc);
    void RewarmFinalize(const std::string &key, const MmcMemBlobPtr &blob, MediaType dstMediaType, uint32_t srcRank);

    /**
      * @brief Get blob query info with key
      * @param key            [in] key of the meta object
      * @param operateId      [in] operateId of the meta object
      * @param flags          [int] the flags of query operation
      * @param queryInfo      [out] the query info of the meta object
      */
    Result Query(const std::string &key, uint64_t operateId, uint32_t flags, MemObjQueryInfo &queryInfo);

    /**
      * @brief Add a read lease for a readable single-blob key and fill query info
      * @param key            [in] key of the meta object
      * @param operateId      [in] operateId of the lease
      * @param leaseTtlMs     [in] lease time to add, in milliseconds. If 0, use configured default TTL
      * @param queryInfo      [out] the query info of the meta object
      */
    Result AddLease(const std::string &key, uint64_t operateId, uint64_t leaseTtlMs, MemObjQueryInfo &queryInfo);

    /**
      * @brief Remove a read lease for a readable single-blob key
      * @param key            [in] key of the meta object
      * @param operateId      [in] operateId of the lease
      */
    Result RemoveLease(const std::string &key, uint64_t operateId);

    /**
     * @brief Get all keys
     * @param keys           [out] vector to store all keys
     */
    Result GetAllKeys(std::vector<std::string> &keys);

    /**
     * @brief Get the number of keys
     * @param count          [out] number of keys
     */
    Result GetKeyCount(size_t &count);

    /**
     * @brief check and evict meta objects
     */
    void CheckAndEvict(MediaType media, uint64_t wantAllocSize);

    /**
     * @brief 碎片整理式异步淘汰（周期任务入口，紧随 CheckAndEvict 之后执行）：
     *        1. 投递异步任务前同步检测所有 (media, loc)，收集碎片化地址空间
     *           （多段空闲且最大连续空闲块不足 1000MB，大块 put 无法直接满足）
     *        2. 容器读锁内全量构建 blob 与 gva 的关系（冷 key 拷贝池 + (gva, keyIdx) 索引）
     *        3. 索引与分配器 addressTree 记录的空闲地址段归并扫描，构造全量段图
     *        4. 窗口贪心选出段图上地址连续最长的冷 key 序列（LRU 尾部 20%）
     *        5. 对这些 key 进行异步释放，与相邻空闲段合并出更大的连续空闲块
     *        无碎片化地址空间时不投递异步任务
     */
    void DefragEvict();

    /**
     * @brief 同步碎片检测：逐介质枚举已挂载 location，按（总空闲, 最大连续空闲块）
     *        O(1) 判断碎片化（多段空闲且最大连续空闲块不足 1000MB），
     *        收集需整理的地址空间
     * @return (media, 碎片化 loc 列表) 按介质分组，无碎片时为空
     */
    std::vector<std::pair<MediaType, std::vector<MmcLocation>>> DetectDefragLocs();

    bool IsEvictRunning() const noexcept
    {
        return evictCheck_.load(std::memory_order_relaxed);
    }

    inline uint64_t Ttl()
    {
        return defaultTtlMs_.load(std::memory_order_relaxed);
    }

    std::vector<std::pair<uint16_t, uint16_t>> GetEvictWatermark() const
    {
        std::vector<std::pair<uint16_t, uint16_t>> result(MEDIA_NONE);
        for (int i = 0; i < MEDIA_NONE; i++) {
            if (i == MEDIA_SSD) {
                continue;
            }
            result[i] = {evictThresholdHigh_.load(std::memory_order_relaxed),
                         evictThresholdLow_.load(std::memory_order_relaxed)};
        }
        return result;
    }

    /**
     * @brief copy blob to loc
     */
    Result ReplicateBlob(const std::string &key, const MmcLocation &loc);

    /**
     * @brief from src loc copy blob to dst loc, and delete the blobs which belong to src loc
     */
    Result MoveBlob(const std::string &key, const MmcLocation &src, const MmcLocation &dst);

    MmcThreadPoolPtr GetRewarmThreadPool() const
    {
        return rewarmThreadPool_;
    }

    // 临时方案
    void SetMetaNetServer(MetaNetServerPtr metaNetServer)
    {
        metaNetServer_ = metaNetServer;
    }

    void SetChangeCallbacks(const MmcMetaChangeCallbacks &callbacks)
    {
        std::lock_guard<std::mutex> lock(changeCallbacks_.mutex);
        changeCallbacks_.stored = callbacks.stored;
        changeCallbacks_.removed = callbacks.removed;
        changeCallbacks_.cleared = callbacks.cleared;
    }

    // UBS IO metadata event handlers
    Result RemoveSsdBlob(const std::string &key, uint32_t rank);
    Result AddSsdBlob(const std::string &key, const MmcMemBlobDesc &desc);

    // On success, record the original object and lease ID for ReleaseBackupReadLease.
    Result AcquireBackupReadLease(const std::string &key, const MmcMemBlobDesc &source, BackupReadLease &lease);
    void ReleaseBackupReadLease(const BackupReadLease &lease);

    bool IsSsdAvailable(uint32_t rank) const
    {
        std::lock_guard<std::mutex> guard(ssdMutex_);
        return ssdEnabledRanks_.count(rank) > 0;
    }

    bool IsAnySsdEnabled() const
    {
        std::lock_guard<std::mutex> guard(ssdMutex_);
        return !ssdEnabledRanks_.empty();
    }

    /**
     * @brief 设置 SSD 元数据管理模式
     * @param mode             [in] 初始化时确定的元数据管理模式
     */
    void SetSsdMetadataMode(SsdMetadataMode mode)
    {
        ssdMetadataMode_ = mode;
    }

    /**
     * @brief 判断是否启用 SSD 元数据分离模式
     */
    bool IsSsdMetadataSeparated() const
    {
        return ssdMetadataMode_ == MMC_SSD_METADATA_SEPARATED;
    }

    /**
     * @brief 优先选择指定存储 rank，否则从启用存储的 rank 中随机选择
     * @param preferredRank    [in] 优先选择的 rank，UINT32_MAX 表示不指定
     * @param storageRank      [out] 选中的存储 rank
     */
    Result GetRandomStorageRank(uint32_t preferredRank, uint32_t &storageRank) const noexcept;

    /**
     * @brief 通过 RPC 批量查询存储 key 的存在性和大小
     * @param keys             [in] 待查询的 key
     * @param results          [out] 每个 key 的查询结果
     * @param sizes            [out] 每个 key 的存储 value size
     * @param requestRank      [in] 请求来源 rank，用于优先选择存储节点
     */
    Result BatchStorageStatRpc(const std::vector<std::string> &keys, std::vector<Result> &results,
                               std::vector<uint64_t> &sizes, uint32_t requestRank);

    /**
     * @brief 同步查询存储，按预取策略异步回温命中的 key
     * @param keys             [in] 待查询的 key
     * @param results          [out] 每个 key 的查询结果，不等待回温完成
     * @param sizes            [out] 每个 key 的存储 value size
     * @param requestRank      [in] 请求来源 rank，用于优先选择存储节点
     */
    Result BatchLookupStorage(const std::vector<std::string> &keys, std::vector<Result> &results,
                              std::vector<uint64_t> &sizes, uint32_t requestRank);

private:
    Result ExistKeyCore(const std::string &key, MmcMemObjMetaPtr &memObj, MmcMemBlobPtr &upperBlob,
                        MmcMemBlobPtr &lowerBlob);
    Result ResolveAndFillMetaDesc(const std::string &key, uint64_t operateId, MmcBlobFilterPtr filterPtr,
                                  const MmcMemObjMetaPtr &memObj, MmcMemMetaDesc &objMeta);

    Result TryRewarmForGet(const std::string &key, uint64_t operateId, const MmcMemObjMetaPtr &memObj,
                           MmcMemBlobPtr &lowerBlob, std::unique_lock<std::mutex> &guard, MmcMemBlobPtr &selectedBlob);

    Result CopyBlob(const std::string &key, const MmcMemObjMetaPtr &objMeta, const MmcMemBlobDesc &srcBlob,
                    const MmcLocation &dstLoc);

    Result RebuildMeta(std::vector<std::pair<std::string, MmcMemBlobDesc>> &blobList);

    Result RemoveBlobs(uint32_t rank, MediaType mediaType);
    Result RemoveBlobsForDeadRank(uint32_t rank, MediaType mediaType);

    bool CheckActiveLease(const std::string &key, const MmcMemObjMetaPtr &meta, const MmcBlobFilterPtr &filter);

    void PushRemoveList(const std::string &key, const MmcMemObjMetaPtr &meta, const MmcBlobFilterPtr &filter,
                        bool triggerSsdPreFree);

    // 同步执行删除列表（FreeBlobs + removed 回调）。仅在异步入队失败时兜底调用，
    // 契约：调用方必须已持有 meta->Mutex()
    void DoRemoveBlobs(const std::string &key, const MmcMemObjMetaPtr &meta, const MmcBlobFilterPtr &filter,
                       bool triggerSsdPreFree);

    // 发布 KV 事件。内部自行加 changeCallbacks_.mutex 并判空，调用方不得再持有该锁。
    // 契约：不要在持有 meta->Mutex() 时调用（外部回调可能阻塞）。
    void KvEventStored(const std::string &key, uint32_t rank, uint16_t mediaType)
    {
        std::lock_guard<std::mutex> lock(changeCallbacks_.mutex);
        if (changeCallbacks_.stored) {
            changeCallbacks_.stored(key, rank, mediaType);
        }
    }

    void KvEventRemoved(const std::string &key, uint32_t rank, uint16_t mediaType)
    {
        std::lock_guard<std::mutex> lock(changeCallbacks_.mutex);
        if (changeCallbacks_.removed) {
            changeCallbacks_.removed(key, rank, mediaType);
        }
    }

    void KvEventCleared(uint32_t rank, uint16_t mediaType)
    {
        std::lock_guard<std::mutex> lock(changeCallbacks_.mutex);
        if (changeCallbacks_.cleared) {
            changeCallbacks_.cleared(rank, mediaType);
        }
    }

    EvictResult EvictCallBackFunction(const std::string &key, const MmcMemObjMetaPtr &objMeta, MediaType srcMediaType);

    /**
     * @brief 内存碎片场景的强制淘汰（同步执行）：从 LRU 尾部直接强制释放 5% 的 key；
     *        有活跃租约等不可释放的 key 跳过后向后延伸一次补足目标（候选预留一倍余量），遍历完候选仍未达到目标即结束。
     *        仅释放 fragmentedRanks 中 rank 的 blob，避免跨 rank 释放错空间
     * @param media            [in] 介质类型
     * @param fragmentedRanks  [in] 碎片化 rank 列表，仅释放这些 rank 上的 blob
     * @param wantAllocSize    [in] 触发淘汰的申请大小，仅用于日志
     * @return 本次实际释放的 key 个数，0 表示无可释放的 key
     */
    size_t ForceEvictForFragmentation(MediaType media, const std::vector<uint32_t> &fragmentedRanks,
                                      uint64_t wantAllocSize);

    /**
     * @brief 同步强制释放 key 在指定介质上目标 rank 的 blob（与 EvictCallBackFunction 同构但同步执行）：
     *        持锁选取首个命中 targetRanks 的 READABLE blob，目标层有空间时同步 move-down 该 rank 的 blob，
     *        否则同步删除；该 blob 有活跃租约时直接跳过。key 在同介质其他 rank 的副本及其他介质
     *        （如 SSD）的 blob 均保留，避免跨 rank 释放错空间、无谓丢数据
     * @param key         [in] 待释放的 key
     * @param media       [in] 源介质类型
     * @param targetRanks [in] 目标 rank 列表（碎片化 rank），仅释放这些 rank 上的 blob
     * @return true 表示已成功释放；key 不存在、目标 rank 无可释放 READABLE blob 或有活跃租约时返回 false
     */
    bool ForceReleaseKey(const std::string &key, MediaType media, const std::vector<uint32_t> &targetRanks);

    /**
     * @brief 单个介质上的碎片整理式淘汰（异步任务内执行）：对同步检测传入的
     *        碎片化 loc 列表，由容器在锁内全量构建 blob 索引并与空闲段归并出
     *        全量段图，窗口贪心选出地址连续最长的冷 key 序列（LRU 尾部 20%）
     *        后调 EvictDefragTargets 逐个 move-down 释放（与 CheckAndEvict 同构）
     * @param media [in] 目标介质类型
     * @param locs  [in] 同步检测判碎片化的地址空间列表
     */
    void DefragEvictForMedia(MediaType media, const std::vector<MmcLocation> &locs);

    /**
     * @brief 碎片整理冷 key 候选池与释放目标计算：LRU 尾部 20%（最久未使用）为候选，
     *        释放目标为该介质 LRU 总量 5%，均按介质整体计算一次（rank 循环外复用）
     * @param media         [in]  目标介质类型
     * @param coldKeys      [out] 冷 key 候选集合
     * @param releaseTarget [out] 单次释放目标数量（最少 1）
     * @return false 表示无冷 key 候选，应跳过该介质
     */
    bool GetDefragColdKeys(MediaType media, std::unordered_set<std::string> &coldKeys, size_t &releaseTarget);

    /**
     * @brief 碎片整理 blob 地址索引构建：metaLock 读锁内遍历 metaMap，收集属于
     *        (media, rank) 地址空间的 blob 记录 (gva, 4K 对齐大小, keyIdx, 可释放)，
     *        冷 key 同时拷贝入 coldKeyCopies——锁释放后 metaMap 内 key 指针不可解引用。
     *        索引按 gva 升序返回（PickDefragWindow 契约）
     * @param media         [in]  目标介质类型
     * @param rank          [in]  目标 rank（gva 地址空间隔离单位）
     * @param coldKeys      [in]  冷 key 候选集合（标记可释放）
     * @param blobIdx       [out] 按 gva 升序的 blob 索引
     * @param coldKeyCopies [out] keyIdx 到冷 key 字符串的锁内拷贝（防悬垂）
     * @return false 表示该 (media, rank) 上无任何 blob，应跳过
     */
    bool BuildDefragBlobIndex(MediaType media, uint32_t rank, const std::unordered_set<std::string> &coldKeys,
                              std::vector<DefragBlobEntry> &blobIdx,
                              std::unordered_map<uint32_t, std::string> &coldKeyCopies);

    /**
     * @brief 单个 (rank, media) 地址空间上的碎片整理：空闲段快照预判碎片化
     *        （多段空闲且最大连续空闲块不足 1000MB，大块 put 无法直接满足）后，
     *        构建 blob 索引、窗口贪心选中连续冷 key（凑够 5% 目标），
     *        调 EvictDefragTargets 持写锁逐 key move-down 释放
     * @param media         [in] 目标介质类型
     * @param loc           [in] 目标分配器位置
     * @param coldKeys      [in] 冷 key 候选集合
     * @param releaseTarget [in] 单次释放目标数量
     */
    void DefragEvictForRank(MediaType media, MmcLocation loc, const std::unordered_set<std::string> &coldKeys,
                            size_t releaseTarget);

    /**
     * @brief 内存碎片场景下的同步分配：循环执行强制淘汰并重试分配，
     *        直到分配成功或强制淘汰已无可释放的 key，保证一次 Alloc 调用内完成清理与分配
     * @param allocOpt         [in]  分配请求
     * @param media            [in]  发生分配失败的介质类型
     * @param fragmentedRanks  [in]  碎片化 rank 列表，强制淘汰仅释放这些 rank 上的 blob
     * @param originRet        [in]  原始分配失败错误码（无进展或单飞行冲突时原样返回）
     * @param blobs            [out] 分配成功的 blob 列表
     * @return 分配结果，MMC_OK 表示清理后分配成功
     */
    Result AllocWithForceEvict(const AllocOptions &allocOpt, MediaType media,
                               const std::vector<uint32_t> &fragmentedRanks, Result originRet,
                               std::vector<MmcMemBlobPtr> &blobs);

    EvictResult EvictRemoveSrc(const std::string &key, const MmcMemObjMetaPtr &objMeta,
                               const MmcBlobFilterPtr &srcFilter, MediaType dstMedium);
    EvictResult EvictRemoveSrc(const std::string &key, const MmcMemObjMetaPtr &objMeta, MediaType srcMediaType,
                               MediaType dstMedium);

    EvictResult DispatchMoveBlob(const std::string &key, const MmcMemObjMetaPtr &objMeta,
                                 const MmcBlobFilterPtr &srcFilter, const MmcLocation &src, const MmcLocation &dst,
                                 MediaType srcMediaType, MediaType dstMedium);

    Result BlobDeleteRpc(const std::string &key, const MmcMemBlobDesc &blob);

private:
    struct GetByRankContext {
        const std::vector<std::string> &keys;
        std::vector<MmcMemMetaDesc> &objMetas;
        uint32_t opRankId;
        uint32_t opSeq;
        std::vector<MmcMemObjMetaPtr> objs;                 // 原始下标对应的 key 粒度元数据。
        std::map<uint32_t, std::vector<size_t>> rankGroups; // 按执行 rank 分组的回温 key 下标。
        std::vector<size_t> pendingWaitList;                // 等待其他请求完成回温的 key 下标。
        std::vector<size_t> deferredLockList;               // 待统一申请读租约的 DRAM 命中项下标。

        // 以下字段仅用于 SSD 元数据分离模式下的存储查询和 SSD-only key 回温。
        const std::vector<uint64_t> &sizes; // 已查询的 value size；为空时查询 SSD。
        std::vector<size_t> missingIndexes; // Meta 缺失 key 的原始下标。

        /**
         * @brief 初始化批量查询上下文，并从操作标识解析 rank 和序列号
         * @param keyList          [in] 原始请求 key
         * @param id               [in] 操作标识
         * @param metas            [out] 每个 key 的 Blob 元数据
         * @param storageSizes     [in] 可选的存储 value size
         */
        GetByRankContext(const std::vector<std::string> &keyList, uint64_t id, std::vector<MmcMemMetaDesc> &metas,
                         const std::vector<uint64_t> &storageSizes)
            : keys(keyList), objMetas(metas), opRankId(GetRankIdByOperateId(id)), opSeq(GetSequenceByOperateId(id)),
              objs(keyList.size()), sizes(storageSizes)
        {}
    };

    struct RewarmGroupContext {
        uint32_t opRankId;
        uint32_t opSeq;
        const std::vector<std::string> &keys;
        const std::vector<MmcMemObjMetaPtr> &objs;
        std::vector<MmcMemMetaDesc> &objMetas;
    };

    struct BatchRpcData {
        std::vector<std::string> keys;
        std::vector<MmcMemBlobDesc> srcBlobs;
        std::vector<MmcMemBlobDesc> dstBlobs;
        std::vector<size_t> groupIndices;
    };

    struct RewarmCtx {
        uint32_t opRankId = 0;
        uint32_t opSeq = 0;
        MediaType srcMedia = MEDIA_NONE;
        MediaType dstMedia = MEDIA_NONE;
    };

    /**
     * @brief 为 SSD-only key 准备目标 DRAM Blob，并加入对应 rank 的回温分组
     * @param index            [in] key 在原始请求中的下标
     * @param storageSize      [in] 存储 value size
     * @param context          [in/out] 批量查询结果和回温分组
     */
    void PrepareBatchRewarmBlob(size_t index, uint64_t storageSize, GetByRankContext &context);

    /**
     * @brief 为普通 Meta key 准备 SSD 回温任务，并加入对应 rank 的回温分组
     * @param key              [in] 原始请求 key
     * @param index            [in] key 在原始请求中的下标
     * @param memObj           [in] key 粒度元数据对象
     * @param srcBlob          [in] 最底层可读源 Blob
     * @param context          [in/out] 批量查询结果和回温分组
     */
    void PrepareRewarmTask(const std::string &key, size_t index, const MmcMemObjMetaPtr &memObj,
                           const MmcMemBlobPtr &srcBlob, GetByRankContext &context);

    /**
     * @brief 将单个 Meta key 分类为可读、等待写入或需要回温，并归入对应分组
     * @param index            [in] key 在原始请求中的下标
     * @param memObj           [in] 已查询到的 Meta 对象
     * @param context          [in/out] 批量查询结果和回温分组
     */
    void ClassifyAndGroupMetaKey(size_t index, const MmcMemObjMetaPtr &memObj, GetByRankContext &context);

    /**
     * @brief 批量查询并分类 Meta key，记录需要继续查询存储的缺失项
     * @param context          [in/out] 请求 key、查询结果和分类分组
     */
    void ClassifyAndGroupMetaKeys(GetByRankContext &context);

    /**
     * @brief 分离模式下聚合 Meta 缺失项，获取存储大小并准备回温 Blob
     * @param context          [in/out] Meta 缺失项、存储大小和回温分组
     */
    Result ClassifyStorageMisses(GetByRankContext &context);

    void RewarmRankGroup(uint32_t rank, const std::vector<size_t> &indices, const RewarmGroupContext &context,
                         bool attachReadLease = true);

    void AttachReadLocks(const std::vector<std::string> &keys, uint32_t opRankId, uint32_t opSeq,
                         const std::vector<MmcMemObjMetaPtr> &objs, std::vector<MmcMemMetaDesc> &objMetas,
                         const std::vector<size_t> &deferredLockList);

    void PendingWaitAndFill(const std::vector<std::string> &keys, const std::vector<MmcMemObjMetaPtr> &objs,
                            uint32_t opRankId, uint32_t opSeq, std::vector<MmcMemMetaDesc> &objMetas, size_t index,
                            bool attachReadLease, const std::chrono::steady_clock::time_point &deadline);

    Result SendBatchRpc(uint32_t rank, BatchRpcData &batch, std::vector<bool> &copyOk);

    void RollbackEntry(const std::string &key, const MmcMemObjMetaPtr &memObj, const MmcMemBlobPtr &srcBlob,
                       const MmcMemBlobPtr &dstBlob, uint32_t opRankId, uint32_t opSeq);

    void RollbackPendingRewarm(const std::string &key, const MmcMemObjMetaPtr &memObj, uint32_t opRankId,
                               uint32_t opSeq);

    Result ApplyRewarm(const std::string &key, const MmcMemObjMetaPtr &memObj, const MmcMemBlobPtr &srcBlob,
                       const MmcMemBlobDesc &srcDesc, const MmcMemBlobPtr &dstBlob, const RewarmCtx &ctx,
                       MmcMemMetaDesc &objMeta, bool attachReadLease = true);

private:
    std::mutex mutex_;
    bool started_ = false;
    // 常规淘汰（CheckAndEvict）与碎片强制淘汰（ForceEvictForFragmentation）共用单飞行标志：
    // 同一时刻最多一个淘汰在执行，避免两者并发重复释放 key
    std::atomic<bool> evictCheck_{false}; /* if any evict worker (check-and-evict or force evict) is running */

    MmcRef<MmcMetaContainer<std::string, MmcMemObjMetaPtr>> metaContainer_;
    MmcGlobalAllocatorPtr globalAllocator_;
    // std::unordered_set<std::string> evictMap_;
    // ReadWriteLock evictMapLock_;

    std::atomic<uint64_t> defaultTtlMs_; /* default ttl in milliseconds */
    std::atomic<uint16_t> evictThresholdHigh_;
    std::atomic<uint16_t> evictThresholdLow_;
    std::atomic<uint16_t> defragEvictThreshold_{static_cast<uint16_t>(DEFRAG_EVICT_WASTE_PERCENT_THRESHOLD)};
    MmcMetaExtConfig extConfig_;
    uint32_t evictYieldInterval_;   /* GC eviction yield interval (0=disabled) */
    uint32_t evictYieldDurationUs_; /* GC eviction yield sleep duration in microseconds */
    uint32_t lruShardCount_;
    MetaLruType lruType_;
    // 碎片整理式异步淘汰的单飞行标志：与 evictCheck_ 独立，
    // 避免周期任务内 CheckAndEvict 异步未完成时碎片整理始终被跳过；
    // 与其他淘汰并发释放同一 key 时由 objMeta 锁内 GetBlobs 空检查兜底
    std::atomic<bool> defragCheck_{false};
    MetaNetServerPtr metaNetServer_;
    MmcThreadPoolPtr threadPool_;

    MmcMetaChangeCallbacks changeCallbacks_;
    MmcThreadPoolPtr rewarmThreadPool_;
    MmcThreadPoolPtr removeThreadPool_;
    // EraseIf 并行执行专用池：线程数按 shard 数对齐（上限 kShardLruPoolMax）
    MmcThreadPoolPtr shardLruThreadPool_;
    // 由 MetaService 冷启动配置统一设置。
    SsdMetadataMode ssdMetadataMode_{MMC_SSD_METADATA_AGGREGATED};
    std::unordered_set<uint32_t> ssdEnabledRanks_;
    mutable std::mutex ssdMutex_;
};
using MmcMetaManagerPtr = MmcRef<MmcMetaManager>;
} // namespace mmc
} // namespace ock

#endif // MEM_FABRIC_MMC_META_MANAGER_H
