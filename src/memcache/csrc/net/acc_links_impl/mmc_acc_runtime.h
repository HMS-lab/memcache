/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemCache_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
*/
#ifndef MEMCACHE_MMC_ACC_RUNTIME_H
#define MEMCACHE_MMC_ACC_RUNTIME_H

#include <cstdint>
#include <functional>
#include <set>
#include <string>

#include "mmc_ref.h"
#include "smem_acc_api.h"

namespace ock::mmc_acc {
constexpr int32_t ACC_OK = 0;
constexpr uint32_t MAX_RECV_BODY_LEN = 64 * 1024 * 1024;
constexpr int16_t MIN_MSG_TYPE = 0;
constexpr int16_t MAX_MSG_TYPE = 48;
enum AccMsgSentResult { MSG_SENT = 0, MSG_TIMEOUT = 1, MSG_LINK_BROKEN = 2, MSG_BUTT };

struct AccConnReq {
    int16_t magic{0};
    int16_t version{0};
    int8_t reconnect{0};
    int8_t reserved[3]{};
    uint64_t rankId{0};
};
struct AccConnResp {
    int16_t result{0};
};
struct AccMsgHeader {
    int16_t type{0};
    int16_t result{0};
    uint32_t bodyLen{0};
    uint32_t seqNo{0};
    uint32_t crc{0};
    std::string ToString() const;
};
struct AccTcpServerOptions {
    std::string listenIp;
    uint16_t listenPort{9966};
    uint16_t workerCount{2};
    uint16_t linkSendQueueSize{128};
    bool enableListener{false};
    int16_t magic{0};
    int16_t version{0};
};
struct AccTlsOption {
    bool enableTls{false};
    std::string tlsTopPath;
    std::string tlsCert;
    std::string tlsCrlPath;
    std::string tlsCaPath;
    std::set<std::string> tlsCaFile;
    std::set<std::string> tlsCrlFile;
    std::string tlsPk;
    std::string tlsPkPwd;
};
using AccDecryptHandler = std::function<int(const char *, size_t, char *, size_t)>;

class AccDataBuffer : public mmc::MmcReferable {
public:
    static mmc::MmcRef<AccDataBuffer> Create(const void *data, uint32_t size);
    static mmc::MmcRef<AccDataBuffer> Create(uint32_t memSize);
    bool AllocIfNeed(uint32_t newSize = 0) noexcept;
    uint8_t *DataPtr() const;
    void *DataPtrVoid() const;
    uintptr_t DataIntPtr() const;
    uint32_t DataLen() const;
    uint32_t MemSize() const;
    void SetDataSize(uint32_t size);
    ~AccDataBuffer() override;
    AccDataBuffer(const void *data, uint32_t size);
    explicit AccDataBuffer(uint32_t memSize);

private:
    uint32_t dataSize_ = 0;
    uint32_t memSize_;
    uint8_t *data_;
};
using AccDataBufferPtr = mmc::MmcRef<AccDataBuffer>;

class AccTcpLinkComplex : public mmc::MmcReferable {
public:
    explicit AccTcpLinkComplex(smem_acc_link_t handle, bool retain);
    ~AccTcpLinkComplex() override;
    uint32_t Id() const;
    void UpCtx(uint64_t value);
    uint64_t UpCtx() const;
    int32_t NonBlockSend(int16_t type, int16_t result, uint32_t sequence, const AccDataBufferPtr &data,
                         const AccDataBufferPtr &callbackContext) const;
    smem_acc_link_t Handle() const
    {
        return handle_;
    }

private:
    smem_acc_link_t handle_{nullptr};
};
using AccTcpLinkComplexPtr = mmc::MmcRef<AccTcpLinkComplex>;

class AccTcpRequestContext {
public:
    AccTcpRequestContext(const AccMsgHeader &header, const AccDataBufferPtr &data, const AccTcpLinkComplexPtr &link)
        : header_(header), link_(link), data_(data)
    {}
    AccTcpRequestContext(const AccTcpRequestContext &other) : header_(other.header_), link_(other.link_)
    {
        data_ = AccDataBuffer::Create(other.DataPtr(), other.DataLen());
    }
    AccTcpRequestContext &operator=(const AccTcpRequestContext &) = delete;
    int32_t Reply(int16_t result, const AccDataBufferPtr &data) const;
    void *DataPtr() const;
    uint32_t DataLen() const;
    uint32_t SeqNo() const
    {
        return header_.seqNo;
    }
    const AccMsgHeader &Header() const
    {
        return header_;
    }
    const AccTcpLinkComplexPtr &Link() const
    {
        return link_;
    }

private:
    const AccMsgHeader header_;
    const AccTcpLinkComplexPtr link_;
    AccDataBufferPtr data_;
};

using AccNewLinkHandler = std::function<int(const AccConnReq &, const AccTcpLinkComplexPtr &)>;
using AccNewReqHandler = std::function<int32_t(const AccTcpRequestContext &)>;
using AccReqSentHandler = std::function<int32_t(AccMsgSentResult, const AccMsgHeader &, const AccDataBufferPtr &)>;
using AccLinkBrokenHandler = std::function<int32_t(const AccTcpLinkComplexPtr &)>;

class AccTcpServer : public mmc::MmcReferable {
public:
    static mmc::MmcRef<AccTcpServer> Create();
    AccTcpServer();
    ~AccTcpServer() override;
    int32_t Start(const AccTcpServerOptions &options, const AccTlsOption &tls);
    void Stop();
    int32_t ConnectToPeerServer(const std::string &ip, uint16_t port, const AccConnReq &request,
                                AccTcpLinkComplexPtr &link);
    int32_t ConnectToPeerServer(const std::string &ip, uint16_t port, const AccConnReq &request, uint32_t retryTimes,
                                AccTcpLinkComplexPtr &link);
    void RegisterNewRequestHandler(int16_t type, const AccNewReqHandler &handler);
    void RegisterRequestSentHandler(int16_t type, const AccReqSentHandler &handler);
    void RegisterLinkBrokenHandler(const AccLinkBrokenHandler &handler);
    void RegisterNewLinkHandler(const AccNewLinkHandler &handler);
    void RegisterDecryptHandler(const AccDecryptHandler &handler);
    int32_t ReleaseLink(const AccTcpLinkComplexPtr &link);
    int32_t LoadDynamicLib(const std::string &path);

private:
    static int32_t OnNewLink(void *, const smem_acc_connect_request_t *, smem_acc_link_t);
    static int32_t OnRequest(void *, const smem_acc_message_v2_t *, const void *, uint32_t, smem_acc_link_t);
    static int32_t OnSent(void *, int32_t, const smem_acc_message_v2_t *, const void *, uint32_t);
    static int32_t OnBroken(void *, smem_acc_link_t);
    static int32_t OnDecrypt(void *, const char *, size_t, uint8_t *, size_t);
    smem_acc_server_t handle_{nullptr};
    AccNewLinkHandler newLink_;
    AccNewReqHandler requestHandlers_[MAX_MSG_TYPE]{};
    AccReqSentHandler sentHandlers_[MAX_MSG_TYPE]{};
    AccLinkBrokenHandler broken_;
    AccDecryptHandler decrypt_;
};
using AccTcpServerPtr = mmc::MmcRef<AccTcpServer>;
} // namespace ock::mmc_acc
namespace ock {
namespace acc = mmc_acc;
}
#endif
