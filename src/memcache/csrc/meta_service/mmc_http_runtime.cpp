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
#include "mmc_http_runtime.h"
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

AccHttpRequestContext::AccHttpRequestContext(smem_acc_http_request_t request) : request_(request) {}

const void *AccHttpRequestContext::BodyPtr() const
{
    uint32_t ignored = 0;
    return mmc::MFAccApi::Body(request_, &ignored);
}

uint32_t AccHttpRequestContext::BodyLen() const
{
    uint32_t length = 0;
    mmc::MFAccApi::Body(request_, &length);
    return length;
}

bool AccHttpRequestContext::Param(const std::string &name, std::string &value) const
{
    const char *param = mmc::MFAccApi::GetParam(request_, name.c_str());
    if (param == nullptr) {
        return false;
    }
    value = param;
    return true;
}

std::multimap<std::string, std::string> AccHttpRequestContext::Params() const
{
    std::multimap<std::string, std::string> params;
    mmc::MFAccApi::VisitParams(
        request_,
        [](void *context, const char *name, const char *value) {
            auto *result = static_cast<std::multimap<std::string, std::string> *>(context);
            result->emplace(name, value);
        },
        &params);
    return params;
}

int32_t AccHttpRequestContext::Reply(AccHttpStatusCode status, const std::string &contentType, const std::string &body)
{
    return mmc::MFAccApi::Reply(request_, static_cast<uint16_t>(status), contentType.c_str(), body.data(), body.size());
}

AccHttpServer::AccHttpServer()
{
    if (mmc::MFAccApi::LoadLibrary() == mmc::MMC_OK) {
        handle_ = mmc::MFAccApi::CreateHttpServer();
    }
}

AccHttpServer::~AccHttpServer()
{
    if (handle_ != nullptr) {
        mmc::MFAccApi::DestroyServer(handle_);
    }
}

AccHttpServerPtr AccHttpServer::Create()
{
    auto result = mmc::MmcMakeRef<AccHttpServer>();
    return result != nullptr && result->handle_ != nullptr ? result : nullptr;
}

void AccHttpServer::RegisterHttpHandler(AccHttpMethod method, const std::string &path, const AccHttpReqHandler &handler)
{
    auto route = std::make_unique<Route>();
    route->handler = handler;
    if (mmc::MFAccApi::RegisterHttpHandler(handle_, static_cast<uint8_t>(method), path.c_str(), OnRequest,
                                           route.get()) == ACC_OK) {
        routes_.emplace_back(std::move(route));
    }
}

void AccHttpServer::RegisterLinkBrokenHandler(const AccLinkBrokenHandler &handler)
{
    if (!handler || broken_) {
        return;
    }
    broken_ = handler;
    mmc::MFAccApi::RegisterLinkBrokenHandler(handle_, OnBroken, this);
}

void AccHttpServer::RegisterDecryptHandler(const AccDecryptHandler &handler)
{
    if (!handler || decrypt_) {
        return;
    }
    decrypt_ = handler;
    mmc::MFAccApi::RegisterDecryptHandler(handle_, OnDecrypt, this);
}

int32_t AccHttpServer::LoadDynamicLib(const std::string &path)
{
    return mmc::MFAccApi::LoadDynamicLib(handle_, path.c_str());
}

int32_t AccHttpServer::Start(const AccHttpServerOptions &value, const AccTlsOption &tls)
{
    smem_acc_http_server_options_t options{value.listenIp.c_str(), value.listenPort, value.workerCount,
                                           value.maxBodySize};
    auto tlsOptions = MakeTls(tls);
    return mmc::MFAccApi::StartHttpServer(handle_, &options, &tlsOptions);
}

void AccHttpServer::Stop()
{
    if (handle_ != nullptr) {
        mmc::MFAccApi::StopServer(handle_);
    }
}

int32_t AccHttpServer::OnRequest(void *context, smem_acc_http_request_t native)
{
    auto *route = static_cast<Route *>(context);
    AccHttpRequestContext request(native);
    return route->handler(request);
}

int32_t AccHttpServer::OnBroken(void *context, smem_acc_link_t native)
{
    auto *server = static_cast<AccHttpServer *>(context);
    return server == nullptr || !server->broken_ ? -1
                                                 : server->broken_(mmc::MmcMakeRef<AccTcpLinkComplex>(native, true));
}

int32_t AccHttpServer::OnDecrypt(void *context, const char *cipher, size_t cipherLen, uint8_t *plain, size_t plainLen)
{
    auto *server = static_cast<AccHttpServer *>(context);
    return server == nullptr || !server->decrypt_
               ? -1
               : server->decrypt_(cipher, cipherLen, reinterpret_cast<char *>(plain), plainLen);
}

} // namespace ock::mmc_acc
