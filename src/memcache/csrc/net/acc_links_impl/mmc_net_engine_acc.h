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
#ifndef MEM_FABRIC_MMC_NET_ENGINE_ACC_LINKS_H
#define MEM_FABRIC_MMC_NET_ENGINE_ACC_LINKS_H

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "mmc_net_engine.h"
#include "mmc_net_common_acc.h"
#include "mmc_thread_pool.h"
#include "mmc_net_ctx_store.h"

namespace ock {
namespace mmc {
class NetEngineAcc final : public NetEngine {
public:
    ~NetEngineAcc() override;

    Result Start(const NetEngineOptions &options) override;
    void Stop() override;

    Result ConnectToPeer(uint32_t peerId, const std::string &peerIp, uint16_t port, NetLinkPtr &newLink, bool isForce,
                         bool ignoreRankId = false) override;
    Result ReleasePeer(uint32_t peerId) override;

    Result Call(uint32_t targetId, int16_t opCode, const char *reqData, uint32_t reqDataLen, char **respData,
                uint32_t &respDataLen, int32_t timeoutInSecond) override;

    Result Send(uint32_t peerId, const char *reqData, uint32_t reqDataLen, int32_t timeoutInSecond) override;

private:
    static Result VerifyOptions(const NetEngineOptions &options);
    Result Initialize(const NetEngineOptions &options);
    void UnInitialize();
    Result StartInner();
    Result StopInner();

    /* callback function of tcp server */
    Result RegisterTcpServerHandler();
    Result HandleNewLink(const TcpConnReq &req, const TcpLinkPtr &link) const;
    Result HandleNeqRequest(const TcpReqContext &context);
    Result HandleMsgSent(TcpMsgSentResult result, const TcpMsgHeader &header, const TcpDataBufPtr &cbCtx);
    Result HandleLinkBroken(const TcpLinkPtr &link);
    Result HandleAllRequests4Response(const TcpReqContext &context);

    Result RegisterDecryptHandler(const std::string &decryptLibPath) const;

    void AddPendingSeqNo(uint32_t peerId, uint32_t seqNo);
    void RemovePendingSeqNo(uint32_t peerId, uint32_t seqNo);
    void HandleFailedPendingRequests(uint32_t peerId);
    void HandleAllFailedPendingRequests();

    /* RAII guard: Add on construct, Remove on destruct, for pending seqNo tracking in Call */
    class PendingSeqNoGuard {
    public:
        PendingSeqNoGuard(NetEngineAcc *engine, uint32_t peerId, uint32_t seqNo)
            : engine_(engine), peerId_(peerId), seqNo_(seqNo)
        {
            engine_->AddPendingSeqNo(peerId_, seqNo_);
        }
        ~PendingSeqNoGuard()
        {
            engine_->RemovePendingSeqNo(peerId_, seqNo_);
        }
        PendingSeqNoGuard(const PendingSeqNoGuard &) = delete;
        PendingSeqNoGuard &operator=(const PendingSeqNoGuard &) = delete;

    private:
        NetEngineAcc *engine_;
        uint32_t peerId_;
        uint32_t seqNo_;
    };

private:
    /* hot used variables */
    TcpServerPtr server_;
    NetLinkMapAccPtr peerLinkMap_;
    NetContextStorePtr ctxStore_;

    /* 纯 client（ignore rank）建连的 link id 集合，与 rank->link 映射空间隔离 */
    mutable std::mutex ignoredRankLinksMutex_;
    mutable std::unordered_set<uint32_t> ignoredRankLinkIds_;

    /* pending seqNo tracking: peerId -> set of in-flight seqNos, for fast-fail on link break */
    std::mutex pendingMutex_;
    std::unordered_map<uint32_t, std::unordered_set<uint32_t>> pendingSeqNos_;

    /* not hot used variables */
    NetEngineOptions options_{};
    bool started_ = false;
    bool inited_ = false;
    std::mutex connectMutex_;
    int32_t timeoutInSecond_ = 0;
    MmcThreadPoolPtr threadPool_;

    /* 在途 Call 计数：teardown（UnInitialize 置空 ctxStore_）前等待归零，
       避免异步调用者（如元数据备份 SendBackupForRank）在 ctxStore_ 释放后读取 */
    std::atomic<int32_t> inFlightCalls_{0};
    std::condition_variable inFlightCv_;
    std::mutex inFlightMutex_;
};
} // namespace mmc
} // namespace ock

#endif // MEM_FABRIC_MMC_NET_ENGINE_ACC_LINKS_H
