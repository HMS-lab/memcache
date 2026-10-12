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

#include "mmc_meta_manager.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <memory>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "mmc_logger.h"
#include "mmc_meta_metric_manager.h"
#include "mmc_msg_client_meta.h"
#include "mmc_types.h"
#include "mmc_ptracer.h"
#include "mmc_defrag_evict.h"
#include "mmc_config_const.h"

namespace ock {
namespace mmc {

constexpr int TIMEOUT_SECOND = 60;
constexpr int BATCH_STORAGE_STAT_RPC_TIMEOUT_SECOND = 1;

namespace {
constexpr size_t kSingleBlobCount = 1U;
constexpr size_t kSsdRewarmBlobCount = 2U;
constexpr uint32_t K_BACKUP_LEASE_RANK_ID = UINT32_MAX;
thread_local std::mt19937 g_storageRankRng{std::random_device{}()};

/* 强制淘汰策略：从 LRU 尾部直接强制释放 5% 的 key，有租约等不可释放的 key 跳过后向后延伸一次 */
constexpr uint64_t FORCE_EVICT_RELEASE_PERCENT = 5U;
constexpr uint64_t FORCE_EVICT_PERCENT_DENOM = 100U;
/* 候选取数倍率：目标数量外预留一倍余量，用于租约跳过后的一次向后延伸 */
constexpr uint64_t FORCE_EVICT_FETCH_MULTIPLIER = 2U;

/* 碎片整理释放策略：单次释放上限 5% */
constexpr uint64_t DEFRAG_EVICT_RELEASE_PERCENT = 5U;
constexpr uint64_t DEFRAG_EVICT_CANDIDATE_PERCENT = 20U;
/* 单次分配调用内强制淘汰的最大重试次数：首次失败后最多再重试一次，仍失败则终止，
   避免逐轮清空整个介质缓存造成无意义的数据丢失（如超过任一 rank 的总容量） */
constexpr uint64_t FORCE_EVICT_MAX_RETRY_COUNT = 1U;

bool IsGvaReadableMedia(MediaType mediaType)
{
    return mediaType == MEDIA_HBM || mediaType == MEDIA_DRAM;
}

Result SelectReadableGvaBlob(const std::vector<MmcMemBlobPtr> &blobs, MmcMemBlobPtr &selectedBlob)
{
    selectedBlob = nullptr;
    if (blobs.size() != kSingleBlobCount && blobs.size() != kSsdRewarmBlobCount) {
        return MMC_UNMATCHED_STATE;
    }

    size_t ssdBlobCount = 0;
    size_t gvaReadableBlobCount = 0;
    for (const auto &blob : blobs) {
        if (blob == nullptr) {
            return MMC_UNMATCHED_STATE;
        }

        MediaType mediaType = static_cast<MediaType>(blob->Type());
        if (mediaType == MEDIA_SSD) {
            ++ssdBlobCount;
            continue;
        }

        if (!IsGvaReadableMedia(mediaType)) {
            return MMC_UNMATCHED_STATE;
        }
        ++gvaReadableBlobCount;
        selectedBlob = blob;
    }

    if (gvaReadableBlobCount != kSingleBlobCount || selectedBlob == nullptr || selectedBlob->State() != READABLE) {
        return MMC_UNMATCHED_STATE;
    }
    if (blobs.size() == kSingleBlobCount && ssdBlobCount != 0) {
        return MMC_UNMATCHED_STATE;
    }
    if (blobs.size() == kSsdRewarmBlobCount && ssdBlobCount != kSingleBlobCount) {
        return MMC_UNMATCHED_STATE;
    }
    return MMC_OK;
}

void FillSingleBlobQueryInfo(const MmcMemObjMetaPtr &objMeta, const MmcMemBlobPtr &blob, MemObjQueryInfo &queryInfo)
{
    queryInfo.blobs_.clear();
    queryInfo.blobs_.reserve(kSingleBlobCount);
    queryInfo.blobs_.push_back(blob->GetDesc());
    queryInfo.numBlobs_ = static_cast<uint8_t>(queryInfo.blobs_.size());
    queryInfo.size_ = objMeta->Size();
    queryInfo.prot_ = objMeta->Prot();
    queryInfo.valid_ = true;
}

} // namespace

Result MmcMetaManager::Get(const std::string &key, uint64_t operateId, MmcBlobFilterPtr filterPtr,
                           MmcMemMetaDesc &objMeta)
{
    MmcMemObjMetaPtr memObj;
    TP_TRACE_BEGIN(TP_MMC_META_GET_LOOKUP_MAP);
    auto ret = metaContainer_->Get(key, memObj);
    TP_TRACE_END(TP_MMC_META_GET_LOOKUP_MAP, ret);
    if (ret != MMC_OK) {
        MMC_LOG_ERROR("Get key: " << key << " failed. ErrCode: " << ret);
        return ret;
    }

    TP_TRACE_BEGIN(TP_MMC_META_GET_PROMOTE_LRU);
    ret = metaContainer_->Promote(key);
    TP_TRACE_END(TP_MMC_META_GET_PROMOTE_LRU, ret);
    if (ret != MMC_OK) {
        MMC_LOG_ERROR("Get key: " << key << " Promote failed. ErrCode: " << ret);
        return ret;
    }

    TP_TRACE_BEGIN(TP_MMC_META_GET_FILL_METAOBJ);
    ret = ResolveAndFillMetaDesc(key, operateId, filterPtr, memObj, objMeta);
    TP_TRACE_END(TP_MMC_META_GET_FILL_METAOBJ, ret);
    return ret;
}

struct BlobClassification {
    MmcMemBlobPtr upperBlob;
    MmcMemBlobPtr lowerBlob;
    MmcMemBlobPtr pendingBlob;
};

static BlobClassification ClassifyBlobs(const MmcMemObjMetaPtr &objMeta, MmcBlobFilterPtr filter = nullptr)
{
    BlobClassification result{};
    for (auto &blob : objMeta->GetBlobs(filter)) {
        if (blob == nullptr) {
            continue;
        }
        MediaType type = static_cast<MediaType>(blob->Type());
        bool isLowestTier = (MoveDown(type) == MEDIA_NONE);
        if (isLowestTier) {
            if (blob->State() == READABLE && result.lowerBlob == nullptr) {
                result.lowerBlob = blob;
            }
        } else if (type != MEDIA_NONE) {
            auto state = blob->State();
            if (state == READABLE) {
                result.upperBlob = blob;
                break;
            }
            if (state == ALLOCATED && result.pendingBlob == nullptr) {
                result.pendingBlob = blob;
            }
        }
    }
    return result;
}

static Result WaitPendingRewarm(const std::string &key, const MmcMemObjMetaPtr &memObj, MmcBlobFilterPtr filterPtr,
                                MmcMemBlobPtr &selectedBlob, BlobClassification &classified,
                                std::unique_lock<std::mutex> &guard, const MmcMetaExtConfig &extConfig)
{
    auto timeout = std::chrono::milliseconds(extConfig.pendingWaitTimeoutMs);
    MMC_LOG_DEBUG("rewarm in progress for key " << key << ", waiting " << timeout.count() << "ms");
    TP_TRACE_BEGIN(TP_MMC_META_GET_WAIT_REWARM);
    bool waitOk = memObj->WaitUntil(guard, timeout, [&classified, &memObj, &filterPtr]() {
        classified = ClassifyBlobs(memObj, filterPtr);
        return classified.upperBlob != nullptr || classified.pendingBlob == nullptr;
    });
    TP_TRACE_END(TP_MMC_META_GET_WAIT_REWARM, waitOk ? MMC_OK : MMC_TIMEOUT);
    if (!waitOk || classified.upperBlob == nullptr) {
        MMC_LOG_ERROR("rewarm wait failed for key " << key << ", waitOk=" << waitOk
                                                    << ", timeoutMs=" << timeout.count());
        return waitOk ? MMC_ERROR : MMC_TIMEOUT;
    }
    selectedBlob = classified.upperBlob;
    return MMC_OK;
}

Result MmcMetaManager::TryRewarmForGet(const std::string &key, uint64_t operateId, const MmcMemObjMetaPtr &memObj,
                                       MmcMemBlobPtr &lowerBlob, std::unique_lock<std::mutex> &guard,
                                       MmcMemBlobPtr &selectedBlob)
{
    MediaType srcMedia = static_cast<MediaType>(lowerBlob->Type());
    MediaType dstMedia = MoveUp(srcMedia);
    if (dstMedia == MEDIA_HBM || dstMedia == MEDIA_NONE) {
        MMC_LOG_WARN("Get: skip rewarm, src=" << srcMedia << ", dst=" << dstMedia << ", key=" << key);
        selectedBlob = lowerBlob;
        return MMC_OK;
    }

    uint32_t opRankId = GetRankIdByOperateId(operateId);
    uint32_t opSeq = GetSequenceByOperateId(operateId);
    auto ret = lowerBlob->UpdateState(key, opRankId, opSeq, MMC_READ_START);
    if (ret != MMC_OK) {
        MMC_LOG_ERROR("lowerBlob UpdateState MMC_READ_START failed, key=" << key << ", ret=" << ret);
        return ret;
    }

    MmcMemBlobPtr dstBlob = nullptr;
    TP_TRACE_BEGIN(TP_MMC_META_REWARM);
    auto rewarmRet = RewarmBlob(key, memObj, guard, lowerBlob->GetDesc(), dstMedia, dstBlob);
    TP_TRACE_END(TP_MMC_META_REWARM, rewarmRet);
    auto finishRet = lowerBlob->UpdateState(key, opRankId, opSeq, MMC_READ_FINISH);
    if (finishRet != MMC_OK) {
        MMC_LOG_WARN("Failed to release SSD read lease after rewarm, key=" << key << ", ret=" << finishRet);
    }
    MMC_LOG_DEBUG("Get: rewarm try for key " << key << ", src=" << srcMedia << ", dst=" << dstMedia
                                             << ", ret=" << rewarmRet);
    if (rewarmRet != MMC_OK || dstBlob == nullptr) {
        MMC_LOG_ERROR("rewarm failed for key " << key << ", ret=" << rewarmRet);
        MmcMetaMetricManager::GetInstance().IncrementRewarmFailCounter(lowerBlob->GetDesc().rank_);
        return MMC_ERROR;
    }
    selectedBlob = dstBlob;
    return MMC_OK;
}

Result MmcMetaManager::ResolveAndFillMetaDesc(const std::string &key, uint64_t operateId, MmcBlobFilterPtr filterPtr,
                                              const MmcMemObjMetaPtr &memObj, MmcMemMetaDesc &objMeta)
{
    MMC_LOG_DEBUG("ResolveAndFillMetaDesc key=" << key);

    std::unique_lock<std::mutex> guard(memObj->Mutex());
    auto classified = ClassifyBlobs(memObj, filterPtr);
    auto selectedBlob = classified.upperBlob;
    auto lowerBlob = classified.lowerBlob;
    auto pendingBlob = classified.pendingBlob;
    // 按本次查询路径分类；历史上从 SSD 回温的 READABLE Blob 仍属于内存命中。
    const bool storageHit = selectedBlob == nullptr && ((lowerBlob != nullptr && lowerBlob->Type() == MEDIA_SSD) ||
                                                        (pendingBlob != nullptr && pendingBlob->IsRewarmOrigin()));

    if (selectedBlob == nullptr && pendingBlob != nullptr &&
        (lowerBlob != nullptr || (IsSsdMetadataSeparated() && pendingBlob->IsRewarmOrigin()))) {
        auto waitRet = WaitPendingRewarm(key, memObj, filterPtr, selectedBlob, classified, guard, extConfig_);
        if (waitRet != MMC_OK) {
            return waitRet;
        }
    }

    if (selectedBlob == nullptr && pendingBlob != nullptr) {
        MMC_LOG_WARN("key " << key << " has ALLOCATED blob without lower tier companion, write in progress");
        objMeta.prot_ = memObj->Prot();
        objMeta.priority_ = memObj->Priority();
        objMeta.size_ = memObj->Size();
        objMeta.numBlobs_ = 0;
        return MMC_OK;
    }

    if (selectedBlob == nullptr && lowerBlob != nullptr) {
        auto ret = TryRewarmForGet(key, operateId, memObj, lowerBlob, guard, selectedBlob);
        if (ret != MMC_OK) {
            MMC_LOG_ERROR("rewarm failed for key=" << key << ", ret=" << ret);
            return ret;
        }
    }

    if (selectedBlob == nullptr) {
        MMC_LOG_WARN("Get: no blob available for key " << key);
        objMeta.prot_ = memObj->Prot();
        objMeta.priority_ = memObj->Priority();
        objMeta.size_ = memObj->Size();
        objMeta.numBlobs_ = 0;
        return MMC_OK;
    }

    objMeta.prot_ = memObj->Prot();
    objMeta.priority_ = memObj->Priority();
    objMeta.size_ = memObj->Size();
    uint32_t opRankId = GetRankIdByOperateId(operateId);
    uint32_t opSeq = GetSequenceByOperateId(operateId);
    auto ret = selectedBlob->UpdateState(key, opRankId, opSeq, MMC_READ_START);
    if (ret != MMC_OK) {
        MMC_LOG_ERROR("update key " << key << " blob state failed with error: " << ret);
        return ret;
    } else {
        objMeta.blobs_.push_back(selectedBlob->GetDesc());
    }
    objMeta.numBlobs_ = objMeta.blobs_.size();
    if (storageHit) {
        const uint32_t rank = lowerBlob != nullptr ? lowerBlob->GetDesc().rank_ : selectedBlob->GetDesc().rank_;
        MmcMetaMetricManager::GetInstance().IncrementGetHitSsdCounter(rank);
        MmcMetaMetricManager::GetInstance().IncrementRewarmBytes(selectedBlob->Size(), rank);
    } else {
        MmcMetaMetricManager::GetInstance().IncrementGetHitDramCounter(selectedBlob->GetDesc().rank_);
    }
    return MMC_OK;
}

Result MmcMetaManager::GetByRank(const std::vector<std::string> &keys, uint64_t operateId,
                                 std::vector<MmcMemMetaDesc> &objMetas, const std::vector<uint64_t> &sizes,
                                 bool attachReadLease)
{
    const size_t keyCount = keys.size();
    objMetas.resize(keyCount);

    if (keyCount == 0) {
        return MMC_OK;
    }
    if (!sizes.empty() && sizes.size() != keyCount) {
        MMC_LOG_ERROR("GetByRank storage size mismatch, keys=" << keyCount << ", sizes=" << sizes.size());
        return MMC_INVALID_PARAM;
    }
    CheckAndEvict(MEDIA_DRAM, 0);
    GetByRankContext context(keys, operateId, objMetas, sizes);
    TP_TRACE_BEGIN(TP_MMC_META_BATCH_GET_CLASSIFY);
    ClassifyAndGroupMetaKeys(context);
    Result classifyRet = MMC_OK;
    if (IsSsdMetadataSeparated()) {
        classifyRet = ClassifyStorageMisses(context);
    }
    TP_TRACE_END(TP_MMC_META_BATCH_GET_CLASSIFY, classifyRet);

    // 只把实际回温任务提交到回温线程池。pending wait 会阻塞等待条件变量，若也占用该线程池，
    // 并发 BatchGet 会让等待者排在生产者前面，导致真正的回温任务无法运行。
    std::vector<std::future<void>> rewarmFutures;
    for (auto &[rank, group] : context.rankGroups) {
        RewarmGroupContext rewarmContext{context.opRankId, context.opSeq, context.keys, context.objs, context.objMetas};
        auto future = rewarmThreadPool_->Enqueue([this, rank, &group, rewarmContext, attachReadLease]() {
            RewarmRankGroup(rank, group, rewarmContext, attachReadLease);
        });
        if (!future.valid()) {
            MMC_LOG_ERROR("submit rewarm task failed, rollback pending entries, rank=" << rank);
            for (auto index : group) {
                RollbackPendingRewarm(context.keys[index], context.objs[index], context.opRankId, context.opSeq);
            }
            continue;
        }
        rewarmFutures.emplace_back(std::move(future));
    }
    if (attachReadLease) {
        AttachReadLocks(context.keys, context.opRankId, context.opSeq, context.objs, context.objMetas,
                        context.deferredLockList);
    }

    // pending key 在当前请求线程内等待，并共享同一个截止时间。这样既不会占用回暖 worker，
    // 也不会让包含多个 pending key 的 BatchGet 把 300ms 超时逐 key 串行累加。
    const auto pendingDeadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(extConfig_.pendingWaitTimeoutMs);
    for (auto index : context.pendingWaitList) {
        PendingWaitAndFill(context.keys, context.objs, context.opRankId, context.opSeq, context.objMetas, index,
                           attachReadLease, pendingDeadline);
    }

    for (auto &f : rewarmFutures) {
        try {
            f.get();
        } catch (const std::exception &e) {
            MMC_LOG_WARN("GetByRank future failed: " << e.what());
        }
    }
    return classifyRet;
}

void MmcMetaManager::PrepareBatchRewarmBlob(size_t index, uint64_t storageSize, GetByRankContext &context)
{
    const std::string &key = context.keys[index];
    MmcMemMetaDesc &objMeta = context.objMetas[index];
    if (objMeta.numBlobs_ != 0 && !objMeta.blobs_.empty()) {
        return;
    }
    MmcMemObjMetaPtr existingMeta;
    if (metaContainer_->Get(key, existingMeta) == MMC_OK && existingMeta != nullptr) {
        ClassifyAndGroupMetaKey(index, existingMeta, context);
        return;
    }

    AllocOptions allocOpt{storageSize, kSingleBlobCount, MEDIA_DRAM, {}, 0};
    // 先由 allocator 决定 DRAM Blob 的实际归属，再使用 Blob 自带的 rank 调用同 rank 的 UBSIO。
    std::vector<MmcMemBlobPtr> newBlobs;
    Result ret = globalAllocator_->Alloc(allocOpt, newBlobs);
    if (ret != MMC_OK || newBlobs.empty()) {
        MMC_LOG_WARN("Separated rewarm DRAM alloc failed for key=" << key << ", size=" << storageSize
                                                                   << ", ret=" << ret);
        MmcMetaMetricManager::GetInstance().IncrementRewarmFailCounter(context.opRankId);
        return;
    }

    auto pendingBlob = newBlobs.front();
    const uint32_t blobRank = pendingBlob->GetDesc().rank_;
    pendingBlob->SetRewarmOrigin();
    ret = pendingBlob->UpdateState(key, blobRank, 0, MMC_ALLOCATED_OK);
    if (ret != MMC_OK) {
        MMC_LOG_WARN("Separated rewarm ALLOCATED_OK failed for key=" << key << ", ret=" << ret);
        globalAllocator_->Free(newBlobs);
        MmcMetaMetricManager::GetInstance().IncrementRewarmFailCounter(blobRank);
        return;
    }

    auto memObj = MmcMakeRef<MmcMemObjMeta>();
    if (memObj == nullptr || memObj->AddBlob(pendingBlob) != MMC_OK) {
        pendingBlob->UpdateState(key, blobRank, 0, MMC_WRITE_FAIL);
        globalAllocator_->Free(newBlobs);
        MMC_LOG_WARN("Separated rewarm failed to create meta or add blob, key=" << key);
        MmcMetaMetricManager::GetInstance().IncrementRewarmFailCounter(blobRank);
        return;
    }

    ret = metaContainer_->Insert(key, memObj);
    if (ret != MMC_OK) {
        pendingBlob->UpdateState(key, blobRank, 0, MMC_WRITE_FAIL);
        globalAllocator_->Free(newBlobs);
        if (metaContainer_->Get(key, existingMeta) == MMC_OK && existingMeta != nullptr) {
            ClassifyAndGroupMetaKey(index, existingMeta, context);
        } else {
            MMC_LOG_WARN("Separated rewarm meta insert failed, key=" << key << ", ret=" << ret);
            MmcMetaMetricManager::GetInstance().IncrementRewarmFailCounter(blobRank);
        }
        return;
    }

    context.objs[index] = memObj;
    MmcMetaMetricManager::GetInstance().IncrementRewarmBytesCurrent(pendingBlob->Size(), pendingBlob->GetDesc().rank_);
    context.rankGroups[blobRank].push_back(index);
}

void MmcMetaManager::PrepareRewarmTask(const std::string &key, size_t index, const MmcMemObjMetaPtr &memObj,
                                       const MmcMemBlobPtr &srcBlob, GetByRankContext &context)
{
    auto readRet = srcBlob->UpdateState(key, context.opRankId, context.opSeq, MMC_READ_START);
    if (readRet != MMC_OK) {
        MMC_LOG_WARN("key: " << key << " srcBlob READ_START failed (rewarm), ret=" << readRet);
        return;
    }

    MediaType dstMedia = MoveUp(static_cast<MediaType>(srcBlob->Type()));
    AllocOptions allocOpt(srcBlob->Size(), 1, static_cast<uint16_t>(dstMedia), {srcBlob->GetDesc().rank_},
                          ALLOC_FORCE_BY_RANK);
    std::vector<MmcMemBlobPtr> newBlobs;
    auto allocRet = globalAllocator_->Alloc(allocOpt, newBlobs);
    if (allocRet != MMC_OK || newBlobs.empty()) {
        MMC_LOG_WARN("key: " << key << " rewarm alloc failed, ret=" << allocRet);
        (void)srcBlob->UpdateState(key, context.opRankId, context.opSeq, MMC_READ_FINISH);
        MmcMetaMetricManager::GetInstance().IncrementRewarmFailCounter(srcBlob->GetDesc().rank_);
        return;
    }

    newBlobs[0]->SetRewarmOrigin();
    auto statusRet = newBlobs[0]->UpdateState(key, srcBlob->GetDesc().rank_, 0, MMC_ALLOCATED_OK);
    if (statusRet != MMC_OK) {
        MMC_LOG_WARN("key: " << key << " rewarm ALLOCATED_OK failed, ret=" << statusRet);
        globalAllocator_->Free(newBlobs);
        (void)srcBlob->UpdateState(key, context.opRankId, context.opSeq, MMC_READ_FINISH);
        MmcMetaMetricManager::GetInstance().IncrementRewarmFailCounter(srcBlob->GetDesc().rank_);
        return;
    }

    auto addRet = memObj->AddBlob(newBlobs[0]);
    if (addRet != MMC_OK) {
        MMC_LOG_WARN("key: " << key << " rewarm AddBlob failed, ret=" << addRet);
        globalAllocator_->Free(newBlobs);
        (void)srcBlob->UpdateState(key, context.opRankId, context.opSeq, MMC_READ_FINISH);
        MmcMetaMetricManager::GetInstance().IncrementRewarmFailCounter(srcBlob->GetDesc().rank_);
        return;
    }

    MmcMetaMetricManager::GetInstance().IncrementRewarmBytesCurrent(newBlobs[0]->Size(), newBlobs[0]->GetDesc().rank_);
    context.rankGroups[srcBlob->GetDesc().rank_].push_back(index);
}

void MmcMetaManager::ClassifyAndGroupMetaKey(size_t index, const MmcMemObjMetaPtr &memObj, GetByRankContext &context)
{
    const std::string &key = context.keys[index];
    context.objs[index] = memObj;
    metaContainer_->Promote(key);

    std::unique_lock<std::mutex> guard(memObj->Mutex());
    MmcBlobFilterPtr filterPtr = MmcMakeRef<MmcBlobFilter>(UINT32_MAX, MEDIA_NONE, NONE);
    auto blobs = memObj->GetBlobs(filterPtr);
    if (blobs.empty()) {
        MMC_LOG_WARN("key " << key << " exists but has no blob, respond as unmatched");
        return;
    }

    auto classified = ClassifyBlobs(memObj, filterPtr);
    auto &selectedBlob = classified.upperBlob;
    auto &pendingBlob = classified.pendingBlob;
    auto &lowerBlob = classified.lowerBlob;

    if (selectedBlob != nullptr) {
        context.objMetas[index].FillFrom(memObj, selectedBlob);
        context.deferredLockList.push_back(index);
        return;
    } else if (pendingBlob != nullptr) {
        // rewarm already in progress by another thread, wait concurrently
        context.pendingWaitList.push_back(index);
        return;
    } else if (lowerBlob == nullptr) {
        return;
    }

    MediaType srcMedia = static_cast<MediaType>(lowerBlob->Type());
    MediaType dstMedia = MoveUp(srcMedia);
    if (dstMedia == MEDIA_HBM || dstMedia == MEDIA_NONE) {
        context.objMetas[index].FillFrom(memObj, lowerBlob);
        context.deferredLockList.push_back(index);
        return;
    }

    PrepareRewarmTask(key, index, memObj, lowerBlob, context);
}

void MmcMetaManager::ClassifyAndGroupMetaKeys(GetByRankContext &context)
{
    for (size_t index = 0; index < context.keys.size(); ++index) {
        MmcMemObjMetaPtr memObj;
        auto ret = metaContainer_->Get(context.keys[index], memObj);
        if (ret != MMC_OK) {
            if (IsSsdMetadataSeparated()) {
                // 分离模式下 SSD-only key 不在 Meta 中，记录后统一查询 UBSIO。
                context.missingIndexes.push_back(index);
            } else {
                MMC_LOG_DEBUG("key: " << context.keys[index] << " not found in container, ret: " << ret);
            }
            continue;
        }
        ClassifyAndGroupMetaKey(index, memObj, context);
    }
}

Result MmcMetaManager::ClassifyStorageMisses(GetByRankContext &context)
{
    const auto &keys = context.keys;
    if (context.missingIndexes.empty()) {
        return MMC_OK;
    }

    std::vector<std::string> missingKeys;
    missingKeys.reserve(context.missingIndexes.size());
    for (size_t index : context.missingIndexes) {
        missingKeys.push_back(keys[index]);
    }

    std::vector<Result> statResults(missingKeys.size(), MMC_OK);
    std::vector<uint64_t> statSizes;
    statSizes.reserve(missingKeys.size());
    if (context.sizes.empty()) {
        Result statRet = BatchStorageStatRpc(missingKeys, statResults, statSizes, context.opRankId);
        if (statRet != MMC_OK) {
            return statRet;
        }
    } else {
        for (size_t index : context.missingIndexes) {
            statSizes.push_back(context.sizes[index]);
        }
    }

    for (size_t index = 0; index < context.missingIndexes.size(); ++index) {
        const size_t keyIndex = context.missingIndexes[index];
        if (statResults[index] != MMC_OK) {
            continue;
        }
        PrepareBatchRewarmBlob(keyIndex, statSizes[index], context);
    }
    return MMC_OK;
}

Result MmcMetaManager::SendBatchRpc(uint32_t rank, BatchRpcData &batch, std::vector<bool> &copyOk)
{
    if (batch.keys.empty()) {
        return MMC_OK;
    }
    if (metaNetServer_.Get() == nullptr) {
        MMC_LOG_ERROR("metaNetServer_ is null, rank=" << rank);
        return MMC_ERROR;
    }

    // move 前先拷出 groupIndices，用于回填逐 key 结果
    std::vector<size_t> groupIndices = batch.groupIndices;
    BatchBlobCopyRequest request(std::move(batch.keys), std::move(batch.srcBlobs), std::move(batch.dstBlobs));
    BatchBlobCopyResponse batchResp;
    auto ret = metaNetServer_->SyncCall(rank, request, batchResp, TIMEOUT_SECOND);
    if (ret != MMC_OK) {
        MMC_LOG_ERROR("batch RPC to rank " << rank << " failed, ret=" << ret);
        return ret;
    }
    if (batchResp.results_.size() != groupIndices.size()) {
        MMC_LOG_ERROR("batch RPC resp size mismatch, rank=" << rank << ", expect=" << groupIndices.size()
                                                            << ", got=" << batchResp.results_.size());
        return MMC_ERROR;
    }
    // 逐 key 回填：批量整体成功不代表每个 key 都成功
    for (size_t bi = 0; bi < groupIndices.size(); ++bi) {
        if (batchResp.results_[bi] == MMC_OK) {
            copyOk[groupIndices[bi]] = true;
        } else {
            MMC_LOG_WARN("copy failed for groupIndex=" << groupIndices[bi] << ", rank=" << rank
                                                       << ", ret=" << batchResp.results_[bi]);
        }
    }
    return MMC_OK;
}

void MmcMetaManager::RollbackEntry(const std::string &key, const MmcMemObjMetaPtr &memObj, const MmcMemBlobPtr &srcBlob,
                                   const MmcMemBlobPtr &dstBlob, uint32_t opRankId, uint32_t opSeq)
{
    if (memObj == nullptr) {
        MMC_LOG_ERROR("rewarm rollback: memObj is null, key=" << key);
        return;
    }
    std::unique_lock<std::mutex> guard(memObj->Mutex(), std::adopt_lock);
    if (dstBlob != nullptr && dstBlob->State() == ALLOCATED) {
        const uint32_t failRank = srcBlob != nullptr ? srcBlob->GetDesc().rank_ : dstBlob->GetDesc().rank_;
        Result failRet = dstBlob->UpdateState(key, failRank, 0, MMC_WRITE_FAIL);
        if (failRet != MMC_OK) {
            MMC_LOG_WARN("Failed to abort pending rewarm blob for key=" << key << ", ret=" << failRet);
        }
    }
    MmcBlobFilterPtr rbFilter =
        dstBlob != nullptr
            ? MmcMakeRef<MmcBlobFilter>(dstBlob->GetDesc().rank_, static_cast<MediaType>(dstBlob->Type()), NONE)
            : nullptr;
    auto blobs = rbFilter != nullptr ? memObj->DetachBlobs(rbFilter) : std::vector<MmcMemBlobPtr>{};
    guard.unlock();
    memObj->FinalizeBlobs(key, globalAllocator_, blobs, false);
    guard.lock();
    if (memObj->NumBlobs() == 0) {
        metaContainer_->Erase(key);
    }
    memObj->NotifyReadable();

    if (srcBlob != nullptr) {
        auto finishRet = srcBlob->UpdateState(key, opRankId, opSeq, MMC_READ_FINISH);
        if (finishRet != MMC_OK) {
            MMC_LOG_WARN("Failed to release SSD read lease during rewarm rollback, key=" << key
                                                                                         << ", ret=" << finishRet);
        }
    }

    MMC_LOG_DEBUG("rollback failed rewarm for key=" << key);
    guard.release();
}

void MmcMetaManager::RollbackPendingRewarm(const std::string &key, const MmcMemObjMetaPtr &memObj, uint32_t opRankId,
                                           uint32_t opSeq)
{
    if (memObj == nullptr) {
        MMC_LOG_ERROR("rollback pending rewarm failed, memObj is null, key=" << key);
        return;
    }

    std::unique_lock<std::mutex> guard(memObj->Mutex());
    auto classified = ClassifyBlobs(memObj);
    RollbackEntry(key, memObj, classified.lowerBlob, classified.pendingBlob, opRankId, opSeq);
    if (classified.lowerBlob != nullptr) {
        MmcMetaMetricManager::GetInstance().IncrementRewarmFailCounter(classified.lowerBlob->GetDesc().rank_);
    } else if (classified.pendingBlob != nullptr) {
        MmcMetaMetricManager::GetInstance().IncrementRewarmFailCounter(classified.pendingBlob->GetDesc().rank_);
    }
}

Result MmcMetaManager::ApplyRewarm(const std::string &key, const MmcMemObjMetaPtr &memObj, const MmcMemBlobPtr &srcBlob,
                                   const MmcMemBlobDesc &srcDesc, const MmcMemBlobPtr &dstBlob, const RewarmCtx &ctx,
                                   MmcMemMetaDesc &objMeta, bool attachReadLease)
{
    if (memObj == nullptr || dstBlob == nullptr || (srcBlob == nullptr && !IsSsdMetadataSeparated())) {
        MMC_LOG_ERROR("rewarm apply: memObj or dstBlob is null, key=" << key);
        return MMC_ERROR;
    }
    std::unique_lock<std::mutex> guard(memObj->Mutex(), std::adopt_lock);

    auto releaseSsdLease = [&srcBlob, &key, opRankId = ctx.opRankId, opSeq = ctx.opSeq](const std::string &context) {
        if (srcBlob != nullptr) {
            auto finishRet = srcBlob->UpdateState(key, opRankId, opSeq, MMC_READ_FINISH);
            if (finishRet != MMC_OK) {
                MMC_LOG_WARN("Failed to release SSD read lease " << context << ", key=" << key
                                                                 << ", ret=" << finishRet);
            }
        }
    };

    Result ret = dstBlob->UpdateState(key, srcDesc.rank_, 0, MMC_WRITE_OK);
    if (ret != MMC_OK) {
        MMC_LOG_WARN("WRITE_OK failed for key=" << key << ", ret=" << ret);
        MmcBlobFilterPtr rbFilter = MmcMakeRef<MmcBlobFilter>(dstBlob->GetDesc().rank_, ctx.dstMedia, NONE);
        auto blobs = memObj->DetachBlobs(rbFilter);
        guard.unlock();
        memObj->FinalizeBlobs(key, globalAllocator_, blobs, false);
        guard.lock();
        if (memObj->NumBlobs() == 0) {
            metaContainer_->Erase(key);
        }
        releaseSsdLease("after WRITE_OK failed");
        memObj->NotifyReadable();
        guard.release();
        return ret;
    }

    memObj->NotifyReadable();

    if (attachReadLease) {
        ret = dstBlob->UpdateState(key, ctx.opRankId, ctx.opSeq, MMC_READ_START);
        if (ret != MMC_OK) {
            MMC_LOG_WARN("READ_START failed after WRITE_OK for key=" << key << ", ret=" << ret);
            MmcBlobFilterPtr rbFilter = MmcMakeRef<MmcBlobFilter>(dstBlob->GetDesc().rank_, ctx.dstMedia, NONE);
            auto blobs = memObj->DetachBlobs(rbFilter);
            guard.unlock();
            memObj->FinalizeBlobs(key, globalAllocator_, blobs, false);
            guard.lock();
            if (memObj->NumBlobs() == 0) {
                metaContainer_->Erase(key);
            }
            releaseSsdLease("after READ_START failed");
            memObj->NotifyReadable();
            guard.release();
            return ret;
        }
    }

    releaseSsdLease("after rewarm");

    // 保留源 SSD blob 作为冗余副本，若 DRAM 被淘汰则无需重复 CopyBlob
    objMeta.FillFromMeta(memObj, {dstBlob->GetDesc()});

    metaContainer_->InsertLru(key, ctx.dstMedia);
    MmcMetaMetricManager::GetInstance().IncrementRewarmCounter(srcDesc.rank_);
    if (attachReadLease) {
        MmcMetaMetricManager::GetInstance().IncrementGetHitSsdCounter(srcDesc.rank_);
    }
    MmcMetaMetricManager::GetInstance().IncrementRewarmBytes(srcDesc.size_, srcDesc.rank_);
    guard.release();
    return MMC_OK;
}

void MmcMetaManager::RewarmRankGroup(uint32_t rank, const std::vector<size_t> &indices,
                                     const RewarmGroupContext &context, bool attachReadLease)
{
    size_t groupSize = indices.size();
    if (groupSize == 0) {
        return;
    }
    std::vector<MmcMemBlobDesc> srcDescs(groupSize);

    BatchRpcData batch;
    batch.keys.reserve(groupSize);
    batch.srcBlobs.reserve(groupSize);
    batch.dstBlobs.reserve(groupSize);
    batch.groupIndices.reserve(groupSize);
    std::vector<MmcMemBlobPtr> srcBlobs(groupSize);
    std::vector<MmcMemBlobPtr> dstBlobs(groupSize);
    std::vector<MmcMemBlobDesc> dstDescs(groupSize);
    std::vector<bool> prepared(groupSize, false);
    for (size_t j = 0; j < groupSize; ++j) {
        const auto index = indices[j];
        auto &memObj = context.objs[index];
        if (memObj == nullptr) {
            MMC_LOG_WARN("rewarm: memObj lost for key=" << context.keys[index] << ", rank=" << rank);
            continue;
        }

        std::unique_lock<std::mutex> guard(memObj->Mutex());
        auto classified = ClassifyBlobs(memObj);
        if (classified.lowerBlob == nullptr) {
            if (!IsSsdMetadataSeparated()) {
                MMC_LOG_WARN("rewarm: source blob lost for key=" << context.keys[index] << ", rank=" << rank);
                continue;
            }
        }
        if (classified.pendingBlob == nullptr) {
            MMC_LOG_WARN("rewarm: pending blob lost for key=" << context.keys[index] << ", rank=" << rank);
            continue;
        }

        srcBlobs[j] = classified.lowerBlob;
        dstBlobs[j] = classified.pendingBlob;
        srcDescs[j] = srcBlobs[j] != nullptr ? srcBlobs[j]->GetDesc()
                                             : MmcMemBlobDesc(dstBlobs[j]->GetDesc().rank_, UINT64_MAX,
                                                              dstBlobs[j]->Size(), MEDIA_SSD, READABLE);
        dstDescs[j] = dstBlobs[j]->GetDesc();
        prepared[j] = true;
        batch.keys.push_back(context.keys[index]);
        batch.srcBlobs.push_back(srcBlobs[j] != nullptr ? srcBlobs[j]->GetDesc() : srcDescs[j]);
        batch.dstBlobs.push_back(dstBlobs[j]->GetDesc());
        batch.groupIndices.push_back(j);
    }

    std::vector<bool> copyOk(groupSize, false);
    TP_TRACE_BEGIN(TP_MMC_META_BATCH_GET_REWARM_RPC);
    Result rpcRet = SendBatchRpc(rank, batch, copyOk);
    TP_TRACE_END(TP_MMC_META_BATCH_GET_REWARM_RPC, rpcRet);

    const auto srcMedia = static_cast<MediaType>(srcDescs[0].mediaType_);
    RewarmCtx ctx{context.opRankId, context.opSeq, srcMedia, MoveUp(srcMedia)};

    if (rpcRet != MMC_OK) {
        MMC_LOG_ERROR("batch RPC failed for rank=" << rank << ", ret=" << rpcRet);
    }

    TP_TRACE_BEGIN(TP_MMC_META_BATCH_GET_REWARM_FINALIZE);
    for (size_t j = 0; j < groupSize; ++j) {
        if (!prepared[j]) {
            continue;
        }

        const auto index = indices[j];
        auto &memObj = context.objs[index];
        std::unique_lock<std::mutex> guard(memObj->Mutex());
        auto classified = ClassifyBlobs(memObj);
        const bool srcChanged = classified.lowerBlob == nullptr ? !IsSsdMetadataSeparated()
                                                                : classified.lowerBlob->GetDesc() != srcDescs[j];
        const bool dstChanged = classified.pendingBlob == nullptr || classified.pendingBlob->GetDesc() != dstDescs[j];
        if (srcChanged || dstChanged) {
            MMC_LOG_ERROR("rewarm finalize: blobs changed for key=" << context.keys[index] << ", srcChanged="
                                                                    << srcChanged << ", dstChanged=" << dstChanged);
            if (dstChanged && srcBlobs[j] != nullptr) {
                (void)srcBlobs[j]->UpdateState(context.keys[index], context.opRankId, context.opSeq, MMC_READ_FINISH);
            } else if (!dstChanged) {
                RollbackEntry(context.keys[index], memObj, srcBlobs[j], dstBlobs[j], context.opRankId, context.opSeq);
            }
            MmcMetaMetricManager::GetInstance().IncrementRewarmFailCounter(srcDescs[j].rank_);
            copyOk[j] = false;
            continue;
        }

        if (rpcRet != MMC_OK || !copyOk[j]) {
            RollbackEntry(context.keys[index], memObj, classified.lowerBlob, classified.pendingBlob, context.opRankId,
                          context.opSeq);
            MmcMetaMetricManager::GetInstance().IncrementRewarmFailCounter(srcDescs[j].rank_);
            continue;
        }

        Result ret = ApplyRewarm(context.keys[index], memObj, classified.lowerBlob, srcDescs[j], classified.pendingBlob,
                                 ctx, context.objMetas[index], attachReadLease);
        if (ret != MMC_OK) {
            MmcMetaMetricManager::GetInstance().IncrementRewarmFailCounter(srcDescs[j].rank_);
        }
    }
    TP_TRACE_END(TP_MMC_META_BATCH_GET_REWARM_FINALIZE, MMC_OK);
}

void MmcMetaManager::AttachReadLocks(const std::vector<std::string> &keys, uint32_t opRankId, uint32_t opSeq,
                                     const std::vector<MmcMemObjMetaPtr> &objs, std::vector<MmcMemMetaDesc> &objMetas,
                                     const std::vector<size_t> &deferredLockList)
{
    TP_TRACE_BEGIN(TP_MMC_META_BATCH_GET_READ_START);
    for (auto index : deferredLockList) {
        auto &memObj = objs[index];
        if (memObj == nullptr) {
            MMC_LOG_WARN("GetByRank: deferred memObj lost for key=" << keys[index]);
            continue;
        }

        std::unique_lock<std::mutex> guard(memObj->Mutex());
        auto classified = ClassifyBlobs(memObj);
        auto &blob = classified.upperBlob != nullptr ? classified.upperBlob : classified.lowerBlob;
        if (blob == nullptr) {
            MMC_LOG_WARN("GetByRank: no readable blob for deferred key=" << keys[index]);
            continue;
        }

        auto ret = blob->UpdateState(keys[index], opRankId, opSeq, MMC_READ_START);
        if (ret != MMC_OK) {
            MMC_LOG_WARN("GetByRank: deferred READ_START failed for key=" << keys[index] << ", ret=" << ret);
            continue;
        }

        objMetas[index].FillFrom(memObj, blob);
        MmcMetaMetricManager::GetInstance().IncrementGetHitDramCounter(blob->GetDesc().rank_);
    }
    TP_TRACE_END(TP_MMC_META_BATCH_GET_READ_START, MMC_OK);
}

void MmcMetaManager::PendingWaitAndFill(const std::vector<std::string> &keys, const std::vector<MmcMemObjMetaPtr> &objs,
                                        uint32_t opRankId, uint32_t opSeq, std::vector<MmcMemMetaDesc> &objMetas,
                                        size_t index, bool attachReadLease,
                                        const std::chrono::steady_clock::time_point &deadline)
{
    auto &memObj = objs[index];
    if (memObj == nullptr) {
        MMC_LOG_ERROR("pending wait: memObj is null, key=" << keys[index]);
        return;
    }
    std::unique_lock<std::mutex> guard(memObj->Mutex());
    auto classified = ClassifyBlobs(memObj);
    if (classified.upperBlob == nullptr) {
        const auto now = std::chrono::steady_clock::now();
        if (now < deadline) {
            memObj->WaitUntil(guard, deadline - now, [&classified, &memObj]() {
                classified = ClassifyBlobs(memObj);
                return classified.upperBlob != nullptr || classified.pendingBlob == nullptr;
            });
        }
    }

    if (classified.upperBlob != nullptr) {
        if (attachReadLease) {
            auto readRet = classified.upperBlob->UpdateState(keys[index], opRankId, opSeq, MMC_READ_START);
            if (readRet != MMC_OK) {
                MMC_LOG_WARN("key: " << keys[index] << " pending rewarm READ_START failed, ret=" << readRet);
                return;
            }
            const uint32_t rank = classified.upperBlob->GetDesc().rank_;
            MmcMetaMetricManager::GetInstance().IncrementGetHitDramCounter(rank);
        }
        objMetas[index].FillFromMeta(memObj, {classified.upperBlob->GetDesc()});
    } else {
        MMC_LOG_WARN("key: " << keys[index] << " pending rewarm timeout or no readable blob, pendingState="
                             << (classified.pendingBlob != nullptr ? static_cast<int>(classified.pendingBlob->State())
                                                                   : static_cast<int>(REMOVING)));
    }
}

Result MmcMetaManager::ExistKeyCore(const std::string &key, MmcMemObjMetaPtr &memObj, MmcMemBlobPtr &upperBlob,
                                    MmcMemBlobPtr &lowerBlob)
{
    auto ret = metaContainer_->Get(key, memObj);
    if (ret != MMC_OK) {
        if (ret == MMC_UNMATCHED_KEY) {
            MMC_LOG_DEBUG("Not exist, key:" << key << " ret:" << ret);
        } else {
            MMC_LOG_ERROR("Failed to get key:" << key << " ret:" << ret);
        }
        return ret;
    }

    ret = metaContainer_->Promote(key);
    if (ret != MMC_OK) {
        MMC_LOG_ERROR("ExistKey key: " << key << " Promote failed. ErrCode: " << ret);
        return ret;
    }

    // 在持有锁的单次遍历内完成分类并返回上层/低层可读 blob，
    // 调用方据此判断预取资格，无需再次加锁或再次读 blobs。
    std::unique_lock<std::mutex> guard(memObj->Mutex());
    auto classified = ClassifyBlobs(memObj);
    upperBlob = classified.upperBlob;
    lowerBlob = classified.lowerBlob;
    if (upperBlob == nullptr && lowerBlob == nullptr) {
        MMC_LOG_DEBUG("Key is exist but do not have readable blob key:" << key);
        return MMC_UNMATCHED_KEY;
    }
    return MMC_OK;
}

Result MmcMetaManager::ExistKey(const std::string &key)
{
    MmcMemObjMetaPtr memObj;
    MmcMemBlobPtr upperBlob;
    MmcMemBlobPtr lowerBlob;
    auto ret = ExistKeyCore(key, memObj, upperBlob, lowerBlob);
    if (ret != MMC_OK) {
        return ret;
    }

    // 预取：如果 key 仅存在于低层介质，无上层 READABLE blob 则触发预取
    if (extConfig_.prefetchEnabled && upperBlob == nullptr && lowerBlob != nullptr) {
        MMC_LOG_DEBUG("ExistKey triggering prefetch for key " << key);
        Result prefetchRet = AsyncPrefetchKeys({key});
        if (prefetchRet != MMC_OK) {
            MMC_LOG_WARN("Failed to submit prefetch for key " << key << ", ret: " << prefetchRet);
        }
    }

    return MMC_OK;
}

Result MmcMetaManager::BatchExist(const std::vector<std::string> &keys, std::vector<Result> &results)
{
    results.assign(keys.size(), MMC_UNMATCHED_KEY);
    std::vector<std::string> prefetchCandidates;
    for (size_t i = 0; i < keys.size(); ++i) {
        MmcMemObjMetaPtr memObj;
        MmcMemBlobPtr upperBlob;
        MmcMemBlobPtr lowerBlob;
        Result ret = ExistKeyCore(keys[i], memObj, upperBlob, lowerBlob);
        results[i] = ret;
        if (ret == MMC_OK && extConfig_.prefetchEnabled && upperBlob == nullptr && lowerBlob != nullptr) {
            prefetchCandidates.push_back(keys[i]);
        }
    }

    // 聚合预取：命中的低层介质 key 由同一后台任务按 rank 批量回温。
    if (!prefetchCandidates.empty()) {
        Result prefetchRet = AsyncPrefetchKeys(prefetchCandidates);
        if (prefetchRet != MMC_OK) {
            MMC_LOG_WARN("Failed to submit batch prefetch, key count=" << prefetchCandidates.size()
                                                                       << ", ret: " << prefetchRet);
        }
    }
    return MMC_OK;
}

Result MmcMetaManager::PromoteKey(const std::string &key)
{
    return metaContainer_->Promote(key);
}

void MmcMetaManager::CheckAndEvict(MediaType media, uint64_t wantAllocSize)
{
    auto evictWatermarks = GetEvictWatermark();
    std::vector<uint16_t> nowMemoryThresholds;
    const auto needEvictList =
        globalAllocator_->GetNeedEvictList(evictWatermarks, nowMemoryThresholds, media, wantAllocSize);
    if (needEvictList.empty()) {
        return;
    }
    bool expected = false;
    // 与碎片强制淘汰（AllocWithForceEvict）互斥：共用 evictCheck_ 单飞行，强制淘汰进行中时跳过本轮
    if (!evictCheck_.compare_exchange_strong(expected, true)) {
        return;
    }
    auto moveFunc = [this](const std::string &key, const MmcMemObjMetaPtr &objMeta,
                           MediaType srcMediaType) -> EvictResult {
        return this->EvictCallBackFunction(key, objMeta, srcMediaType);
    };

    auto evictFuture = threadPool_->Enqueue(
        [&](const std::vector<std::pair<uint16_t, uint16_t>> &evictWatermarksL,
            const std::vector<MediaType> &needEvictListL, const std::vector<uint16_t> &nowMemoryThresholds,
            const std::function<EvictResult(const std::string &key, const MmcMemObjMetaPtr &objMeta, MediaType)>
                &moveFuncL) {
            metaContainer_->MultiLevelElimination(evictWatermarksL, needEvictListL, nowMemoryThresholds, moveFuncL,
                                                  evictYieldInterval_, evictYieldDurationUs_);
            bool expected = true;
            evictCheck_.compare_exchange_strong(expected, false);
        },
        evictWatermarks, needEvictList, nowMemoryThresholds, moveFunc);
    if (!evictFuture.valid()) {
        MMC_LOG_ERROR("submit evict task failed");
        evictCheck_.store(false);
    }
}

Result MmcMetaManager::AllocWithForceEvict(const AllocOptions &allocOpt, MediaType media,
                                           const std::vector<uint32_t> &fragmentedRanks, Result originRet,
                                           std::vector<MmcMemBlobPtr> &blobs)
{
    // 与 CheckAndEvict 互斥（共用 evictCheck_ 单飞行）：同一时刻仅一个淘汰在执行，
    // 常规淘汰进行中时本次直接返回原始失败，调用方重试即可
    bool expected = false;
    if (!evictCheck_.compare_exchange_strong(expected, true)) {
        return originRet;
    }
    // RAII 保证任何退出路径（含异常）都恢复单飞行标志，避免标志卡死后所有淘汰失效
    struct EvictFlagGuard {
        std::atomic<bool> &flag_;
        ~EvictFlagGuard()
        {
            flag_.store(false);
        }
    } flagGuard{evictCheck_};

    Result ret = originRet;
    // 同步强制淘汰后重试分配：失败后最多再重试一次，仍失败则终止，
    // 避免逐轮清空整个介质缓存造成无意义的数据丢失（如超过任一 rank 的总容量）
    for (uint64_t retry = 0; retry <= FORCE_EVICT_MAX_RETRY_COUNT; ++retry) {
        // 同步 force evict 释放耗时：从上一次分配失败到本次释放结束（下一次分配开始前）
        TP_TRACE_BEGIN(TP_MMC_META_FORCE_EVICT_RELEASE);
        const size_t releasedCount = ForceEvictForFragmentation(media, fragmentedRanks, allocOpt.blobSize_);
        TP_TRACE_END(TP_MMC_META_FORCE_EVICT_RELEASE, MMC_OK);
        // 无可释放的 key 时终止重试
        if (releasedCount == 0) {
            break;
        }
        ret = globalAllocator_->Alloc(allocOpt, blobs);
        if (ret == MMC_OK) {
            break;
        }
        MMC_LOG_WARN("AllocWithForceEvict times" << retry << ", media: " << media
                                                 << ", blobSize: " << allocOpt.blobSize_);
    }
    return ret;
}

size_t MmcMetaManager::ForceEvictForFragmentation(MediaType media, const std::vector<uint32_t> &fragmentedRanks,
                                                  uint64_t wantAllocSize)
{
    if (media == MEDIA_NONE || media == MEDIA_SSD) {
        MMC_LOG_WARN("ForceEvictForFragmentation skip, invalid media: " << media);
        return 0;
    }
    if (fragmentedRanks.empty()) {
        MMC_LOG_WARN("ForceEvictForFragmentation skip, no fragmented rank on media: " << media);
        return 0;
    }

    size_t totalKeys = metaContainer_->GetKeyCount(media);
    if (totalKeys == 0) {
        MMC_LOG_WARN("ForceEvictForFragmentation skip, no keys on media: " << media);
        return 0;
    }
    const size_t releaseTarget =
        std::max(totalKeys * FORCE_EVICT_RELEASE_PERCENT / FORCE_EVICT_PERCENT_DENOM, static_cast<size_t>(1));

    // 从 LRU 尾部直接强制释放 5% 的 key；有活跃租约等不可释放的 key 跳过后，
    // 向后延伸一次补足目标（候选预留一倍余量），遍历完候选即结束，不再继续延伸
    const size_t fetchCount = std::min(releaseTarget * FORCE_EVICT_FETCH_MULTIPLIER, totalKeys);
    const auto candidateKeys = metaContainer_->GetEvictCandidateKeys(media, fetchCount);

    size_t releasedCount = 0;
    for (const auto &key : candidateKeys) {
        if (releasedCount >= releaseTarget) {
            break;
        }
        // 同步释放：move-down 或直接删除，跳过有活跃租约的 key；仅释放碎片化 rank 上的 blob在 ForceReleaseKey 内部完成
        if (ForceReleaseKey(key, media, fragmentedRanks)) {
            ++releasedCount;
        }
    }

    MMC_LOG_INFO("ForceEvictForFragmentation media: " << media << ", wantAllocSize: " << wantAllocSize
                                                      << ", totalKeys: " << totalKeys << ", releaseTarget: "
                                                      << releaseTarget << ", released: " << releasedCount
                                                      << ", fragmentedRanks.size: " << fragmentedRanks.size());
    return releasedCount;
}

bool MmcMetaManager::ForceReleaseKey(const std::string &key, MediaType media, const std::vector<uint32_t> &targetRanks)
{
    // 与 EvictCallBackFunction 同构但同步执行：双写已保证数据在目标层有副本，
    // 直接同步删除源层 blob 无需 move-down；有活跃租约时跳过（LRU 不动）。
    MmcMemObjMetaPtr objMeta;
    if (metaContainer_->Get(key, objMeta) != MMC_OK || objMeta == nullptr) {
        return false;
    }

    MmcBlobFilterPtr mediaFilter = MmcMakeRef<MmcBlobFilter>(UINT32_MAX, media, READABLE);
    if (mediaFilter == nullptr) {
        MMC_LOG_ERROR("ForceReleaseKey create filter failed, key=" << key);
        return false;
    }

    bool shouldErase = false;
    bool hasSameMediaBlob = false;
    {
        std::unique_lock<std::mutex> guard(objMeta->Mutex());
        // 选取首个命中 targetRanks 的 READABLE blob
        std::vector<MmcMemBlobDesc> evictBlobs;
        objMeta->GetBlobsDesc(evictBlobs, mediaFilter);
        uint32_t evictRank = UINT32_MAX;
        for (const auto &desc : evictBlobs) {
            if (std::find(targetRanks.begin(), targetRanks.end(), desc.rank_) != targetRanks.end()) {
                evictRank = desc.rank_;
                break;
            }
        }
        // 目标 rank 无可释放的 READABLE blob，LRU 不动
        if (evictRank == UINT32_MAX) {
            return false;
        }
        MmcBlobFilterPtr srcFilter = MmcMakeRef<MmcBlobFilter>(evictRank, media, READABLE);
        if (srcFilter == nullptr) {
            MMC_LOG_ERROR("ForceReleaseKey create src filter failed, key=" << key);
            return false;
        }
        if (CheckActiveLease(key, objMeta, srcFilter)) {
            return false;
        }
        MmcMetaMetricManager::GetInstance().IncrementEvictCounter(evictRank);
        // 同步删除：DetachBlobs + FinalizeBlobs，内存立即归还分配器
        DoRemoveBlobs(key, objMeta, srcFilter, false);
        shouldErase = (objMeta->NumBlobs() == 0);
        if (!shouldErase) {
            MmcBlobFilterPtr anyStateFilter = MmcMakeRef<MmcBlobFilter>(UINT32_MAX, media, NONE);
            hasSameMediaBlob = (anyStateFilter != nullptr) && !objMeta->GetBlobs(anyStateFilter).empty();
        }
    }
    // 重新判空（锁序：容器锁 → objMeta 锁，与 EvictDefragTargets 一致）
    if (shouldErase) {
        metaContainer_->EraseIfEmpty(key, [](const MmcMemObjMetaPtr &meta) {
            std::unique_lock<std::mutex> guard(meta->Mutex());
            return meta->NumBlobs() == 0;
        });
    } else if (!hasSameMediaBlob) {
        metaContainer_->InsertLru(key, MEDIA_NONE);
    }
    return true;
}

void MmcMetaManager::DefragEvict()
{
    // defragCheck_ 防止碎片整理定时任务自身并发
    bool expected = false;
    if (!defragCheck_.compare_exchange_strong(expected, true)) {
        MMC_LOG_WARN("DefragEvict skip: previous defrag task still running");
        return;
    }
    MMC_LOG_DEBUG("DefragEvict begin: regular/force eviction in progress");
    // 投递异步任务前同步检测：仅当存在碎片化（多段空闲且不可用空闲占比超阈值）的
    // (media, loc) 时才执行异步整理，无碎片时不空转线程池
    std::vector<std::pair<MediaType, std::vector<MmcLocation>>> defragLocs = DetectDefragLocs();
    if (defragLocs.empty()) {
        MMC_LOG_DEBUG("DefragEvict skip: no fragmented address space");
        defragCheck_.store(false);
        return;
    }

    // 实际释放前与水位淘汰（CheckAndEvict）/强制淘汰（AllocWithForceEvict）互斥：
    // 共用 evictCheck_ 单飞行；检测阶段仅读全局分配器无需互斥，进入释放环节才争用
    bool evictExpected = false;
    if (!evictCheck_.compare_exchange_strong(evictExpected, true)) {
        MMC_LOG_WARN("DefragEvict skip: regular/force eviction in progress");
        defragCheck_.store(false);
        return;
    }

    size_t totalFragmentedLocs = 0;
    for (const auto &defragLoc : defragLocs) {
        totalFragmentedLocs += defragLoc.second.size();
    }
    MMC_LOG_INFO("DefragEvict start: fragmented media groups: " << defragLocs.size()
                                                                << ", fragmented locs: " << totalFragmentedLocs);

    auto future = threadPool_->Enqueue([this, defragLocs = std::move(defragLocs)]() {
        // 碎片化的 (media, loc) 已同步检测得出，media 与 loc 作为入参逐组整理
        for (const auto &defragLoc : defragLocs) {
            DefragEvictForMedia(defragLoc.first, defragLoc.second);
        }
        defragCheck_.store(false);
        evictCheck_.store(false);
    });
    if (!future.valid()) {
        MMC_LOG_ERROR("submit defrag evict task failed");
        defragCheck_.store(false);
        evictCheck_.store(false);
    }
}

std::vector<std::pair<MediaType, std::vector<MmcLocation>>> MmcMetaManager::DetectDefragLocs()
{
    // 同步碎片检测：逐介质枚举已挂载 location，取空闲段快照按碎片浪费率判断
    // （不可用空闲占比超阈值且至少两段空闲才值得整理），结果按介质分组，无碎片时为空
    const uint64_t maxAlignedSize = globalAllocator_->GetMaxAlignedSize();
    const uint64_t minUsableBlockSize = maxAlignedSize != 0 ? maxAlignedSize : DEFRAG_EVICT_MIN_USABLE_BLOCK_SIZE;
    MMC_LOG_INFO("DetectDefragLocs: maxAlignedSize: " << maxAlignedSize
                                                      << ", minUsableBlockSize: " << minUsableBlockSize);
    std::vector<std::pair<MediaType, std::vector<MmcLocation>>> defragLocs;
    for (MediaType media = MEDIA_HBM; media != MEDIA_NONE; media = MoveDown(media)) {
        if (media == MEDIA_SSD) {
            continue;
        }
        std::vector<MmcLocation> fragmentedLocs;
        for (const auto &loc : globalAllocator_->GetMediaLocations(media)) {
            const auto freeSegments = globalAllocator_->GetFreeSegments(loc);
            uint64_t fragmentedRate = 0;
            if (IsDefragWorthy(freeSegments, fragmentedRate, defragEvictThreshold_.load(std::memory_order_relaxed),
                               minUsableBlockSize)) {
                fragmentedLocs.push_back(loc);
            }
            MMC_LOG_INFO("DetectDefragLocs: fragmented loc, media: " << media << ", rank: " << loc.rank_
                                                                     << ", freeSegments: " << freeSegments.size()
                                                                     << ", fragmentedRate: " << fragmentedRate << "%");
        }
        if (!fragmentedLocs.empty()) {
            defragLocs.emplace_back(media, std::move(fragmentedLocs));
        }
    }
    return defragLocs;
}

void MmcMetaManager::DefragEvictForMedia(MediaType media, const std::vector<MmcLocation> &locs)
{
    if (media == MEDIA_NONE || media == MEDIA_SSD) {
        return;
    }

    // 冷 key 候选池与 5% 释放目标按介质计算（rank 循环外只取一次），复用容器现有 LRU 原语
    std::unordered_set<std::string> coldKeys;
    size_t releaseTarget = 0;
    if (!GetDefragColdKeys(media, coldKeys, releaseTarget)) {
        return;
    }

    MMC_LOG_INFO("DefragEvictForMedia start: media: " << media << ", fragmented locs: " << locs.size() << ", coldKeys: "
                                                      << coldKeys.size() << ", releaseTarget: " << releaseTarget);

    // 逐 (rank, media) 分配器整理：不同 rank 的 gva 地址空间独立，不可跨 rank 归并。
    // locs 为同步检测判碎片化的地址空间
    for (const auto &loc : locs) {
        DefragEvictForRank(media, loc, coldKeys, releaseTarget);
    }
}

bool MmcMetaManager::GetDefragColdKeys(MediaType media, std::unordered_set<std::string> &coldKeys,
                                       size_t &releaseTarget)
{
    const size_t totalKeys = metaContainer_->GetKeyCount(media);
    const size_t coldCount =
        std::max(totalKeys * DEFRAG_EVICT_CANDIDATE_PERCENT / DEFRAG_EVICT_PERCENT_DENOM, static_cast<size_t>(1));
    const auto coldKeyVec = metaContainer_->GetEvictCandidateKeys(media, coldCount);
    coldKeys.insert(coldKeyVec.begin(), coldKeyVec.end());
    if (coldKeys.empty()) {
        MMC_LOG_DEBUG("DefragEvictForMedia skip: no cold key, media: " << media);
        return false;
    }
    releaseTarget =
        std::max(totalKeys * DEFRAG_EVICT_RELEASE_PERCENT / DEFRAG_EVICT_PERCENT_DENOM, static_cast<size_t>(1));
    MMC_LOG_INFO("GetDefragColdKeys media: " << media << ", totalKeys: " << totalKeys << ", coldCount: " << coldCount
                                             << ", releaseTarget: " << releaseTarget);
    return true;
}

bool MmcMetaManager::BuildDefragBlobIndex(MediaType media, uint32_t rank,
                                          const std::unordered_set<std::string> &coldKeys,
                                          std::vector<DefragBlobEntry> &blobIdx,
                                          std::unordered_map<uint32_t, std::string> &coldKeyCopies)
{
    // 冷 key 须在读锁内拷贝到 coldKeyCopies——IterateIf 返回后锁已释放，解引用 map 内 key 指针会悬垂
    std::map<std::string, MmcMemObjMetaPtr> matched;
    uint32_t keyCount = 0;
    metaContainer_->IterateIf(
        [&](const std::string &key, const MmcMemObjMetaPtr &objMeta) -> bool {
            // 仅收集属于该 (media, rank) 地址空间的 blob，coldKeys 集合用于标记可释放
            if (objMeta == nullptr) {
                return false;
            }
            const uint32_t idx = keyCount++;
            const bool isCold = coldKeys.count(key) > 0;
            if (isCold) {
                coldKeyCopies.emplace(idx, key);
            }
            std::lock_guard<std::mutex> objGuard(objMeta->Mutex());
            for (const auto &blob : objMeta->GetBlobs()) {
                if (blob != nullptr && static_cast<MediaType>(blob->Type()) == media && blob->Rank() == rank) {
                    blobIdx.push_back({blob->Gva(), MmcBlobAllocator::AllocSizeAlignUp(blob->Size()), idx,
                                       isCold && blob->State() == READABLE});
                }
            }
            return false; // 不收集到 matchedValues，只借 IterateIf 的读锁遍历
        },
        matched);
    // metaMap_ 为 unordered_map，遍历序任意：PickDefragWindow 契约要求 blobIdx 按 gva 升序
    std::sort(blobIdx.begin(), blobIdx.end(),
              [](const DefragBlobEntry &l, const DefragBlobEntry &r) { return l.gva_ < r.gva_; });
    if (blobIdx.empty()) {
        MMC_LOG_DEBUG("DefragEvictForMedia skip: no blob in address space, media: " << media << ", rank: " << rank);
        return false;
    }
    return true;
}

void MmcMetaManager::DefragEvictForRank(MediaType media, MmcLocation loc,
                                        const std::unordered_set<std::string> &coldKeys, size_t releaseTarget)
{
    // 检测与投递之间存在时滞，重新预判碎片状态：已消除（单段连续或不可用空闲
    // 占比已降至阈值以下）则跳过该地址空间
    // 可用块阈值优先取分配器历史上出现过的最大对齐分配块（字节），无历史记录（0）时回退默认 3MB
    const uint64_t maxAlignedSize = globalAllocator_->GetMaxAlignedSize();
    const uint64_t minUsableBlockSize = maxAlignedSize != 0 ? maxAlignedSize : DEFRAG_EVICT_MIN_USABLE_BLOCK_SIZE;
    MMC_LOG_INFO("DefragEvictForRank: media: " << media << ", rank: " << loc.rank_ << ", maxAlignedSize: "
                                               << maxAlignedSize << ", minUsableBlockSize: " << minUsableBlockSize);
    const auto freeSegments = globalAllocator_->GetFreeSegments(loc);
    uint64_t beforeFragmentedRate = 0;
    if (!IsDefragWorthy(freeSegments, beforeFragmentedRate, defragEvictThreshold_.load(std::memory_order_relaxed),
                        minUsableBlockSize)) {
        MMC_LOG_DEBUG("DefragEvictForRank skip: no longer fragmented, media: " << media << ", rank: " << loc.rank_);
        return;
    }

    // blob 索引构建在 manager 层完成（此处 Value=MmcMemObjMetaPtr，可安全解引用 GetBlobs()/Mutex()）
    std::vector<DefragBlobEntry> blobIdx;
    std::unordered_map<uint32_t, std::string> coldKeyCopies;
    if (!BuildDefragBlobIndex(media, loc.rank_, coldKeys, blobIdx, coldKeyCopies)) {
        return;
    }

    const auto bestKeyIdx = PickDefragWindow(freeSegments, blobIdx, releaseTarget);
    if (bestKeyIdx.empty()) {
        MMC_LOG_DEBUG("DefragEvictForMedia: no releasable consecutive window, media: "
                      << media << ", rank: " << loc.rank_ << ", coldKeys: " << coldKeys.size()
                      << ", blobs: " << blobIdx.size());
        return;
    }

    std::vector<std::string> targets;
    targets.reserve(bestKeyIdx.size());
    // 同一 key 的多个 blob 可能落入不同窗口而被选中两次，拷贝而非 move 避免二次取出为空串；
    // 重复 target 由 EvictDefragTargets 锁内 find 兜底（首次 REMOVE 后二次未命中跳过）
    for (const uint32_t idx : bestKeyIdx) {
        targets.push_back(coldKeyCopies.at(idx));
    }

    MMC_LOG_INFO("DefragEvict media: " << media << ", rank: " << loc.rank_ << ", freeSegments: " << freeSegments.size()
                                       << ", fragmentedRate: " << beforeFragmentedRate << "%"
                                       << ", coldKeys: " << coldKeys.size() << ", blobs: " << blobIdx.size()
                                       << ", releaseTarget: " << releaseTarget
                                       << ", selected consecutive keys: " << targets.size());

    // 与 CheckAndEvict 同构：复用 EvictCallBackFunction 做 move-down
    auto moveFunc = [this](const std::string &key, const MmcMemObjMetaPtr &objMeta,
                           MediaType srcMediaType) -> EvictResult {
        return this->EvictCallBackFunction(key, objMeta, srcMediaType);
    };
    const size_t evicted =
        metaContainer_->EvictDefragTargets(targets, moveFunc, media, evictYieldInterval_, evictYieldDurationUs_);
    const auto freeSegmentsAfter = globalAllocator_->GetFreeSegments(loc);
    const uint64_t afterFragmentedRate = GetDefragWastePercent(freeSegmentsAfter, minUsableBlockSize);
    MMC_LOG_INFO("DefragEvict done: media: " << media << ", rank: " << loc.rank_ << ", targets: " << targets.size()
                                             << ", evicted: " << evicted << ", fragmentedRate: " << afterFragmentedRate
                                             << "%");
}

Result MmcMetaManager::Alloc(const std::string &key, const AllocOptions &allocOpt, uint64_t operateId,
                             uint64_t leaseTtlMs, MmcMemMetaDesc &objMeta)
{
    MmcMemObjMetaPtr tempMetaObj = MmcMakeRef<MmcMemObjMeta>();
    if (tempMetaObj == nullptr) {
        MMC_LOG_ERROR("Fail to malloc tempMetaObj");
        return MMC_MALLOC_FAILED;
    }
    std::vector<MmcMemBlobPtr> blobs;
    MMC_LOG_DEBUG("Blob allocating key=" << key << ", " << allocOpt);
    Result ret = globalAllocator_->Alloc(allocOpt, blobs);
    if (ret != MMC_OK) {
        globalAllocator_->Free(blobs);
        // 内存碎片场景：水位线正常，但申请大小超过当前系统中最大的单个连续空闲块，
        // 同步执行强制淘汰并在本次调用内重试分配，确保清理完成后即可分配成功
        MediaType media = static_cast<MediaType>(allocOpt.mediaType_);
        if (media == MEDIA_NONE) {
            media = globalAllocator_->GetTopLayerMediumType();
        }
        if (media != MEDIA_NONE && media != MEDIA_SSD && globalAllocator_->IsFragmentation(media, allocOpt.blobSize_)) {
            // 逐 rank 检测碎片：Alloc 经 ArrangeLocality/RandomAssign 可能在任意 rank 上失败，
            // preferredRank_ 不能反映实际失败的 rank，须检查该介质所有已挂载 rank
            auto fragmentedRanks = globalAllocator_->GetFragmentedRanks(media, allocOpt.blobSize_);
            if (!fragmentedRanks.empty()) {
                MMC_LOG_WARN("Alloc failed by fragmentation, media: "
                             << media << ", wantSize: " << allocOpt.blobSize_
                             << ", maxContiguousFree: " << globalAllocator_->GetMaxContiguousFreeSize(media)
                             << ", fragmentedRanks: " << fragmentedRanks.size() << ", trigger force evict");
                ret = AllocWithForceEvict(allocOpt, media, fragmentedRanks, ret, blobs);
                if (ret == MMC_OK) {
                    MMC_LOG_INFO("Alloc succeed after force evict, key=" << key);
                }
            }
        }
        if (ret != MMC_OK) {
            MMC_LOG_ERROR("Alloc " << allocOpt.blobSize_ << " failed, ret:" << ret);
            return ret;
        }
    }

    const uint64_t actualLeaseTtlMs = leaseTtlMs == 0 ? defaultTtlMs_.load(std::memory_order_relaxed) : leaseTtlMs;
    MMC_LOG_DEBUG("Alloc key=" << key << ", leaseTtlMs=" << leaseTtlMs
                               << ", defaultTtlMs_=" << defaultTtlMs_.load(std::memory_order_relaxed)
                               << ", actualLeaseTtlMs=" << actualLeaseTtlMs);
    for (auto &blob : blobs) {
        blob->SetDefaultLeaseTtlMs(actualLeaseTtlMs);
        MMC_LOG_DEBUG("Blob allocated, key=" << key << ", size=" << blob->Size() << ", rank=" << blob->Rank());
        tempMetaObj->AddBlob(blob);
    }

    ret = metaContainer_->Insert(key, tempMetaObj);
    if (ret != MMC_OK) {
        globalAllocator_->Free(blobs);
        if (ret != MMC_DUPLICATED_OBJECT) {
            MMC_LOG_ERROR("Fail to insert " << key << " into MmcMetaContainer. ret:" << ret);
        }
    }

    if (ret == MMC_DUPLICATED_OBJECT && (allocOpt.flags_ & ALLOC_FLAGS_GVA_MALLOC_MASK)) {
        tempMetaObj = nullptr;
        auto repRet = metaContainer_->Get(key, tempMetaObj);
        if (repRet != MMC_OK || tempMetaObj == nullptr) {
            MMC_LOG_ERROR("Unexcept error! key: " << key << " not find in MmcMetaContainer. ret:" << repRet);
            ret = MMC_ERROR;
        }
    }

    if (ret == MMC_OK || (ret == MMC_DUPLICATED_OBJECT && (allocOpt.flags_ & ALLOC_FLAGS_GVA_MALLOC_MASK))) {
        std::unique_lock<std::mutex> guard(tempMetaObj->Mutex());
        uint32_t opRankId = GetRankIdByOperateId(operateId);
        uint32_t opSeq = GetSequenceByOperateId(operateId);
        for (auto &blob : tempMetaObj->GetBlobs()) {
            if (blob != nullptr) {
                BlobActionResult actionRet = (ret == MMC_DUPLICATED_OBJECT) ? MMC_REPEAT_ALLOC : MMC_ALLOCATED_OK;
                auto leaseRet = blob->UpdateState(key, opRankId, opSeq, actionRet);
                if (leaseRet != MMC_OK) {
                    MMC_LOG_WARN("Alloc: grant write lease failed, key=" << key << ", ret=" << leaseRet);
                }
            }
        }
        objMeta.prot_ = tempMetaObj->Prot();
        objMeta.priority_ = tempMetaObj->Priority();
        objMeta.size_ = tempMetaObj->Size();
        tempMetaObj->GetBlobsDesc(objMeta.blobs_);
        objMeta.numBlobs_ = objMeta.blobs_.size();

        if ((allocOpt.flags_ & ALLOC_FLAGS_GVA_MALLOC_MASK)) {
            ret = MMC_OK;
        }
    }
    return ret;
}

Result MmcMetaManager::UpdateState(const std::string &key, const MmcLocation &loc, const BlobActionResult &actRet,
                                   uint64_t operateId)
{
    uint32_t opRankId = GetRankIdByOperateId(operateId);
    uint32_t opSeq = GetSequenceByOperateId(operateId);
    MMC_LOG_DEBUG("UpdateState enter, key=" << key << ", loc=" << loc << ", action=" << static_cast<uint32_t>(actRet)
                                            << ", opRank=" << opRankId << ", opSeq=" << opSeq);

    Result ret;
    if (actRet == MMC_WRITE_FAIL) {
        MMC_LOG_DEBUG("UpdateState: WRITE_FAIL for key=" << key << ", loc=" << loc << ", removing key");
        ret = Remove(key);
        if (ret != MMC_OK) {
            MMC_LOG_ERROR("UpdateState: Failed remove key " << key << ", ret: " << ret);
        }
        return ret;
    }

    MmcMemObjMetaPtr metaObj;
    // when update state, do not update the lru
    Result result = MMC_OK;
    ret = metaContainer_->Get(key, metaObj);
    if (ret != MMC_OK || metaObj == nullptr) {
        MMC_LOG_DEBUG("UpdateState: Cannot find " << key << " memObjMeta! ret:" << ret
                                                  << ", action:" << static_cast<uint32_t>(actRet) << ", loc=" << loc);
        return MMC_UNMATCHED_KEY;
    }
    MmcBlobFilterPtr filter = MmcMakeRef<MmcBlobFilter>(loc.rank_, loc.mediaType_, NONE);
    std::vector<std::pair<uint32_t, uint16_t>> storedBlobs;
    {
        std::unique_lock<std::mutex> guard(metaObj->Mutex());

        std::vector<MmcMemBlobPtr> blobs = metaObj->GetBlobs(filter);

        if (blobs.empty()) {
            MMC_LOG_WARN("UpdateState: no matching blobs for key=" << key << ", loc=" << loc
                                                                   << ", action=" << static_cast<uint32_t>(actRet));
        }

        for (auto blob : blobs) {
            MMC_LOG_DEBUG("UpdateState: updating blob, key=" << key << ", gva=" << blob->Gva()
                                                             << ", curState=" << static_cast<uint32_t>(blob->State())
                                                             << ", newState=" << static_cast<uint32_t>(actRet));
            auto ret = blob->UpdateState(key, opRankId, opSeq, actRet);
            if (ret != MMC_OK) {
                MMC_LOG_ERROR("UpdateState: blob UpdateState failed, key="
                              << key << ", gva=" << blob->Gva() << ", rank=" << opRankId << ", seq=" << opSeq
                              << ", curState=" << static_cast<uint32_t>(blob->State())
                              << ", action=" << static_cast<uint32_t>(actRet) << ", ret=" << ret);
                result = MMC_ERROR;
                continue;
            } else if (blob->State() == READABLE) {
                metaObj->NotifyReadable();
            }
            if (actRet == MMC_WRITE_OK) {
                storedBlobs.emplace_back(blob->Rank(), blob->Type());
            }
        }
    }
    // 回调在 metaObj 锁外发布，避免持锁调用外部回调
    for (const auto &b : storedBlobs) {
        KvEventStored(key, b.first, b.second);
    }
    MMC_LOG_DEBUG("UpdateState exit, key=" << key << ", loc=" << loc << ", action=" << static_cast<uint32_t>(actRet)
                                           << ", result=" << result);
    return result;
}

bool MmcMetaManager::CheckActiveLease(const std::string &key, const MmcMemObjMetaPtr &meta,
                                      const MmcBlobFilterPtr &filter)
{
    // 契约：调用方必须已持有 meta->Mutex()，因此 GetBlobs 遍历是安全的
    bool hasActiveLease = false;
    uint32_t leaseUseCount = 0;
    uint64_t leaseRemainTtlMs = 0;
    const char *leaseType = "read";
    for (const auto &blob : meta->GetBlobs(filter)) {
        if (blob != nullptr && blob->UseCount() > 0) {
            hasActiveLease = true;
            leaseUseCount = blob->UseCount();
            leaseRemainTtlMs = blob->LeaseTimeoutTtlMs();
            leaseType = (blob->State() == ALLOCATED) ? "write" : "read";
            break;
        }
    }
    if (hasActiveLease) {
        MMC_LOG_WARN("Remove key=" << key << " with active " << leaseType << " lease, useCount=" << leaseUseCount
                                   << ", remainTtlMs=" << leaseRemainTtlMs);
    }
    return hasActiveLease;
}

void MmcMetaManager::PushRemoveList(const std::string &key, const MmcMemObjMetaPtr &meta,
                                    const MmcBlobFilterPtr &filter, bool triggerSsdPreFree)
{
    bool hasActiveLease = CheckActiveLease(key, meta, filter);

    TP_TRACE_RECORD(TP_MMC_META_EVICT_DELETE_COUNT, 1000ULL, hasActiveLease ? -1 : 0);
    const MmcThreadPoolPtr &pool = hasActiveLease ? removeThreadPool_ : threadPool_;

    auto future = pool->Enqueue(
        [this](const std::string keyL, const MmcMemObjMetaPtr metaL, MmcBlobFilterPtr filterL,
               bool triggerSsdPreFreeL) {
            std::unique_lock<std::mutex> guard(metaL->Mutex());
            DoRemoveBlobs(keyL, metaL, filterL, triggerSsdPreFreeL);
            return MMC_OK;
        },
        key, meta, filter, triggerSsdPreFree);
    if (!future.valid()) {
        // 异步入队失败（线程池已停止）：调用方已持锁，直接同步执行
        DoRemoveBlobs(key, meta, filter, triggerSsdPreFree);
    }
}

void MmcMetaManager::DoRemoveBlobs(const std::string &key, const MmcMemObjMetaPtr &meta, const MmcBlobFilterPtr &filter,
                                   bool triggerSsdPreFree)
{
    // 契约：调用方必须已持有 meta->Mutex()（异步 lambda 内自行加锁，同步兜底由调用方持锁）
    auto blobs = meta->DetachBlobs(filter);
    std::unique_lock<std::mutex> adoptGuard(meta->Mutex(), std::adopt_lock);
    adoptGuard.unlock();
    meta->FinalizeBlobs(key, globalAllocator_, blobs, true, triggerSsdPreFree);
    adoptGuard.lock();
    adoptGuard.release();
    for (const auto &blob : blobs) {
        KvEventRemoved(key, blob->Rank(), blob->Type());
    }
    // 不在此处 Erase：容器层删除由各调用方负责（evict 外层/Remove/RemoveAll 已完成容器删除），
    // 异步重复 Erase 既有 ABA 风险（可能删掉并发重建的新对象），
    // 也会与 evict 外层/EraseAll 持有的容器写锁形成自死锁，此处只需释放 blob
}

Result MmcMetaManager::BlobDeleteRpc(const std::string &key, const MmcMemBlobDesc &blob)
{
    if (metaNetServer_.Get() == nullptr) {
        MMC_LOG_WARN("metaNetServer_ is null, skip BlobDelete RPC for key: " << key);
        return MMC_ERROR;
    }
    BlobDeleteRequest req(key, blob.rank_, blob);
    BlobDeleteResponse resp;
    Result ret = metaNetServer_->SyncCall(blob.rank_, req, resp, TIMEOUT_SECOND);
    if (ret != MMC_OK || resp.ret_ != MMC_OK) {
        MMC_LOG_ERROR("failed to delete blob by RPC for key: " << key << ", rank: " << blob.rank_ << ", ret: " << ret
                                                               << ", resp: " << resp.ret_);
        return MMC_ERROR;
    }
    MMC_LOG_DEBUG("Deleted blob via RPC successfully, key=" << key << ", rank=" << blob.rank_);
    return MMC_OK;
}

Result MmcMetaManager::RemoveSsdBlob(const std::string &key, uint32_t rank)
{
    MmcMemObjMetaPtr objMeta;
    if (metaContainer_->Get(key, objMeta) != MMC_OK || objMeta == nullptr) {
        MMC_LOG_DEBUG("RemoveSsdBlob: key=" << key << " not found");
        return MMC_UNMATCHED_KEY;
    }
    std::unique_lock<std::mutex> guard(objMeta->Mutex());
    MmcBlobFilterPtr ssdFilter = MmcMakeRef<MmcBlobFilter>(rank, MEDIA_SSD, NONE);
    if (ssdFilter == nullptr) {
        MMC_LOG_ERROR("RemoveSsdBlob: create filter failed for key=" << key);
        return MMC_MALLOC_FAILED;
    }
    auto blobs = objMeta->DetachBlobs(ssdFilter);
    guard.unlock();
    objMeta->FinalizeBlobs(key, globalAllocator_, blobs, false);
    for (const auto &blob : blobs) {
        KvEventRemoved(key, blob->Rank(), blob->Type());
    }
    guard.lock();
    MMC_LOG_DEBUG("RemoveSsdBlob: key=" << key << ", rank=" << rank);
    if (objMeta->NumBlobs() == 0) {
        guard.unlock();
        metaContainer_->Erase(key);
        MMC_LOG_DEBUG("RemoveSsdBlob: key=" << key << " fully removed (no remaining blobs)");
    }
    return MMC_OK;
}

Result MmcMetaManager::AcquireBackupReadLease(const std::string &key, const MmcMemBlobDesc &source,
                                              BackupReadLease &lease)
{
    if (source.mediaType_ != MEDIA_DRAM) {
        return MMC_INVALID_PARAM;
    }
    MmcMemObjMetaPtr objMeta;
    if (metaContainer_->Get(key, objMeta) != MMC_OK || objMeta == nullptr) {
        return MMC_UNMATCHED_KEY;
    }
    std::lock_guard<std::mutex> guard(objMeta->Mutex());
    for (const auto &blob : objMeta->GetBlobs()) {
        if (blob->GetDesc() != source || blob->State() != READABLE) {
            continue;
        }
        // Use an internal rank and the existing sequence generator to avoid sharing client lease IDs.
        const uint32_t sequence = GetSequenceByOperateId(GenerateOperateId(K_BACKUP_LEASE_RANK_ID));
        BackupReadLease acquiredLease{key, objMeta, sequence};
        Result ret = blob->UpdateState(key, K_BACKUP_LEASE_RANK_ID, sequence, MMC_READ_START);
        if (ret == MMC_OK) {
            lease = std::move(acquiredLease);
        }
        return ret;
    }
    return MMC_UNMATCHED_STATE;
}

void MmcMetaManager::ReleaseBackupReadLease(const BackupReadLease &lease)
{
    if (lease.object == nullptr) {
        return;
    }
    // Release through the original object even if the key was removed or recreated.
    std::lock_guard<std::mutex> guard(lease.object->Mutex());
    for (const auto &blob : lease.object->GetBlobs()) {
        // Only release this operation's lease from a readable blob still owned by the object.
        if (blob->Type() != MEDIA_DRAM || blob->State() != READABLE ||
            !blob->HasLease(K_BACKUP_LEASE_RANK_ID, lease.sequence)) {
            continue;
        }
        Result ret = blob->UpdateState(lease.key, K_BACKUP_LEASE_RANK_ID, lease.sequence, MMC_READ_FINISH);
        if (ret != MMC_OK) {
            MMC_LOG_WARN("Release backup read lease failed, key=" << lease.key << ", ret=" << ret);
        }
        return;
    }
}

Result MmcMetaManager::AddSsdBlob(const std::string &key, const MmcMemBlobDesc &desc)
{
    MmcMemObjMetaPtr objMeta;
    if (metaContainer_->Get(key, objMeta) != MMC_OK || objMeta == nullptr) {
        MMC_LOG_DEBUG("Key not found for adding SSD blob, key=" << key);
        return MMC_UNMATCHED_KEY;
    }
    std::unique_lock<std::mutex> guard(objMeta->Mutex());
    // SSD 副本不走 allocator 释放，无切块语义，allocSize 显式置 0
    auto ssdBlob = MmcMakeRef<MmcMemBlob>(desc.rank_, 0, desc.size_, MEDIA_SSD, READABLE, 0);
    if (ssdBlob == nullptr) {
        MMC_LOG_ERROR("Failed to create SSD blob, key=" << key);
        return MMC_MALLOC_FAILED;
    }
    Result ret = objMeta->AddBlob(ssdBlob);
    if (ret != MMC_OK) {
        MMC_LOG_ERROR("Failed to add SSD blob to meta object, key=" << key << ", ret=" << ret);
        return ret;
    }
    guard.unlock();
    KvEventStored(key, desc.rank_, ssdBlob->Type());
    MMC_LOG_DEBUG("Added SSD blob, key=" << key << ", rank=" << desc.rank_ << ", size=" << desc.size_);
    return MMC_OK;
}

Result MmcMetaManager::Remove(const std::string &key)
{
    MmcMemObjMetaPtr objMeta;
    auto ret = metaContainer_->Erase(key, objMeta);
    if (ret != MMC_OK || objMeta == nullptr) {
        MMC_LOG_DEBUG("Erase returned null objMeta for key: " << key);
        return ret;
    }
    std::unique_lock<std::mutex> guard(objMeta->Mutex());
    PushRemoveList(key, objMeta, nullptr, true);
    return MMC_OK;
}

Result MmcMetaManager::RemoveAll()
{
    auto removeFunc = [this](const std::string &key, const MmcMemObjMetaPtr &objMeta) -> void {
        if (objMeta == nullptr) {
            MMC_LOG_ERROR("objMeta is null in RemoveAll for key: " << key);
            return;
        }
        std::unique_lock<std::mutex> guard(objMeta->Mutex());
        this->PushRemoveList(key, objMeta, nullptr, true);
    };

    MMC_RETURN_ERROR(metaContainer_->EraseAll(removeFunc), "RemoveAll: Fail to erase all from container!");

    MMC_LOG_INFO("Removed all keys");
    return MMC_OK;
}

Result MmcMetaManager::Mount(const MmcLocation &loc, const MmcLocalMemlInitInfo &localMemInitInfo,
                             std::vector<std::pair<std::string, MmcMemBlobDesc>> &blobList, bool enableMaxValueMode)
{
    Result ret = globalAllocator_->Mount(loc, localMemInitInfo, enableMaxValueMode);
    if (ret != MMC_OK) {
        MMC_LOG_ERROR("allocator mount failed, loc rank: " << loc.rank_ << " mediaType_: " << loc.mediaType_);
        return ret;
    }
    if (blobList.empty()) {
        return globalAllocator_->Start(loc);
    }
    ret = globalAllocator_->BuildFromBlobs(loc, blobList);
    if (ret != MMC_OK) {
        MMC_LOG_ERROR("build from blobs failed, loc rank: " << loc.rank_ << " mediaType_: " << loc.mediaType_);
        return ret;
    }

    if (!blobList.empty()) {
        ret = RebuildMeta(blobList);
        if (ret != MMC_OK) {
            MMC_LOG_ERROR("rebuild meta failed, loc rank: " << loc.rank_ << " mediaType_: " << loc.mediaType_);
            return ret;
        }
    }
    return MMC_OK;
}

Result MmcMetaManager::Mount(const std::vector<MmcLocation> &locs,
                             const std::vector<MmcLocalMemlInitInfo> &localMemInitInfos,
                             std::vector<std::pair<std::string, MmcMemBlobDesc>> &blobList, bool storageEnabled,
                             bool enableMaxValueMode)
{
    if (locs.size() != localMemInitInfos.size()) {
        MMC_LOG_ERROR("Mount: loc size:" << locs.size() << " != localMemInitInfo size:" << localMemInitInfos.size());
        return MMC_INVALID_PARAM;
    }
    if (storageEnabled && IsSsdMetadataSeparated()) {
        // separated 模式只重建内存层元数据，SSD 文件及其生命周期由 UBSIO 管理。
        blobList.erase(std::remove_if(blobList.begin(), blobList.end(),
                                      [](const auto &entry) {
                                          return static_cast<MediaType>(entry.second.mediaType_) == MEDIA_SSD;
                                      }),
                       blobList.end());
    }
    Result ret = MMC_OK;
    uint32_t i = 0;
    for (; i < locs.size(); i++) {
        ret = Mount(locs[i], localMemInitInfos[i], blobList, enableMaxValueMode);
        if (ret != MMC_OK) {
            MMC_LOG_ERROR("Mount failed ret:" << ret << " loc rank:" << locs[i].rank_
                                              << " mediaType_: " << locs[i].mediaType_);
            break;
        }
    }
    if (ret != MMC_OK) {
        MMC_LOG_WARN("Unable to mount locs partially, unmounting mounted locs...");
        for (; i > 0; i--) {
            auto unmountRet = Unmount(locs[i - 1]);
            if (unmountRet != MMC_OK) {
                MMC_LOG_ERROR("Unmount failed ret:" << unmountRet << " loc rank:" << locs[i - 1].rank_
                                                    << " mediaType_: " << locs[i - 1].mediaType_);
            }
        }
    } else if (storageEnabled && !locs.empty()) {
        std::lock_guard<std::mutex> guard(ssdMutex_);
        ssdEnabledRanks_.insert(locs.front().rank_);
        MMC_LOG_TRACE("Storage enabled for rank=" << locs.front().rank_);
    }
    return MMC_OK;
}

Result MmcMetaManager::RebuildMeta(std::vector<std::pair<std::string, MmcMemBlobDesc>> &blobList)
{
    Result ret;
    for (auto &blob : blobList) {
        std::string key = blob.first;
        MmcMemBlobDesc desc = blob.second;
        BlobState state = (desc.state_ == NONE ? READABLE : desc.state_);
        MmcMemBlobPtr blobPtr =
            MmcMakeRef<MmcMemBlob>(desc.rank_, desc.gva_, desc.size_, static_cast<MediaType>(desc.mediaType_), state,
                                   desc.allocSize_, defaultTtlMs_.load(std::memory_order_relaxed));
        if (blobPtr == nullptr) {
            MMC_LOG_ERROR("Failed to create blob while rebuilding meta, key: " << key);
            return MMC_MALLOC_FAILED;
        }
        MmcMemObjMetaPtr objMeta;

        if (metaContainer_->Get(key, objMeta) == MMC_OK) {
            std::unique_lock<std::mutex> guard(objMeta->Mutex());
            if (objMeta->AddBlob(blobPtr) != MMC_OK) {
                globalAllocator_->Free(blobPtr);
            }
            continue;
        }

        objMeta = MmcMakeRef<MmcMemObjMeta>();
        if (objMeta != nullptr) {
            ret = objMeta->AddBlob(blobPtr);
            if (ret != MMC_OK) {
                globalAllocator_->Free(blobPtr);
                MMC_LOG_ERROR("Failed to add blob while rebuilding meta, key: " << key << ", ret: " << ret);
                return ret;
            }
            ret = metaContainer_->Insert(key, objMeta);
            if (ret == MMC_OK) {
                continue;
            }
        }

        if (metaContainer_->Get(key, objMeta) == MMC_OK) {
            std::unique_lock<std::mutex> guard(objMeta->Mutex());
            if (objMeta->AddBlob(blobPtr) != MMC_OK) {
                globalAllocator_->Free(blobPtr);
            }
        } else {
            globalAllocator_->Free(blobPtr);
        }
    }
    return MMC_OK;
}

Result MmcMetaManager::RemoveBlobs(uint32_t rank, MediaType mediaType)
{
    MmcBlobFilterPtr filter = MmcMakeRef<MmcBlobFilter>(rank, mediaType, NONE);
    if (filter == nullptr) {
        MMC_LOG_ERROR("Failed to create blob filter for rank=" << rank << ", media=" << static_cast<int>(mediaType));
        return MMC_MALLOC_FAILED;
    }

    uint64_t freedBlobs = 0;
    uint64_t erasedKeys = 0;
    auto matchFunc = [this, &filter, &freedBlobs, &erasedKeys](const std::string &key,
                                                               const MmcMemObjMetaPtr &objMeta) -> bool {
        if (objMeta == nullptr) {
            MMC_LOG_ERROR("objMeta is null for key:" << key);
            return false;
        }
        std::unique_lock<std::mutex> guard(objMeta->Mutex());
        auto blobs = objMeta->DetachBlobs(filter);
        guard.unlock();
        objMeta->FinalizeBlobs(key, globalAllocator_, blobs, false);
        guard.lock();
        freedBlobs += blobs.size();
        const bool shouldErase = (objMeta->NumBlobs() == 0);
        if (shouldErase) {
            ++erasedKeys;
        }
        return shouldErase;
    };

    metaContainer_->EraseIf(matchFunc);
    KvEventCleared(rank, mediaType);
    MMC_LOG_TRACE("Remove blobs for rank=" << rank << ", media=" << static_cast<int>(mediaType)
                                           << " done, freedBlobs=" << freedBlobs << ", erasedKeys=" << erasedKeys);
    return MMC_OK;
}

Result MmcMetaManager::Unmount(const MmcLocation &loc)
{
    Result ret = globalAllocator_->Stop(loc);
    if (ret != MMC_OK) {
        return ret;
    }
    ret = RemoveBlobs(loc.rank_, loc.mediaType_);
    const Result removeBlobsRet = ret;
    if (removeBlobsRet != MMC_OK) {
        MMC_LOG_ERROR("Remove blobs failed for loc " << loc << ", ret=" << removeBlobsRet);
    }
    ret = globalAllocator_->Unmount(loc);
    if (ret != MMC_OK) {
        MMC_LOG_ERROR("global allocator unmount failed for loc " << loc << ", ret=" << ret);
        return ret;
    }
    if (removeBlobsRet != MMC_OK) {
        return removeBlobsRet;
    }
    std::lock_guard<std::mutex> guard(ssdMutex_);
    ssdEnabledRanks_.erase(loc.rank_);
    return MMC_OK;
}

Result MmcMetaManager::GetRandomStorageRank(uint32_t preferredRank, uint32_t &storageRank) const noexcept
{
    if (!IsSsdMetadataSeparated()) {
        return MMC_INVALID_PARAM;
    }
    std::vector<uint32_t> candidates;
    {
        std::lock_guard<std::mutex> guard(ssdMutex_);
        if (ssdEnabledRanks_.empty()) {
            return MMC_SSD_NOT_AVAILABLE;
        }
        const auto preferred = ssdEnabledRanks_.find(preferredRank);
        if (preferred != ssdEnabledRanks_.end()) {
            storageRank = *preferred;
            return MMC_OK;
        }
        candidates.assign(ssdEnabledRanks_.begin(), ssdEnabledRanks_.end());
    }

    std::uniform_int_distribution<size_t> distribution(0, candidates.size() - 1U);
    storageRank = candidates[distribution(g_storageRankRng)];
    return MMC_OK;
}

Result MmcMetaManager::BatchStorageStatRpc(const std::vector<std::string> &keys, std::vector<Result> &results,
                                           std::vector<uint64_t> &sizes, uint32_t requestRank)
{
    results.assign(keys.size(), MMC_UNMATCHED_KEY);
    sizes.assign(keys.size(), 0);
    if (keys.empty()) {
        return MMC_OK;
    }
    if (metaNetServer_ == nullptr) {
        return MMC_NOT_INITIALIZED;
    }

    uint32_t statRank = UINT32_MAX;
    Result ret = GetRandomStorageRank(requestRank, statRank);
    if (ret != MMC_OK) {
        MMC_LOG_WARN("No storage-enabled rank available for batch stat, request rank=" << requestRank << ", key count="
                                                                                       << keys.size());
        return ret;
    }

    BatchStorageStatRequest request(keys);
    BatchStorageStatResponse response;
    TP_TRACE_BEGIN(TP_MMC_META_BATCH_STORAGE_STAT);
    ret = metaNetServer_->SyncCall(statRank, request, response, BATCH_STORAGE_STAT_RPC_TIMEOUT_SECOND);
    TP_TRACE_END(TP_MMC_META_BATCH_STORAGE_STAT, ret);
    if (ret != MMC_OK) {
        MMC_LOG_WARN("Batch storage stat RPC failed, rank=" << statRank << ", key count=" << keys.size()
                                                            << ", ret=" << ret);
        return ret;
    }
    if (response.results_.size() != keys.size() || response.sizes_.size() != keys.size()) {
        MMC_LOG_WARN("Batch storage stat response size mismatch, rank=" << statRank << ", expected=" << keys.size()
                                                                        << ", results=" << response.results_.size()
                                                                        << ", sizes=" << response.sizes_.size());
        return MMC_ERROR;
    }

    results = std::move(response.results_);
    sizes = std::move(response.sizes_);
    return MMC_OK;
}

Result MmcMetaManager::BatchLookupStorage(const std::vector<std::string> &keys, std::vector<Result> &results,
                                          std::vector<uint64_t> &sizes, uint32_t requestRank)
{
    Result ret = BatchStorageStatRpc(keys, results, sizes, requestRank);
    if (ret != MMC_OK) {
        return ret;
    }
    if (!extConfig_.prefetchEnabled) {
        return MMC_OK;
    }

    std::vector<std::string> prefetchKeys;
    std::vector<uint64_t> prefetchSizes;
    prefetchKeys.reserve(keys.size());
    prefetchSizes.reserve(keys.size());
    for (size_t index = 0; index < keys.size(); ++index) {
        if (results[index] == MMC_OK) {
            prefetchKeys.push_back(keys[index]);
            prefetchSizes.push_back(sizes[index]);
        }
    }
    AsyncPrefetchKeys(prefetchKeys, prefetchSizes);
    return MMC_OK;
}

Result MmcMetaManager::CleanSsdBlobs(uint32_t rank)
{
    return RemoveBlobs(rank, MEDIA_SSD);
}

nlohmann::json MmcMetaManager::GetAllSegmentInfo() const
{
    return globalAllocator_->GetAllSegmentInfo();
}

Result MmcMetaManager::PrefetchKeys(const std::vector<std::string> &keys, MediaType srcMedia, MediaType dstMedia,
                                    uint32_t flags)
{
    (void)flags;
    MMC_LOG_DEBUG("PrefetchKeys called, keys size: " << keys.size());
    if (keys.empty()) {
        MMC_LOG_INFO("PrefetchKeys: keys is empty, silently passing the function");
        return MMC_OK;
    }
    if (srcMedia != MEDIA_SSD || dstMedia != MEDIA_DRAM) {
        MMC_LOG_ERROR("PrefetchKeys: only SSD->DRAM is supported currently, srcMedia: " << srcMedia
                                                                                        << ", dstMedia: " << dstMedia);
        return MMC_INVALID_PARAM;
    }
    if (!IsAnySsdEnabled()) {
        MMC_LOG_DEBUG("PrefetchKeys: no SSD enabled, skipping prefetch");
        return MMC_OK;
    }
    std::vector<MmcMemMetaDesc> objMetas;
    Result ret = GetByRank(keys, GenerateOperateId(UINT32_MAX), objMetas, {}, false);
    if (ret != MMC_OK) {
        MMC_LOG_WARN("Batch prefetch rewarm failed, key count=" << keys.size() << ", ret=" << ret);
        return ret;
    }
    return MMC_OK;
}

Result MmcMetaManager::AsyncPrefetchKeys(const std::vector<std::string> &keys, const std::vector<uint64_t> &sizes)
{
    if (keys.empty()) {
        MMC_LOG_INFO("AsyncPrefetchKeys: keys is empty, silently passing the function");
        return MMC_OK;
    }
    if (!IsAnySsdEnabled()) {
        MMC_LOG_DEBUG("PrefetchKeysAsync: no SSD enabled, skipping prefetch");
        return MMC_OK;
    }
    if (!sizes.empty() && keys.size() != sizes.size()) {
        MMC_LOG_WARN("Storage prefetch input size mismatch, keys=" << keys.size() << ", sizes=" << sizes.size());
        return MMC_INVALID_PARAM;
    }
    auto future = threadPool_->Enqueue([this, keys, sizes]() {
        auto ownedKeys = std::make_shared<std::vector<std::string>>(keys);
        auto ownedMetas = std::make_shared<std::vector<MmcMemMetaDesc>>(keys.size());
        CheckAndEvict(MEDIA_DRAM, 0);
        auto context =
            std::make_shared<GetByRankContext>(*ownedKeys, GenerateOperateId(UINT32_MAX), *ownedMetas, sizes);
        ClassifyAndGroupMetaKeys(*context);
        Result ret = MMC_OK;
        if (IsSsdMetadataSeparated()) {
            ret = ClassifyStorageMisses(*context);
        }
        if (ret != MMC_OK) {
            MMC_LOG_WARN("Async prefetch classification failed, key count=" << keys.size() << ", ret=" << ret);
        }
        for (auto &[rank, group] : context->rankGroups) {
            RewarmGroupContext rewarmContext{context->opRankId, context->opSeq, context->keys, context->objs,
                                             context->objMetas};
            rewarmThreadPool_->Enqueue([this, rank, &group, context, rewarmContext, ownedKeys, ownedMetas]() mutable {
                RewarmRankGroup(rank, group, rewarmContext, false);
            });
        }
    });
    if (!future.valid()) {
        MMC_LOG_WARN("Failed to submit storage prefetch, key count=" << keys.size());
        return MMC_ERROR;
    }
    return MMC_OK;
}

Result MmcMetaManager::EvictKeys(std::vector<std::string> keys, MediaType srcMedia, MediaType dstMedia, uint32_t flags)
{
    (void)flags;
    MMC_LOG_DEBUG("EvictKeys called, keys size: " << keys.size());
    if (keys.empty()) {
        MMC_LOG_INFO("EvictKeys called with empty keys");
        return MMC_INVALID_PARAM;
    }
    if (srcMedia != MEDIA_DRAM || dstMedia != MEDIA_SSD) {
        MMC_LOG_ERROR("EvictKeys: only DRAM->SSD is supported currently, srcMedia: " << srcMedia
                                                                                     << ", dstMedia: " << dstMedia);
        return MMC_INVALID_PARAM;
    }
    for (const auto &key : keys) {
        MmcMemObjMetaPtr objMeta;
        auto ret = metaContainer_->Get(key, objMeta);
        if (ret != MMC_OK || objMeta == nullptr) {
            MMC_LOG_INFO("key not found: " << key << ", EvictKeys() skip");
            continue;
        }

        // 先从 DRAM LRU 摘除，防止被自动淘汰并发处理
        metaContainer_->RemoveFromLru(key, MEDIA_DRAM);

        EvictCallBackFunction(key, objMeta, MEDIA_DRAM);
    }
    return MMC_OK;
}

Result MmcMetaManager::Query(const std::string &key, uint64_t operateId, uint32_t flags, MemObjQueryInfo &queryInfo)
{
    (void)operateId;
    MmcMemObjMetaPtr objMeta;
    if (metaContainer_->Get(key, objMeta) != MMC_OK || objMeta == nullptr) {
        MMC_LOG_DEBUG("Cannot find MmcMemObjMeta with key : " << key);
        return MMC_UNMATCHED_KEY;
    }

    TP_TRACE_BEGIN(TP_MMC_META_QUERY_PROMOTE_LRU);
    auto ret = metaContainer_->Promote(key);
    TP_TRACE_END(TP_MMC_META_QUERY_PROMOTE_LRU, ret);
    if (ret != MMC_OK) {
        MMC_LOG_ERROR("Query key: " << key << " Promote failed. ErrCode: " << ret);
        return ret;
    }

    std::unique_lock<std::mutex> guard(objMeta->Mutex());
    std::vector<MmcMemBlobDesc> blobs;
    objMeta->GetBlobsDesc(blobs);
    queryInfo.blobs_.clear();
    const size_t reservedBlobCount =
        blobs.size() < static_cast<size_t>(MAX_BLOB_COPIES) ? blobs.size() : static_cast<size_t>(MAX_BLOB_COPIES);
    queryInfo.blobs_.reserve(reservedBlobCount);
    for (const auto &blob : blobs) {
        if (queryInfo.blobs_.size() >= MAX_BLOB_COPIES) {
            MMC_LOG_WARN("Query blob count reached MAX_BLOB_COPIES cap: " << MAX_BLOB_COPIES << ", truncating");
            break;
        }
        if ((flags & GET_KEY_INFO_FOR_LAYER_WISE) != 0) {
            if (blob.mediaType_ == MEDIA_NONE || blob.mediaType_ == MEDIA_SSD || blob.state_ != READABLE) {
                MMC_LOG_DEBUG("Skip blob, key : " << key << ", blob type : " << blob.mediaType_
                                                  << ", state : " << blob.state_);
                continue;
            }
        }
        queryInfo.blobs_.push_back(blob);
    }
    queryInfo.numBlobs_ = static_cast<uint8_t>(queryInfo.blobs_.size());
    queryInfo.size_ = queryInfo.blobs_.size() == 0 ? 0 : objMeta->Size();
    queryInfo.prot_ = objMeta->Prot();
    queryInfo.valid_ = !queryInfo.blobs_.empty();
    return MMC_OK;
}

Result MmcMetaManager::AddLease(const std::string &key, uint64_t operateId, uint64_t leaseTtlMs,
                                MemObjQueryInfo &queryInfo)
{
    const uint64_t actualLeaseTtlMs = leaseTtlMs == 0 ? defaultTtlMs_.load(std::memory_order_relaxed) : leaseTtlMs;
    MmcMemObjMetaPtr objMeta;
    if (metaContainer_->Get(key, objMeta) != MMC_OK || objMeta == nullptr) {
        MMC_LOG_DEBUG("Cannot find MmcMemObjMeta with key : " << key);
        return MMC_UNMATCHED_KEY;
    }

    TP_TRACE_BEGIN(TP_MMC_META_ADD_LEASE_PROMOTE_LRU);
    auto ret = metaContainer_->Promote(key);
    TP_TRACE_END(TP_MMC_META_ADD_LEASE_PROMOTE_LRU, ret);
    if (ret != MMC_OK) {
        MMC_LOG_ERROR("AddLease key: " << key << " Promote failed. ErrCode: " << ret);
        return ret;
    }

    std::unique_lock<std::mutex> guard(objMeta->Mutex());
    std::vector<MmcMemBlobPtr> blobs = objMeta->GetBlobs();
    MmcMemBlobPtr selectedBlob = nullptr;
    ret = SelectReadableGvaBlob(blobs, selectedBlob);
    if (ret != MMC_OK) {
        MMC_LOG_ERROR("AddLease requires one readable GVA blob, optionally with one SSD blob, key:"
                      << key << ", blobNum:" << blobs.size());
        return ret;
    }

    uint32_t opRankId = GetRankIdByOperateId(operateId);
    uint32_t opSeq = GetSequenceByOperateId(operateId);
    ret = selectedBlob->ExtendLease(opRankId, opSeq, actualLeaseTtlMs);
    if (ret != MMC_OK) {
        MMC_LOG_ERROR("AddLease failed for key:" << key << ", ret:" << ret << ", leaseTtlMs:" << actualLeaseTtlMs);
        return ret;
    }

    FillSingleBlobQueryInfo(objMeta, selectedBlob, queryInfo);
    return MMC_OK;
}

Result MmcMetaManager::RemoveLease(const std::string &key, uint64_t operateId)
{
    MmcMemObjMetaPtr objMeta;
    if (metaContainer_->Get(key, objMeta) != MMC_OK || objMeta == nullptr) {
        MMC_LOG_DEBUG("Cannot find MmcMemObjMeta with key : " << key);
        return MMC_UNMATCHED_KEY;
    }

    std::unique_lock<std::mutex> guard(objMeta->Mutex());
    std::vector<MmcMemBlobPtr> blobs = objMeta->GetBlobs();
    MmcMemBlobPtr selectedBlob = nullptr;
    Result ret = SelectReadableGvaBlob(blobs, selectedBlob);
    if (ret != MMC_OK) {
        MMC_LOG_ERROR("RemoveLease requires one readable GVA blob, optionally with one SSD blob, key:"
                      << key << ", blobNum:" << blobs.size());
        return ret;
    }

    uint32_t opRankId = GetRankIdByOperateId(operateId);
    uint32_t opSeq = GetSequenceByOperateId(operateId);
    ret = selectedBlob->UpdateState(key, opRankId, opSeq, MMC_READ_FINISH);
    if (ret != MMC_OK) {
        MMC_LOG_ERROR("RemoveLease failed for key:" << key << ", ret:" << ret);
        return ret;
    }
    return MMC_OK;
}

Result MmcMetaManager::GetAllKeys(std::vector<std::string> &keys)
{
    MMC_VALIDATE_RETURN(metaContainer_ != nullptr, "meta container not initialized! ", MMC_NOT_INITIALIZED);

    metaContainer_->GetAllKeys(keys);
    return MMC_OK;
}

Result MmcMetaManager::GetKeyCount(size_t &count)
{
    MMC_VALIDATE_RETURN(metaContainer_ != nullptr, "meta container not initialized! ", MMC_NOT_INITIALIZED);

    count = metaContainer_->GetKeyCount();
    return MMC_OK;
}

Result MmcMetaManager::CopyBlob(const std::string &key, const MmcMemObjMetaPtr &objMeta, const MmcMemBlobDesc &srcBlob,
                                const MmcLocation &dstLoc)
{
    if (objMeta == nullptr) {
        MMC_LOG_ERROR("objMeta is null");
        return MMC_INVALID_PARAM;
    }

    std::vector<MmcMemBlobPtr> blobs;
    AllocOptions allocOpt{};
    allocOpt.blobSize_ = srcBlob.size_;
    allocOpt.numBlobs_ = 1;
    allocOpt.mediaType_ = dstLoc.mediaType_;
    allocOpt.preferredRank_.clear();
    allocOpt.preferredRank_.push_back(dstLoc.rank_);
    allocOpt.flags_ = dstLoc.rank_ == UINT32_MAX ? 0 : ALLOC_FORCE_BY_RANK;

    TP_TRACE_BEGIN(TP_MMC_META_MOVEBLOB_ALLOC);
    Result ret = globalAllocator_->Alloc(allocOpt, blobs);
    TP_TRACE_END(TP_MMC_META_MOVEBLOB_ALLOC, ret);
    if (ret != MMC_OK || blobs.empty()) {
        MMC_LOG_WARN("alloc failed, ret=" << ret << ", key=" << key);
        return MMC_MALLOC_FAILED;
    }
    MmcMemBlobPtr blob = blobs[0];
    MmcMemBlobDesc blobDesc = blob->GetDesc();

    if (metaNetServer_.Get() == nullptr) {
        MMC_LOG_ERROR("metaNetServer_ is null, key=" << key);
        globalAllocator_->Free(blob);
        return MMC_ERROR;
    }

    BlobCopyRequest request{key, srcBlob, blobDesc};
    Response response;
    TP_TRACE_BEGIN(TP_MMC_META_MOVEBLOB_RPC);
    ret = metaNetServer_->SyncCall(request.dstBlob_.rank_, request, response, TIMEOUT_SECOND);
    TP_TRACE_END(TP_MMC_META_MOVEBLOB_RPC, ret);

    if (ret != MMC_OK || response.ret_ != MMC_OK) {
        MMC_LOG_ERROR("RPC failed, key=" << key << ", srcRank=" << request.srcBlob_.rank_ << ", dstRank="
                                         << request.dstBlob_.rank_ << ", ret=" << ret << ", resp=" << response.ret_);
        globalAllocator_->Free(blob);
        return MMC_ERROR;
    }

    ret = blob->UpdateState(key, dstLoc.rank_, 0, MMC_WRITE_OK);
    if (ret != MMC_OK) {
        MMC_LOG_ERROR("UpdateState WRITE_OK failed, key=" << key << ", ret=" << ret);
        globalAllocator_->Free(blob);
        return MMC_ERROR;
    }

    ret = objMeta->AddBlob(blob);
    if (ret != MMC_OK) {
        MMC_LOG_ERROR("AddBlob failed, key=" << key << ", ret=" << ret);
        globalAllocator_->Free(blob);
        return MMC_ERROR;
    }

    KvEventStored(key, blobDesc.rank_, blobDesc.mediaType_);
    MMC_LOG_DEBUG("copy ok, key=" << key << ", dstRank=" << blobDesc.rank_ << ", size=" << blobDesc.size_);
    return MMC_OK;
}

namespace {

Result GetMoveBlobSrcDesc(const std::string &key, const MmcMemObjMetaPtr &objMeta, const MmcLocation &src,
                          MmcMemBlobDesc &outDesc)
{
    MmcBlobFilterPtr filter = MmcMakeRef<MmcBlobFilter>(src.rank_, src.mediaType_, READABLE);
    if (filter == nullptr) {
        MMC_LOG_ERROR("Fail to malloc filter");
        return MMC_MALLOC_FAILED;
    }
    std::vector<MmcMemBlobDesc> blobsDesc;
    objMeta->GetBlobsDesc(blobsDesc, filter);
    if (blobsDesc.empty()) {
        MMC_LOG_ERROR("blob for " << src << " is empty with key : " << key << "," << objMeta);
        return MMC_UNMATCHED_KEY;
    }
    outDesc = blobsDesc[0];
    return MMC_OK;
}

} // namespace

Result MmcMetaManager::MoveBlob(const std::string &key, const MmcLocation &src, const MmcLocation &dst)
{
    MmcMemObjMetaPtr objMeta;
    if (metaContainer_->Get(key, objMeta) != MMC_OK) {
        MMC_LOG_ERROR("Cannot find MmcMemObjMeta with key : " << key);
        return MMC_UNMATCHED_KEY;
    }

    TP_TRACE_BEGIN(TP_MMC_META_MOVEBLOB);
    {
        std::unique_lock<std::mutex> guard(objMeta->Mutex());
        MmcMemBlobDesc srcDesc;
        Result ret = GetMoveBlobSrcDesc(key, objMeta, src, srcDesc);
        if (ret != MMC_OK) {
            TP_TRACE_END(TP_MMC_META_MOVEBLOB, ret);
            return ret;
        }

        TP_TRACE_BEGIN(TP_MMC_META_MOVEBLOB_COPY);
        ret = CopyBlob(key, objMeta, srcDesc, dst);
        TP_TRACE_END(TP_MMC_META_MOVEBLOB_COPY, ret);

        if (ret != MMC_OK) {
            MMC_LOG_WARN("key: " << key << " copy blob failed, ret " << ret);

            TP_TRACE_END(TP_MMC_META_MOVEBLOB, ret);
            return ret;
        }

        MmcBlobFilterPtr filter = MmcMakeRef<MmcBlobFilter>(src.rank_, src.mediaType_, NONE);
        if (filter == nullptr) {
            MMC_LOG_ERROR("Fail to malloc filter");
            TP_TRACE_END(TP_MMC_META_MOVEBLOB, MMC_MALLOC_FAILED);
            return MMC_MALLOC_FAILED;
        }

        auto blobs = objMeta->DetachBlobs(filter);
        guard.unlock();
        objMeta->FinalizeBlobs(key, globalAllocator_, blobs);
        guard.lock();
        MMC_LOG_TRACE("move " << key << " from " << src << " to " << dst << " " << srcDesc << ", " << objMeta);
        guard.unlock();
        for (const auto &blob : blobs) {
            if (blob != nullptr) {
                KvEventRemoved(key, blob->Rank(), blob->Type());
            }
        }
    }
    metaContainer_->InsertLru(key, dst.mediaType_);
    TP_TRACE_END(TP_MMC_META_MOVEBLOB, MMC_OK);
    return MMC_OK;
}

Result MmcMetaManager::ReplicateBlob(const std::string &key, const MmcLocation &loc)
{
    MmcMemObjMetaPtr objMeta;
    if (metaContainer_->Get(key, objMeta) != MMC_OK || objMeta.Get() == nullptr) {
        MMC_LOG_ERROR("Cannot find MmcMemObjMeta with key : " << key);
        return MMC_UNMATCHED_KEY;
    }
    std::unique_lock<std::mutex> guard(objMeta->Mutex());
    std::vector<MmcMemBlobDesc> blobsDesc;
    MmcBlobFilterPtr filter = MmcMakeRef<MmcBlobFilter>(UINT32_MAX, MEDIA_NONE, READABLE);
    objMeta->GetBlobsDesc(blobsDesc, filter);
    if (blobsDesc.empty()) {
        MMC_LOG_ERROR("blob is empty with key : " << key);
        return MMC_UNMATCHED_KEY;
    }

    return CopyBlob(key, objMeta, blobsDesc[0], loc);
}

namespace {

bool HasRemainingBlobsAfterEvict(const MmcMemObjMetaPtr &objMeta, const MmcBlobFilterPtr &srcFilter, MediaType dstMedia)
{
    // filter 为 nullptr 时 MatchFilter 恒命中，即全部 blob 都会被释放，对应无剩余（REMOVE）
    auto blobs = objMeta->GetBlobs();

    for (auto &blob : blobs) {
        if (blob == nullptr) {
            continue;
        }
        // 与异步 FreeBlobs 使用同一过滤语义：命中的 blob 才会被释放，其余视为保留
        if (blob->MatchFilter(srcFilter)) {
            continue;
        }

        MMC_LOG_DEBUG("HasRemainingBlobsAfterEvict remaining blob media="
                      << static_cast<int>(blob->Type()) << " rank=" << blob->Rank() << " srcMedia="
                      << static_cast<int>(srcFilter->mediaType_) << " dstMedia=" << static_cast<int>(dstMedia));
        return true;
    }
    return false;
}

} // namespace

EvictResult MmcMetaManager::EvictRemoveSrc(const std::string &key, const MmcMemObjMetaPtr &objMeta,
                                           const MmcBlobFilterPtr &srcFilter, MediaType dstMedium)
{
    PushRemoveList(key, objMeta, srcFilter, false);
    return HasRemainingBlobsAfterEvict(objMeta, srcFilter, dstMedium) ? EvictResult::MOVE_DOWN : EvictResult::REMOVE;
}

EvictResult MmcMetaManager::EvictRemoveSrc(const std::string &key, const MmcMemObjMetaPtr &objMeta,
                                           MediaType srcMediaType, MediaType dstMedium)
{
    MmcBlobFilterPtr srcFilter = MmcMakeRef<MmcBlobFilter>(UINT32_MAX, srcMediaType, NONE);
    if (srcFilter == nullptr) {
        MMC_LOG_ERROR("Evict skip key=" << key << " from " << srcMediaType << ", create filter failed");
        return EvictResult::FAIL;
    }
    return EvictRemoveSrc(key, objMeta, srcFilter, dstMedium);
}

EvictResult MmcMetaManager::DispatchMoveBlob(const std::string &key, const MmcMemObjMetaPtr &objMeta,
                                             const MmcBlobFilterPtr &srcFilter, const MmcLocation &src,
                                             const MmcLocation &dst, MediaType srcMediaType, MediaType dstMedium)
{
    auto future = threadPool_->Enqueue(
        [this, objMeta, srcFilter](const std::string keyL, const MmcLocation srcL, const MmcLocation dstL) {
            auto ret = MoveBlob(keyL, srcL, dstL);
            if (ret != MMC_OK) {
                // 池线程内未持有 memObj 锁，fallback 需自行加锁
                std::unique_lock<std::mutex> guard(objMeta->Mutex());
                PushRemoveList(keyL, objMeta, srcFilter, false);
                MMC_LOG_WARN("key: " << keyL << " move blob from " << srcL << " to " << dstL
                                     << " not successful: " << ret << ", remove src blob");
            }
            return ret;
        },
        key, src, dst);
    if (!future.valid()) {
        MMC_LOG_WARN("key: " << key << " move blob from " << src << " to " << dst << " not successful");
        return EvictRemoveSrc(key, objMeta, srcFilter, dstMedium);
    }
    return EvictResult::MOVE_DOWN;
}

EvictResult MmcMetaManager::EvictCallBackFunction(const std::string &key, const MmcMemObjMetaPtr &objMeta,
                                                  MediaType srcMediaType)
{
    if (objMeta == nullptr) {
        MMC_LOG_ERROR("objMeta is null");
        return EvictResult::FAIL;
    }

    TP_TRACE_BEGIN(TP_MMC_META_EVICT);

    std::unique_lock<std::mutex> guard(objMeta->Mutex());

    MediaType dstMedium = MoveDown(srcMediaType);
    MmcBlobFilterPtr srcFilter = MmcMakeRef<MmcBlobFilter>(UINT32_MAX, srcMediaType, READABLE);

    std::vector<MmcMemBlobDesc> evictBlobs;
    objMeta->GetBlobsDesc(evictBlobs, srcFilter);
    if (evictBlobs.empty()) {
        // ALLOCATED-only blobs (write not finished or WRITE_OK lost) cannot be evicted through the
        // READABLE path; skipping would block LRU eviction for every key.
        MMC_LOG_WARN("Evict ALLOCATED-only key=" << key << " from " << srcMediaType << ", remove unreadable blob");
        TP_TRACE_END(TP_MMC_META_EVICT, MMC_OK);
        return EvictRemoveSrc(key, objMeta, srcMediaType, dstMedium);
    }
    uint32_t evictRank = evictBlobs[0].rank_;
    MmcMetaMetricManager::GetInstance().IncrementEvictCounter(evictRank);

    uint64_t freeSize = globalAllocator_->GetFreeSpace(dstMedium);
    if (freeSize < objMeta->Size()) {
        MMC_LOG_DEBUG("Evict REMOVE key=" << key << " from " << srcMediaType
                                          << " reason=no_space, freeSize=" << freeSize << ", need=" << objMeta->Size());
        TP_TRACE_END(TP_MMC_META_EVICT, MMC_OK);
        return EvictRemoveSrc(key, objMeta, srcFilter, dstMedium);
    }

    MmcLocation src{UINT32_MAX, srcMediaType};
    MmcLocation dst{UINT32_MAX, dstMedium};
    TP_TRACE_END(TP_MMC_META_EVICT, MMC_OK);
    return DispatchMoveBlob(key, objMeta, srcFilter, src, dst, srcMediaType, dstMedium);
}

Result MmcMetaManager::RewarmAllocBlob(const std::string &key, const MmcMemBlobDesc &srcDesc, MediaType dstMediaType,
                                       const MmcMemObjMetaPtr &objMeta, MmcMemBlobPtr &outBlob, MmcMemBlobDesc &outDesc)
{
    AllocOptions allocOpt{};
    allocOpt.blobSize_ = srcDesc.size_;
    allocOpt.numBlobs_ = 1;
    allocOpt.mediaType_ = dstMediaType;
    allocOpt.flags_ = (dstMediaType == MEDIA_HBM) ? 0 : ALLOC_FORCE_BY_RANK;
    if (dstMediaType != MEDIA_HBM) {
        allocOpt.preferredRank_.push_back(srcDesc.rank_);
    }

    std::vector<MmcMemBlobPtr> newBlobs;
    CheckAndEvict(dstMediaType, srcDesc.size_);
    TP_TRACE_BEGIN(TP_MMC_META_REWARM_ALLOC_BLOB);
    auto ret = globalAllocator_->Alloc(allocOpt, newBlobs);
    TP_TRACE_END(TP_MMC_META_REWARM_ALLOC_BLOB, ret);
    if (ret != MMC_OK || newBlobs.empty()) {
        MMC_LOG_ERROR("alloc failed for rewarm, dstMedia=" << dstMediaType << ", key=" << key << ", ret=" << ret);
        return MMC_MALLOC_FAILED;
    }

    ret = objMeta->AddBlob(newBlobs[0]);
    if (ret != MMC_OK) {
        MMC_LOG_ERROR("AddBlob failed for rewarm, key=" << key << ", ret=" << ret);
        globalAllocator_->Free(newBlobs);
        return MMC_ERROR;
    }
    newBlobs[0]->SetRewarmOrigin();
    outBlob = newBlobs[0];
    outDesc = newBlobs[0]->GetDesc();
    MmcMetaMetricManager::GetInstance().IncrementRewarmBytesCurrent(newBlobs[0]->Size(), outDesc.rank_);
    return MMC_OK;
}

void MmcMetaManager::RewarmFinalize(const std::string &key, const MmcMemBlobPtr &blob, MediaType dstMediaType,
                                    uint32_t srcRank)
{
    metaContainer_->InsertLru(key, dstMediaType);
    MmcMetaMetricManager::GetInstance().IncrementRewarmCounter(srcRank);
    MMC_LOG_TRACE("rewarmed key=" << key << " to " << dstMediaType << ", size=" << blob->Size());
}

Result MmcMetaManager::RewarmBlob(const std::string &key, const MmcMemObjMetaPtr &objMeta,
                                  std::unique_lock<std::mutex> &guard, const MmcMemBlobDesc &srcDesc,
                                  MediaType dstMediaType, MmcMemBlobPtr &dstBlob)
{
    if (!IsSsdAvailable(srcDesc.rank_)) {
        return MMC_SSD_NOT_AVAILABLE;
    }
    MmcMemBlobPtr newBlob;
    MmcMemBlobDesc dstDesc;
    auto ret = RewarmAllocBlob(key, srcDesc, dstMediaType, objMeta, newBlob, dstDesc);
    if (ret != MMC_OK) {
        return ret;
    }
    auto rollback = [&, rollbackRank = dstDesc.rank_]() -> void {
        MMC_LOG_WARN("rolling back rewarm, key=" << key);
        MmcBlobFilterPtr rbFilter = MmcMakeRef<MmcBlobFilter>(rollbackRank, dstMediaType, NONE);
        auto blobs = objMeta->DetachBlobs(rbFilter);
        guard.unlock();
        objMeta->FinalizeBlobs(key, globalAllocator_, blobs, false);
        guard.lock();
        objMeta->NotifyReadable();
    };

    if (metaNetServer_.Get() == nullptr) {
        MMC_LOG_WARN("metaNetServer_ is null, cannot copy blob for rewarm, key=" << key);
        rollback();
        return MMC_ERROR;
    }

    BatchBlobCopyRequest request{{key}, {srcDesc}, {dstDesc}};
    BatchBlobCopyResponse response;
    TP_TRACE_BEGIN(TP_MMC_META_REWARM_COPY_BLOB);
    ret = metaNetServer_->SyncCall(dstDesc.rank_, request, response, TIMEOUT_SECOND);
    TP_TRACE_END(TP_MMC_META_REWARM_COPY_BLOB, ret);
    if (ret != MMC_OK || response.results_.size() != kSingleBlobCount || response.results_[0] != MMC_OK) {
        MMC_LOG_ERROR("batch copy RPC failed for rewarm, key=" << key << ", ret=" << ret);
        rollback();
        return MMC_ERROR;
    }

    ret = newBlob->UpdateState(key, srcDesc.rank_, 0, MMC_WRITE_OK);
    if (ret != MMC_OK) {
        MMC_LOG_WARN("Unable to UpdateState WRITE_OK for rewarm, key=" << key << ", ret=" << ret);
        MmcBlobFilterPtr rbFilter = MmcMakeRef<MmcBlobFilter>(newBlob->GetDesc().rank_, dstMediaType, NONE);
        auto blobs = objMeta->DetachBlobs(rbFilter);
        guard.unlock();
        objMeta->FinalizeBlobs(key, globalAllocator_, blobs, false);
        guard.lock();
        objMeta->NotifyReadable();
        return MMC_ERROR;
    }
    // key 级：回温目标 blob 已 READABLE，唤醒等待本 key 的并发 Get()
    objMeta->NotifyReadable();
    dstBlob = newBlob;

    RewarmFinalize(key, newBlob, dstMediaType, srcDesc.rank_);

    return MMC_OK;
}

Result MmcMetaManager::UpdateConfig(uint64_t leaseTtlMs, uint16_t evictThresholdHigh, uint16_t evictThresholdLow)
{
    const uint64_t newLeaseTtlMs = leaseTtlMs == 0 ? MMC_DATA_TTL_MS : leaseTtlMs;
    if (evictThresholdHigh <= evictThresholdLow) {
        MMC_LOG_ERROR("invalid evict thresholds, high=" << evictThresholdHigh << ", low=" << evictThresholdLow
                                                        << ", high must be greater than low");
        return MMC_INVALID_PARAM;
    }
    if (newLeaseTtlMs == 0) {
        MMC_LOG_ERROR("invalid leaseTtlMs, must be greater than 0, got " << leaseTtlMs);
        return MMC_INVALID_PARAM;
    }

    if (defaultTtlMs_.load(std::memory_order_relaxed) != newLeaseTtlMs) {
        MMC_LOG_TRACE("meta leaseTtlMs hot-reloaded from " << defaultTtlMs_.load(std::memory_order_relaxed) << " to "
                                                           << newLeaseTtlMs);
        defaultTtlMs_.store(newLeaseTtlMs, std::memory_order_relaxed);
    }
    if (evictThresholdHigh_.load(std::memory_order_relaxed) != evictThresholdHigh) {
        MMC_LOG_TRACE("meta evictThresholdHigh hot-reloaded from "
                      << evictThresholdHigh_.load(std::memory_order_relaxed) << " to " << evictThresholdHigh);
        evictThresholdHigh_.store(evictThresholdHigh, std::memory_order_relaxed);
    }
    if (evictThresholdLow_.load(std::memory_order_relaxed) != evictThresholdLow) {
        MMC_LOG_TRACE("meta evictThresholdLow hot-reloaded from " << evictThresholdLow_.load(std::memory_order_relaxed)
                                                                  << " to " << evictThresholdLow);
        evictThresholdLow_.store(evictThresholdLow, std::memory_order_relaxed);
    }
    return MMC_OK;
}

Result MmcMetaManager::UpdateDefragEvictThreshold(uint16_t defragEvictThreshold)
{
    if (defragEvictThreshold < MIN_DEFRAG_EVICT_THRESHOLD || defragEvictThreshold > MAX_DEFRAG_EVICT_THRESHOLD) {
        MMC_LOG_ERROR("invalid defrag evict waste percent threshold, value=" << defragEvictThreshold << ", must be in ["
                                                                             << MIN_DEFRAG_EVICT_THRESHOLD << ", "
                                                                             << MAX_DEFRAG_EVICT_THRESHOLD << "]");
        return MMC_INVALID_PARAM;
    }
    if (defragEvictThreshold_.load(std::memory_order_relaxed) != defragEvictThreshold) {
        MMC_LOG_TRACE("meta defragEvictThreshold hot-reloaded from "
                      << defragEvictThreshold_.load(std::memory_order_relaxed) << " to " << defragEvictThreshold);
        defragEvictThreshold_.store(defragEvictThreshold, std::memory_order_relaxed);
    }
    return MMC_OK;
}

} // namespace mmc
} // namespace ock
