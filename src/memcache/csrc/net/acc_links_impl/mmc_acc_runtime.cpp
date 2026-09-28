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
#include "mmc_acc_runtime.h"

#include <algorithm>
#include <sstream>

#include "mmc_logger.h"

namespace ock::mmc_acc {
namespace {
smem_tls_options_t MakeTls(const AccTlsOption &value)
{
    return {static_cast<uint8_t>(value.enableTls),
            value.tlsTopPath.c_str(),
            value.tlsCert.c_str(),
            value.tlsCrlPath.c_str(),
            value.tlsCaPath.c_str(),
            value.tlsCaFile.empty() ? nullptr : value.tlsCaFile.begin()->c_str(),
            value.tlsCrlFile.empty() ? nullptr : value.tlsCrlFile.begin()->c_str(),
            value.tlsPk.c_str(),
            value.tlsPkPwd.c_str()};
}
} // namespace

std::string AccMsgHeader::ToString() const
{
    std::ostringstream out;
    out << "type: " << type << ", result: " << result << ", bodyLen: " << bodyLen << ", seqNo: " << seqNo;
    return out.str();
}

AccDataBufferPtr AccDataBuffer::Create(const void *data, uint32_t size)
{
    auto buffer = mmc::MmcMakeRef<AccDataBuffer>(data, size);
    if (buffer.Get() == nullptr || buffer->data_ == nullptr) {
        return nullptr;
    }

    return buffer;
}

AccDataBufferPtr AccDataBuffer::Create(uint32_t memSize)
{
    auto buffer = mmc::MmcMakeRef<AccDataBuffer>(memSize);
    if (buffer.Get() == nullptr || buffer->data_ == nullptr) {
        return nullptr;
    }

    return buffer;
}

AccDataBuffer::AccDataBuffer(uint32_t memSize) : memSize_{memSize}, data_{nullptr}
{
    if (memSize_ <= MAX_RECV_BODY_LEN) {
        data_ = new (std::nothrow) uint8_t[memSize_];
    }
}

AccDataBuffer::AccDataBuffer(const void *data, uint32_t size) : AccDataBuffer{size}
{
    if (data_ != nullptr) {
        const uint8_t *srcPtr = static_cast<const uint8_t *>(data);
        std::copy(srcPtr, srcPtr + size, data_);
        dataSize_ = size;
    }
}

AccDataBuffer::~AccDataBuffer()
{
    delete[] data_;
    data_ = nullptr;
    memSize_ = 0;
    dataSize_ = 0;
}

bool AccDataBuffer::AllocIfNeed(uint32_t newSize) noexcept
{
    if (newSize > MAX_RECV_BODY_LEN || memSize_ > MAX_RECV_BODY_LEN) {
        return false;
    }

    if (data_ == nullptr) {
        memSize_ = std::max(memSize_, newSize);
        data_ = new (std::nothrow) uint8_t[memSize_];
        return data_ != nullptr;
    }

    if (newSize > memSize_) {
        const uint32_t targetSize = std::max(memSize_, newSize);
        if (targetSize > MAX_RECV_BODY_LEN) {
            MMC_LOG_ERROR("Failed to alloc with size:" << targetSize << ", exceeds limit:" << MAX_RECV_BODY_LEN);
            return false;
        }
        auto *newBuf = new (std::nothrow) uint8_t[targetSize];
        if (newBuf == nullptr) {
            return false;
        }
        if (dataSize_ > 0) {
            std::copy(data_, data_ + dataSize_, newBuf);
        }
        delete[] data_;
        data_ = newBuf;
        memSize_ = targetSize;
        return true;
    }

    return true;
}

uint8_t *AccDataBuffer::DataPtr() const
{
    return data_;
}

void *AccDataBuffer::DataPtrVoid() const
{
    return static_cast<void *>(data_);
}

uintptr_t AccDataBuffer::DataIntPtr() const
{
    return reinterpret_cast<uintptr_t>(data_);
}

uint32_t AccDataBuffer::DataLen() const
{
    return dataSize_;
}

uint32_t AccDataBuffer::MemSize() const
{
    return memSize_;
}

void AccDataBuffer::SetDataSize(uint32_t size)
{
    dataSize_ = size;
}

AccTcpLinkComplex::AccTcpLinkComplex(smem_acc_link_t handle, bool retain)
    : handle_(retain ? mmc::MFAccApi::RetainLink(handle) : handle)
{}

AccTcpLinkComplex::~AccTcpLinkComplex()
{
    if (handle_ != nullptr) {
        mmc::MFAccApi::ReleaseLink(handle_);
    }
}

uint32_t AccTcpLinkComplex::Id() const
{
    return mmc::MFAccApi::Id(handle_);
}

void AccTcpLinkComplex::UpCtx(uint64_t value)
{
    mmc::MFAccApi::UpCtx(handle_, value);
}

uint64_t AccTcpLinkComplex::UpCtx() const
{
    return mmc::MFAccApi::UpCtx(handle_);
}

int32_t AccTcpLinkComplex::NonBlockSend(int16_t type, int16_t result, uint32_t sequence, const AccDataBufferPtr &data,
                                        const AccDataBufferPtr &callbackContext) const
{
    return mmc::MFAccApi::NonBlockSend(handle_, type, result, sequence, data == nullptr ? nullptr : data->DataPtrVoid(),
                                       data == nullptr ? 0 : data->DataLen(),
                                       callbackContext == nullptr ? nullptr : callbackContext->DataPtrVoid(),
                                       callbackContext == nullptr ? 0 : callbackContext->DataLen());
}

int32_t AccTcpRequestContext::Reply(int16_t result, const AccDataBufferPtr &data) const
{
    return link_->NonBlockSend(header_.type, result, header_.seqNo, data, nullptr);
}

void *AccTcpRequestContext::DataPtr() const
{
    return data_ == nullptr ? nullptr : data_->DataPtrVoid();
}

uint32_t AccTcpRequestContext::DataLen() const
{
    return data_ == nullptr ? 0 : data_->DataLen();
}

AccTcpServer::AccTcpServer()
{
    if (mmc::MFAccApi::LoadLibrary() == mmc::MMC_OK) {
        handle_ = mmc::MFAccApi::CreateTcpServer();
    }
}

AccTcpServer::~AccTcpServer()
{
    if (handle_ != nullptr) {
        mmc::MFAccApi::DestroyServer(handle_);
    }
}

AccTcpServerPtr AccTcpServer::Create()
{
    auto result = mmc::MmcMakeRef<AccTcpServer>();
    return result != nullptr && result->handle_ != nullptr ? result : nullptr;
}

int32_t AccTcpServer::Start(const AccTcpServerOptions &value, const AccTlsOption &tls)
{
    smem_acc_tcp_server_options_t options{value.listenIp.c_str(),
                                          value.listenPort,
                                          value.workerCount,
                                          value.linkSendQueueSize,
                                          value.magic,
                                          value.version,
                                          static_cast<uint8_t>(value.enableListener)};
    auto tlsOptions = MakeTls(tls);
    return mmc::MFAccApi::StartTcpServer(handle_, &options, &tlsOptions);
}

void AccTcpServer::Stop()
{
    if (handle_ != nullptr) {
        mmc::MFAccApi::StopServer(handle_);
    }
}

int32_t AccTcpServer::ConnectToPeerServer(const std::string &ip, uint16_t port, const AccConnReq &value,
                                          AccTcpLinkComplexPtr &link)
{
    return ConnectToPeerServer(ip, port, value, 30, link);
}

int32_t AccTcpServer::ConnectToPeerServer(const std::string &ip, uint16_t port, const AccConnReq &value,
                                          uint32_t retryTimes, AccTcpLinkComplexPtr &link)
{
    smem_acc_connect_request_t request{value.magic, value.version, static_cast<uint8_t>(value.reconnect), value.rankId};
    smem_acc_link_t native = nullptr;
    int32_t ret = mmc::MFAccApi::ConnectToPeerServer(handle_, ip.c_str(), port, &request, retryTimes, &native);
    if (ret == ACC_OK) {
        link = mmc::MmcMakeRef<AccTcpLinkComplex>(native, false);
    }
    return ret;
}

void AccTcpServer::RegisterNewRequestHandler(int16_t type, const AccNewReqHandler &handler)
{
    if (type < MIN_MSG_TYPE || type >= MAX_MSG_TYPE || !handler || requestHandlers_[type]) {
        return;
    }
    requestHandlers_[type] = handler;
    mmc::MFAccApi::RegisterNewRequestHandler(handle_, type, OnRequest, this);
}

void AccTcpServer::RegisterRequestSentHandler(int16_t type, const AccReqSentHandler &handler)
{
    if (type < MIN_MSG_TYPE || type >= MAX_MSG_TYPE || !handler || sentHandlers_[type]) {
        return;
    }
    sentHandlers_[type] = handler;
    mmc::MFAccApi::RegisterRequestSentHandler(handle_, type, OnSent, this);
}

void AccTcpServer::RegisterLinkBrokenHandler(const AccLinkBrokenHandler &handler)
{
    if (!handler || broken_) {
        return;
    }
    broken_ = handler;
    mmc::MFAccApi::RegisterLinkBrokenHandler(handle_, OnBroken, this);
}

void AccTcpServer::RegisterNewLinkHandler(const AccNewLinkHandler &handler)
{
    if (!handler || newLink_) {
        return;
    }
    newLink_ = handler;
    mmc::MFAccApi::RegisterNewLinkHandler(handle_, OnNewLink, this);
}

void AccTcpServer::RegisterDecryptHandler(const AccDecryptHandler &handler)
{
    if (!handler || decrypt_) {
        return;
    }
    decrypt_ = handler;
    mmc::MFAccApi::RegisterDecryptHandler(handle_, OnDecrypt, this);
}

int32_t AccTcpServer::LoadDynamicLib(const std::string &path)
{
    return mmc::MFAccApi::LoadDynamicLib(handle_, path.c_str());
}

int32_t AccTcpServer::OnNewLink(void *self, const smem_acc_connect_request_t *value, smem_acc_link_t native)
{
    auto *server = static_cast<AccTcpServer *>(self);
    AccConnReq request{value->magic, value->version, static_cast<int8_t>(value->reconnect), {}, value->rank_id};
    auto link = mmc::MmcMakeRef<AccTcpLinkComplex>(native, true);
    return server->newLink_(request, link);
}

int32_t AccTcpServer::OnRequest(void *self, const smem_acc_message_v2_t *value, const void *data, uint32_t length,
                                smem_acc_link_t native)
{
    auto *server = static_cast<AccTcpServer *>(self);
    AccMsgHeader header{value->type, value->result, value->body_len, value->seq_no, value->crc};
    AccTcpRequestContext request(header, AccDataBuffer::Create(data, length),
                                 mmc::MmcMakeRef<AccTcpLinkComplex>(native, true));
    if (value->type < MIN_MSG_TYPE || value->type >= MAX_MSG_TYPE || !server->requestHandlers_[value->type]) {
        return -1;
    }
    return server->requestHandlers_[value->type](request);
}

int32_t AccTcpServer::OnSent(void *self, int32_t result, const smem_acc_message_v2_t *value, const void *callbackData,
                             uint32_t callbackDataLen)
{
    auto *server = static_cast<AccTcpServer *>(self);
    AccMsgHeader header{value->type, value->result, value->body_len, value->seq_no, value->crc};
    if (value->type < MIN_MSG_TYPE || value->type >= MAX_MSG_TYPE || !server->sentHandlers_[value->type]) {
        return -1;
    }
    return server->sentHandlers_[value->type](
        static_cast<AccMsgSentResult>(result), header,
        callbackData == nullptr ? nullptr : AccDataBuffer::Create(callbackData, callbackDataLen));
}

int32_t AccTcpServer::OnBroken(void *self, smem_acc_link_t native)
{
    auto *server = static_cast<AccTcpServer *>(self);
    return server->broken_(mmc::MmcMakeRef<AccTcpLinkComplex>(native, true));
}

int32_t AccTcpServer::OnDecrypt(void *self, const char *cipher, size_t cipherLen, uint8_t *plain, size_t plainLen)
{
    auto *server = static_cast<AccTcpServer *>(self);
    return server == nullptr || !server->decrypt_
               ? -1
               : server->decrypt_(cipher, cipherLen, reinterpret_cast<char *>(plain), plainLen);
}

} // namespace ock::mmc_acc
