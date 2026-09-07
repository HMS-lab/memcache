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
#include "mmc_net_engine_acc.h"

#include <chrono>

#include "mf_tls_util.h"
#include "acc_def.h"
#include "acc_tcp_server.h"
#include "mmc_net_link_acc.h"
#include "mmc_net_ctx_store.h"
#include "mmc_msg_base.h"
#include "mmc_net_wait_handle.h"
#include "mmc_net_common_acc.h"
#include "mmc_ptracer.h"
#include "mmc_net_ctx_acc.h"

namespace ock {
namespace mmc {
constexpr const int16_t NET_SERVER_MAGIC = 3867;
constexpr const int NET_POOL_BASE = 16;
// 纯 client（未初始化 BM）建连时宣告的无效 rank id，接收端应忽略该 rank。
constexpr const uint64_t INVALID_RANK_ID = UINT64_MAX;

Result NetEngineAcc::Start(const NetEngineOptions &options)
{
    std::lock_guard<std::mutex> guard(mutex_);
    if (started_) {
        MMC_LOG_TRACE("NetEngineAcc " << options.name << " already started");
        return MMC_OK;
    }

    /* verify */
    MMC_RETURN_ERROR(VerifyOptions(options), "NetEngineAcc " << options.name << " option set error");

    /* initialize */
    MMC_RETURN_ERROR(Initialize(options), "NetEngineAcc " << options.name << " initialize error");

    /* check handler */
    if (options_.startListener && newLinkHandler_ == nullptr) {
        MMC_LOG_ERROR("No new link handler function registered, call 'RegisterNewLinkHandler' to register");
        return MMC_INVALID_PARAM;
    } else if (!options_.startListener && newLinkHandler_ != nullptr) {
        MMC_LOG_ERROR("No need to register new link handler function for the engine without listener");
        return MMC_INVALID_PARAM;
    }

    /* call start inner */
    auto ret = StartInner();
    if (ret != MMC_OK) {
        UnInitialize();
        MMC_LOG_ERROR("NetEngineAcc " << options.name << " start error");
        return ret;
    }

    threadPool_ = MmcMakeRef<MmcThreadPool>("net_pool", NET_POOL_BASE);
    if (threadPool_ == nullptr || threadPool_->Start() != MMC_OK) {
        StopInner();
        UnInitialize();
        MMC_LOG_ERROR("Failed to start thread pool");
        return MMC_ERROR;
    }

    started_ = true;
    return MMC_OK;
}

void NetEngineAcc::Stop()
{
    std::lock_guard<std::mutex> guard(mutex_);
    if (!started_) {
        MMC_LOG_WARN("NetEngineAcc has not been started");
        return;
    }
    // 先停 server（join 所有 AccTcpWorker），确保不会再有任何线程进入 HandleNeqRequest
    // 访问 threadPool_；之后再销毁并置空线程池，避免 worker 在 teardown 期间解引用空指针。
    Result result = StopInner();
    MMC_ASSERT(result == MMC_OK, "StopInner = " << result);

    if (threadPool_ != nullptr) {
        threadPool_->Destroy();
        threadPool_ = nullptr;
    }

    // 先关进入口：在持 mutex_ 时置 started_=false，使新 Call() 在 started_ 检查处早退，
    // 不再触达 ctxStore_；已越过检查的在途 Call() 已完成 ++inFlightCalls_，会被
    // UnInitialize 排空后再释放 ctxStore_。
    started_ = false;
    UnInitialize();
}

Result NetEngineAcc::VerifyOptions(const NetEngineOptions &options)
{
    if (options.rankId == UINT16_MAX) {
        MMC_LOG_ERROR("verify NetEngineOptions failed, invalid rank id " << options.rankId);
        return MMC_INVALID_PARAM;
    } else if (options.name.empty()) {
        MMC_LOG_ERROR("verify NetEngineOptions failed, empty name");
        return MMC_INVALID_PARAM;
    }

    if (options.startListener) {
        if (options.ip.empty()) {
            MMC_LOG_ERROR("verify NetEngineOptions failed, ip is empty");
            return MMC_INVALID_PARAM;
        } else if (options.port <= N1024) {
            MMC_LOG_ERROR("verify NetEngineOptions failed, invalid port " << options.port);
            return MMC_INVALID_PARAM;
        }
    }

    if (options.threadCount > N1024 || options.threadCount == 0) {
        MMC_LOG_ERROR("verify NetEngineOptions failed, threadCount is too large, which should between 1~" << N1024);
        return MMC_INVALID_PARAM;
    }

    return MMC_OK;
}

Result NetEngineAcc::StartInner()
{
    /* construct acc tcp server */
    TcpServerOptions serverOptions;
    serverOptions.version = static_cast<int16_t>(NetProtoVersion::VERSION_1);
    serverOptions.magic = NET_SERVER_MAGIC;
    if (options_.startListener) {
        serverOptions.enableListener = true;
        serverOptions.listenIp = options_.ip;
        serverOptions.listenPort = options_.port;
    }
    serverOptions.workerCount = options_.threadCount;
    serverOptions.linkSendQueueSize = UN32;

    TcpTlsOption tlsOpt{};
    tlsOpt.enableTls = options_.tlsOption.tlsEnable;
    if (tlsOpt.enableTls) {
        tlsOpt.tlsTopPath = "/";
        tlsOpt.tlsCaPath = "/";
        tlsOpt.tlsCaFile.insert(options_.tlsOption.caPath);
        tlsOpt.tlsCrlPath = "/";
        std::string crlFile = options_.tlsOption.crlPath;
        if (!crlFile.empty()) {
            tlsOpt.tlsCrlFile.insert(crlFile);
        }
        tlsOpt.tlsCert = options_.tlsOption.certPath;
        tlsOpt.tlsPk = options_.tlsOption.keyPath;
        tlsOpt.tlsPkPwd = options_.tlsOption.keyPassPath;
        MMC_RETURN_ERROR(server_->LoadDynamicLib(options_.tlsOption.packagePath),
                         "Failed to load openssl dynamic library");
        if (!tlsOpt.tlsPkPwd.empty()) {
            MMC_RETURN_ERROR(RegisterDecryptHandler(options_.tlsOption.decrypterLibPath),
                             "Failed to register decrypt handler");
        }
    }

    /* start server, listen and thread will be started */
    MMC_RETURN_ERROR(server_->Start(serverOptions, tlsOpt),
                     "Failed to start tcp server for NetEngine " << options_.name);

    return MMC_OK;
}

Result NetEngineAcc::StopInner()
{
    /* stop server */
    if (server_ != nullptr) {
        server_->Stop();
        server_ = nullptr;
    }
    mf::MfTlsUtil::CloseTlsLib();
    return MMC_OK;
}

static void TraceSendRecord(int16_t opCode, uint64_t diff)
{
    switch (opCode) {
        case ML_ALLOC_REQ:
            TP_TRACE_RECORD(TP_ACC_SEND_ALLOC, diff, 0);
            break;
        case ML_UPDATE_REQ:
            TP_TRACE_RECORD(TP_ACC_SEND_UPDATE, diff, 0);
            break;
        case ML_GET_REQ:
            TP_TRACE_RECORD(TP_ACC_SEND_GET, diff, 0);
            break;
        case ML_REMOVE_REQ:
            TP_TRACE_RECORD(TP_ACC_SEND_REMOVE, diff, 0);
            break;
        case ML_IS_EXIST_REQ:
            TP_TRACE_RECORD(TP_ACC_SEND_EXIST, diff, 0);
            break;
        case ML_QUERY_REQ:
            TP_TRACE_RECORD(TP_ACC_SEND_QUERY, diff, 0);
            break;
        case ML_BATCH_IS_EXIST_REQ:
            TP_TRACE_RECORD(TP_ACC_SEND_EXIST_BAT, diff, 0);
            break;
        case ML_BATCH_REMOVE_REQ:
            TP_TRACE_RECORD(TP_ACC_SEND_REMOVE_BAT, diff, 0);
            break;
        case ML_BATCH_GET_REQ:
            TP_TRACE_RECORD(TP_ACC_SEND_GET_BAT, diff, 0);
            break;
        case ML_BATCH_QUERY_REQ:
            TP_TRACE_RECORD(TP_ACC_SEND_QUERY_BAT, diff, 0);
            break;
        case ML_BATCH_UPDATE_LEASE_REQ:
            TP_TRACE_RECORD(TP_ACC_SEND_QUERY_BAT, diff, 0);
            break;
        case ML_BATCH_ALLOC_REQ:
            TP_TRACE_RECORD(TP_ACC_SEND_ALLOC_BAT, diff, 0);
            break;
        case ML_BATCH_UPDATE_REQ:
            TP_TRACE_RECORD(TP_ACC_SEND_UPDATE_BAT, diff, 0);
            break;
        default:
            break;
    };
}

static void TraceSendWaitRecord(int16_t opCode, uint64_t diff)
{
    switch (opCode) {
        case ML_ALLOC_REQ:
            TP_TRACE_RECORD(TP_ACC_SEND_WAIT_ALLOC, diff, 0);
            break;
        case ML_UPDATE_REQ:
            TP_TRACE_RECORD(TP_ACC_SEND_WAIT_UPDATE, diff, 0);
            break;
        case ML_GET_REQ:
            TP_TRACE_RECORD(TP_ACC_SEND_WAIT_GET, diff, 0);
            break;
        case ML_REMOVE_REQ:
            TP_TRACE_RECORD(TP_ACC_SEND_WAIT_REMOVE, diff, 0);
            break;
        case ML_IS_EXIST_REQ:
            TP_TRACE_RECORD(TP_ACC_SEND_WAIT_EXIST, diff, 0);
            break;
        case ML_QUERY_REQ:
            TP_TRACE_RECORD(TP_ACC_SEND_WAIT_QUERY, diff, 0);
            break;
        case ML_BATCH_IS_EXIST_REQ:
            TP_TRACE_RECORD(TP_ACC_SEND_WAIT_EXIST_BAT, diff, 0);
            break;
        case ML_BATCH_REMOVE_REQ:
            TP_TRACE_RECORD(TP_ACC_SEND_WAIT_REMOVE_BAT, diff, 0);
            break;
        case ML_BATCH_GET_REQ:
            TP_TRACE_RECORD(TP_ACC_SEND_WAIT_GET_BAT, diff, 0);
            break;
        case ML_BATCH_QUERY_REQ:
            TP_TRACE_RECORD(TP_ACC_SEND_WAIT_QUERY_BAT, diff, 0);
            break;
        case ML_BATCH_UPDATE_LEASE_REQ:
            TP_TRACE_RECORD(TP_ACC_SEND_WAIT_QUERY_BAT, diff, 0);
            break;
        case ML_BATCH_ALLOC_REQ:
            TP_TRACE_RECORD(TP_ACC_SEND_WAIT_ALLOC_BAT, diff, 0);
            break;
        default:
            break;
    };
}

Result NetEngineAcc::Call(uint32_t targetId, int16_t opCode, const char *reqData, uint32_t reqDataLen, char **respData,
                          uint32_t &respDataLen, int32_t timeoutInSecond)
{
    uint64_t startTime = TP_CURRENT_TIME_NS;
    /* 在途调用计数必须最先建立（started_ 检查之前）：Stop 中 UnInitialize 会等待
       inFlightCalls_ 归零后再置空 ctxStore_，若在 started_ 检查后、guard 建立前被
       Stop 抢先释放 ctxStore_，本函数仍会访问已释放对象。 */
    /* InFlightGuard: 用 inFlightMutex_ 保护 check-then-act 的复合原子性。
       inFlightCalls_ 虽是 atomic，但 UnInitialize 需要"判断归零→释放 ctxStore_"
       整体为原子操作；若不加锁，新 Call++ 后已越过 started_ 检查，
       可在归零判定与 ctxStore_ 释放之间插入，导致 UAF。 */
    struct InFlightGuard {
        NetEngineAcc *engine;
        explicit InFlightGuard(NetEngineAcc *e) : engine(e)
        {
            std::lock_guard<std::mutex> guard(engine->inFlightMutex_);
            ++engine->inFlightCalls_;
        }
        ~InFlightGuard()
        {
            std::lock_guard<std::mutex> guard(engine->inFlightMutex_);
            --engine->inFlightCalls_;
            if (engine->inFlightCalls_ == 0) {
                engine->inFlightCv_.notify_all();
            }
        }
    } inFlightGuard(this);
    MMC_ASSERT_LOG_AND_RETURN(started_, "started_ = " << started_, MMC_NOT_STARTED);
    MMC_ASSERT_LOG_AND_RETURN(reqData != nullptr, "reqData is nullptr", MMC_INVALID_PARAM);
    MMC_ASSERT_LOG_AND_RETURN(reqDataLen != 0, "reqDataLen = " << reqDataLen, MMC_INVALID_PARAM);
    MMC_ASSERT_LOG_AND_RETURN(respData != nullptr, "respData is nullptr", MMC_INVALID_PARAM);

    MMC_ASSERT_LOG_AND_RETURN(opCode != -1, "opCode = " << opCode, MMC_INVALID_PARAM);

    /* step1: do serialization */

    /* step2: get the link to send */
    NetLinkAccPtr link;
    auto successful = peerLinkMap_->Find(targetId, link);
    if (!successful || link == nullptr) {
        return MMC_LINK_NOT_FOUND; /* need to connect */
    }
    /* step3: copy data */
    auto dataBuf = MmcMakeRef<ock::acc::AccDataBuffer>(reqDataLen);
    MMC_ASSERT_LOG_AND_RETURN(dataBuf.Get() != nullptr, "dataBuf.Get() is nullptr", MMC_NEW_OBJECT_FAILED);
    auto ret = dataBuf->AllocIfNeed();
    MMC_ASSERT_LOG_AND_RETURN(ret, "dataBuf->AllocIfNeed() = " << ret, MMC_NEW_OBJECT_FAILED);
    std::copy_n(reqData, reqDataLen, static_cast<char *>(dataBuf->DataPtrVoid()));
    dataBuf->SetDataSize(reqDataLen);

    /* step4: create wait handler and initialize */
    auto waiter = MmcMakeRef<NetWaitHandler>(ctxStore_);
    MMC_ASSERT_LOG_AND_RETURN(waiter.Get() != nullptr, "waiter.Get() is nullptr", MMC_NEW_OBJECT_FAILED);
    auto res = waiter->Initialize();
    MMC_ASSERT_LOG_AND_RETURN(res == MMC_OK, "res = " << res, MMC_ERROR);

    /* step5: put into ctx store before sent the data to peer in case of the peer responses very fast  */
    uint32_t seqNo = 0;
    Result result = ctxStore_->PutAndGetSeqNo<NetWaitHandler>(waiter.Get(), seqNo);
    MMC_ASSERT_LOG_AND_RETURN(result == MMC_OK, "result = " << result, result);
    MMC_ASSERT_LOG_AND_RETURN(link->RealLink() != nullptr, "link->RealLink() is nullptr", MMC_ERROR);
    /* step6: send message to peer */

    result = link->RealLink()->NonBlockSend(MSG_TYPE_DATA, opCode, seqNo, dataBuf.Get(), nullptr);
    if (result != MMC_OK) {
        /* remove wait handler from context store */
        ctxStore_->RemoveSeqNo<NetWaitHandler>(seqNo);
        MMC_LOG_ERROR("Failed to send data to service " << targetId);
        return result;
    }
    uint64_t waitStartTime = TP_CURRENT_TIME_NS;
    /* step7: wait for response */
    result = waiter->TimedWait(timeoutInSecond);
    TraceSendWaitRecord(opCode, TP_CURRENT_TIME_NS - waitStartTime);
    /* if timeout */
    if (result == MMC_TIMEOUT) {
        /* remove waiter in context store */
        ctxStore_->RemoveSeqNo<NetWaitHandler>(seqNo);
        MMC_LOG_WARN("Peer " << targetId << " doesn't response within " << timeoutInSecond << " seconds");
        return result;
    }

    /* got response data and deserialize */
    auto &data = waiter->Data();
    MMC_ASSERT_LOG_AND_RETURN(data.Get() != nullptr, "data.Get() is nullptr", MMC_ERROR);
    /* set response code */

    /* deserialize */

    if (*respData == nullptr) {
        *respData = (char *)malloc(data->DataLen());
        if (*respData == nullptr) {
            MMC_LOG_ERROR("Failed to malloc resp data length:" << data->DataLen());
            return MMC_MALLOC_FAILED;
        }
    }
    std::copy_n(reinterpret_cast<char *>(data->DataIntPtr()), data->DataLen(), *respData);
    respDataLen = data->DataLen();
    TraceSendRecord(opCode, TP_CURRENT_TIME_NS - startTime);
    return result;
}

Result NetEngineAcc::Send(uint32_t peerId, const char *reqData, uint32_t reqDataLen, int32_t timeoutInSecond)
{
    MMC_ASSERT_LOG_AND_RETURN(started_, "started_ = " << started_, MMC_NOT_STARTED);
    MMC_ASSERT_LOG_AND_RETURN(reqData != nullptr, "reqData is nullptr", MMC_INVALID_PARAM);
    MMC_ASSERT_LOG_AND_RETURN(reqDataLen != 0, "reqDataLen = " << reqDataLen, MMC_INVALID_PARAM);

    return MMC_OK;
}

NetEngineAcc::~NetEngineAcc()
{
    Stop();
}

Result NetEngineAcc::Initialize(const NetEngineOptions &options)
{
    if (inited_) {
        MMC_LOG_TRACE("NetEngine [" << options.name << "] already initialized");
        return MMC_OK;
    }

    MMC_LOG_DEBUG("Start to init NetEngine " << options.name);

    /* create concurrent link map */
    NetLinkMapAccPtr tmpLinkMap = MmcMakeRef<NetLinkMapAcc>();
    MMC_ASSERT_LOG_AND_RETURN(tmpLinkMap != nullptr, "tmpLinkMap is nullptr", MMC_NEW_OBJECT_FAILED);

    /* create ctx store */
    NetContextStorePtr tmpCtxStore = MmcMakeRef<NetContextStore>(UN65536);
    MMC_ASSERT_LOG_AND_RETURN(tmpCtxStore != nullptr, "tmpCtxStore is nullptr", MMC_NEW_OBJECT_FAILED);
    auto result = tmpCtxStore->Initialize();
    MMC_RETURN_ERROR(result, "Failed to initialize ctx store for communication seq number");

    /* create tcp server */
    auto tmpServer = TcpServer::Create();
    MMC_ASSERT_LOG_AND_RETURN(tmpServer != nullptr, "tmpServer is nullptr", MMC_NEW_OBJECT_FAILED);
    server_ = tmpServer.Get();

    /* register callbacks */
    options_ = options;
    MMC_RETURN_ERROR(RegisterTcpServerHandler(), "NetEngineAcc " << options_.name << " register handler error");
    /* assign to member variables */
    peerLinkMap_ = tmpLinkMap;
    ctxStore_ = tmpCtxStore;

    inited_ = true;

    return MMC_OK;
}

void NetEngineAcc::UnInitialize()
{
    if (!inited_) {
        MMC_LOG_DEBUG("NetEngine [" << options_.name << "] has not been initialized");
        return;
    }

    /* 等待所有在途 Call 返回后再释放 ctxStore_，避免异步调用者
       （如元数据备份 SendBackupForRank）在 ctxStore_ 释放后继续访问 */
    constexpr int kDrainTimeoutSec = 120;
    {
        std::unique_lock<std::mutex> lock(inFlightMutex_);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kDrainTimeoutSec);
        if (!inFlightCv_.wait_until(lock, deadline, [this]() { return inFlightCalls_ == 0; })) {
            MMC_LOG_ERROR("in-flight calls drain timeout after "
                          << kDrainTimeoutSec << "s, count=" << inFlightCalls_.load() << ", forcing teardown");
        }
    }

    /* un-initialize ctx store */
    if (ctxStore_ != nullptr) {
        ctxStore_->UnInitialize();
        ctxStore_ = nullptr;
    }

    if (server_ != nullptr) {
        server_->Stop();
        server_ = nullptr;
    }

    /* clear link map */
    if (peerLinkMap_ != nullptr) {
        peerLinkMap_->Clear();
    }

    /* clear ignored-rank link id set */
    {
        std::lock_guard<std::mutex> guard(ignoredRankLinksMutex_);
        ignoredRankLinkIds_.clear();
    }

    inited_ = false;
}

Result NetEngineAcc::RegisterTcpServerHandler()
{
    MMC_ASSERT_LOG_AND_RETURN(server_ != nullptr, "server_ is nullptr", MMC_NOT_INITIALIZED);

    using namespace std::placeholders;
    if (options_.startListener) {
        server_->RegisterNewLinkHandler(std::bind(&NetEngineAcc::HandleNewLink, this, _1, _2));
    }

    server_->RegisterNewRequestHandler(MSG_TYPE_DATA, std::bind(&NetEngineAcc::HandleNeqRequest, this, _1));
    server_->RegisterRequestSentHandler(MSG_TYPE_DATA, std::bind(&NetEngineAcc::HandleMsgSent, this, _1, _2, _3));
    server_->RegisterLinkBrokenHandler(std::bind(&NetEngineAcc::HandleLinkBroken, this, _1));

    return MMC_OK;
}

Result NetEngineAcc::HandleNewLink(const TcpConnReq &req, const TcpLinkPtr &link) const
{
    MMC_ASSERT_LOG_AND_RETURN(link.Get() != nullptr, "link.Get() is nullptr", MMC_INVALID_PARAM);

    const bool ignoreRank = (req.rankId == INVALID_RANK_ID);
    auto peerId = static_cast<uint32_t>(req.rankId);
    link->UpCtx(peerId);

    auto newLinkAcc = MmcMakeRef<NetLinkAcc>(peerId, link);
    MMC_ASSERT_LOG_AND_RETURN(newLinkAcc != nullptr, "newLinkAcc is nullptr", MMC_NEW_OBJECT_FAILED);
    MMC_LOG_DEBUG("NEW Link");

    if (ignoreRank) {
        /* 纯 client（无 BM）建连，rank 无效，放入独立集合跟踪，避免污染 rank 键空间 */
        {
            std::lock_guard<std::mutex> guard(ignoredRankLinksMutex_);
            ignoredRankLinkIds_.insert(link->Id());
        }
        MMC_LOG_INFO("HandleNewLink with ignored rank, linkId: " << link->Id());
    } else {
        /* add into peer link map */
        peerLinkMap_->Add(peerId, newLinkAcc);
        MMC_LOG_INFO("HandleNewLink with peer rankId: " << req.rankId << ", linkId: " << link->Id());
    }
    Result ret = MMC_OK;
    if (newLinkHandler_ != nullptr) {
        ret = newLinkHandler_(newLinkAcc.Get());
    }
    return MMC_OK;
}

Result NetEngineAcc::HandleNeqRequest(const TcpReqContext &context)
{
    /* use result variable for real opcode */
    MMC_LOG_DEBUG("HandleNeqRequest Header " << context.Header().ToString());
    int16_t opCode = context.Header().result;
    MMC_ASSERT_LOG_AND_RETURN(opCode >= gHandlerMin && opCode < gHandlerMax,
                              "opCode = " << opCode << ", gHandlerMin = " << gHandlerMin
                                          << ", gHandlerMax = " << gHandlerMax,
                              MMC_NET_REQ_HANDLE_NO_FOUND);
    if (reqReceivedHandlers_[opCode] == nullptr) {
        /*  client do reply response */
        auto res = HandleAllRequests4Response(context);
        MMC_ASSERT_LOG_AND_RETURN(res == MMC_OK, "HandleAllRequests4Response(context) = " << res, MMC_ERROR);
    } else {
        /* server do function */
        // context buf是link缓冲区，切线程需要将数据copy出来
        ock::acc::AccDataBufferPtr bufPtr = ock::acc::AccDataBuffer::Create(context.DataPtr(), context.DataLen());
        if (bufPtr.Get() == nullptr) {
            MMC_LOG_ERROR("req: " << context.SeqNo() << " alloc failed. op:" << opCode);
            return MMC_ERROR;
        }
        ock::acc::AccTcpRequestContext reqContext(context.Header(), bufPtr, context.Link());
        NetContextPtr asynCtxPtr = MmcMakeRef<NetContextAcc>(reqContext).Get();
        if (asynCtxPtr.Get() == nullptr) {
            MMC_LOG_ERROR("req: " << context.SeqNo() << " alloc ctx failed. op:" << opCode);
            return MMC_ERROR;
        }
        if (threadPool_ == nullptr) {
            MMC_LOG_ERROR("req: " << context.SeqNo() << " thread pool is null, engine stopped. op:" << opCode);
            return MMC_ERROR;
        }
        auto future = threadPool_->Enqueue(
            [&](int16_t opCode, NetContextPtr contextPtrL) { return reqReceivedHandlers_[opCode](contextPtrL); },
            opCode, asynCtxPtr);
        if (!future.valid()) {
            MMC_LOG_ERROR("req: " << context.SeqNo() << " add thread pool failed. op:" << opCode);
            return MMC_ERROR;
        }
    }
    return MMC_OK;
}

Result NetEngineAcc::HandleMsgSent(TcpMsgSentResult result, const TcpMsgHeader &header, const TcpDataBufPtr &cbCtx)
{
    return MMC_OK;
}

Result NetEngineAcc::HandleLinkBroken(const TcpLinkPtr &link) const
{
    MMC_ASSERT_LOG_AND_RETURN(link.Get() != nullptr, "link.Get() is nullptr", MMC_INVALID_PARAM);

    /* 纯 client（ignore rank）链接断链，仅从独立集合中移除，无 rank 资源需要清理 */
    {
        std::lock_guard<std::mutex> guard(ignoredRankLinksMutex_);
        if (ignoredRankLinkIds_.erase(link->Id()) != 0) {
            return MMC_OK;
        }
    }

    const auto peerId = static_cast<uint32_t>(link->UpCtx());

    MmcRef<NetLinkAcc> linkAcc = nullptr;
    peerLinkMap_->Find(peerId, linkAcc);

    Result ret = MMC_OK;
    if (linkBrokenHandler_ != nullptr && linkAcc.Get() != nullptr && linkAcc->RealLink() != nullptr) {
        if (linkAcc->RealLink()->Id() == link->Id()) {
            peerLinkMap_->Remove(peerId);
            MMC_RETURN_ERROR(linkBrokenHandler_(linkAcc.Get()), "Failed to remove link with id " << peerId);
        } else {
            MMC_LOG_WARN("Old linkId: " << linkAcc->RealLink()->Id() << ", rankId: " << peerId);
        }
    }
    return ret;
}

Result NetEngineAcc::ConnectToPeer(uint32_t peerId, const std::string &peerIp, uint16_t port, NetLinkPtr &newLink,
                                   bool isForce, bool ignoreRankId)
{
    MMC_ASSERT_LOG_AND_RETURN(started_, "started_ = " << started_, MMC_NOT_STARTED);
    MMC_ASSERT_LOG_AND_RETURN(!peerIp.empty(), "peerIp is empty", MMC_INVALID_PARAM);
    MMC_ASSERT_LOG_AND_RETURN(port != 0, "port = " << port, MMC_INVALID_PARAM);

    TcpConnReq connReq;
    connReq.rankId = ignoreRankId ? INVALID_RANK_ID : peerId;
    connReq.version = static_cast<int16_t>(NetProtoVersion::VERSION_1);
    connReq.magic = NET_SERVER_MAGIC;

    NetLinkAccPtr linkAcc;
    /* connect to peer with mutex held, in case of multiple threads connect to peer at the same time */
    std::lock_guard<std::mutex> guard(connectMutex_);
    /* double check, in case of other thread already connected */
    bool result = peerLinkMap_->Find(peerId, linkAcc);
    if (result) {
        if (!isForce) {
            MMC_LOG_INFO("The link to peer " << peerId << " already exists");
            return MMC_OK;
        } else {
            peerLinkMap_->Remove(peerId);
        }
    }

    MMC_LOG_DEBUG("Connecting to " << peerIp << ":" << port << "");

    /* connect */
    TcpLinkPtr realLink;
    Result ret = server_->ConnectToPeerServer(peerIp, port, connReq, realLink);
    if (ret != MMC_OK || realLink == nullptr) {
        MMC_LOG_ERROR("Failed to connection to peerId: " << peerId << ", peerIpPort: " << peerIp << ":" << port);
        return ret;
    }
    realLink->UpCtx(peerId);

    /* add into peer link map */
    linkAcc = MmcMakeRef<NetLinkAcc>(peerId, realLink);
    MMC_ASSERT_LOG_AND_RETURN(linkAcc != nullptr, "linkAcc is nullptr", MMC_NEW_OBJECT_FAILED);
    peerLinkMap_->Add(peerId, linkAcc);

    /* set peer id */
    newLink = linkAcc.Get();
    newLink->UpCtx(peerId);

    return MMC_OK;
}

Result NetEngineAcc::HandleAllRequests4Response(const TcpReqContext &context)
{
    MMC_LOG_DEBUG("Get request with seqNo " << context.SeqNo());
    NetWaitHandler *out = nullptr;
    auto result = ctxStore_->GetSeqNoAndRemove<NetWaitHandler>(context.SeqNo(), out, false);
    /* check if out is nullptr */
    MMC_ASSERT_LOG_AND_RETURN(out != nullptr, "out is nullptr", MMC_ERROR);
    if (result != MMC_OK) {
        if (out != nullptr) { /* decrease ref */
            out->DecreaseRef();
        }

        MMC_LOG_WARN("Unable to get waiter from ctx store with seqNo " << context.SeqNo() << ", probably timeout");
        return MMC_OK;
    }

    NetWaitHandlerPtr waiter = out;
    out->DecreaseRef(); /* decrease ref */

    auto dataBuf = MmcMakeRef<ock::acc::AccDataBuffer>(context.DataLen());
    MMC_ASSERT_LOG_AND_RETURN(result == MMC_OK, "result = " << result, MMC_NEW_OBJECT_FAILED);
    auto ret = dataBuf->AllocIfNeed();
    MMC_ASSERT_LOG_AND_RETURN(ret, "dataBuf->AllocIfNeed() = " << ret, MMC_NEW_OBJECT_FAILED);
    MMC_ASSERT_LOG_AND_RETURN(context.DataPtr() != nullptr, "context.DataPtr() is nullptr", MMC_ERROR);
    std::copy_n(static_cast<char *>(context.DataPtr()), context.DataLen(),
                reinterpret_cast<char *>(dataBuf->DataIntPtr()));
    dataBuf->SetDataSize(context.DataLen());

    result = waiter->Notify(context.Header().result, dataBuf.Get());
    if (result == MMC_ALREADY_NOTIFIED) {
        MMC_LOG_WARN("Already notify wait handler for seqNo " << context.SeqNo());
        return MMC_OK;
    }

    return MMC_OK;
}

Result NetEngineAcc::RegisterDecryptHandler(const std::string &decryptLibPath) const
{
    if (decryptLibPath.empty()) {
        MMC_LOG_WARN("No decrypter provided, using default decrypter handler");
        server_->RegisterDecryptHandler(mf::MfTlsUtil::DefaultDecrypter);
        return MMC_OK;
    }

    const auto decrypter = mf::MfTlsUtil::LoadDecryptFunction(decryptLibPath.c_str());
    if (decrypter == nullptr) {
        MMC_LOG_ERROR("failed to load customized decrypt function");
        return MMC_ERROR;
    }
    server_->RegisterDecryptHandler(decrypter);

    return MMC_OK;
}
} // namespace mmc
} // namespace ock
