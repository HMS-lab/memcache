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
#ifndef MEMCACHE_MMC_HTTP_RUNTIME_H
#define MEMCACHE_MMC_HTTP_RUNTIME_H
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "mmc_acc_runtime.h"
namespace ock::mmc_acc {
enum class AccHttpMethod : uint8_t { GET, HEAD, POST, PUT, DELETE, CONNECT, OPTIONS, TRACE, PATCH };
enum class AccHttpStatusCode : uint16_t { OK = 200 };
class AccHttpRequestContext {
public:
    explicit AccHttpRequestContext(smem_acc_http_request_t request);
    const void *BodyPtr() const;
    uint32_t BodyLen() const;
    bool Param(const std::string &name, std::string &value) const;
    std::multimap<std::string, std::string> Params() const;
    int32_t Reply(AccHttpStatusCode status, const std::string &contentType, const std::string &body);

private:
    smem_acc_http_request_t request_;
};
using AccHttpReqHandler = std::function<int32_t(AccHttpRequestContext &)>;
struct AccHttpServerOptions {
    std::string listenIp;
    uint16_t listenPort{9966};
    uint16_t workerCount{2};
    bool enableListener{true};
    size_t maxBodySize{10 * 1024 * 1024};
};
class AccHttpServer : public mmc::MmcReferable {
public:
    static mmc::MmcRef<AccHttpServer> Create();
    AccHttpServer();
    ~AccHttpServer() override;
    void RegisterHttpHandler(AccHttpMethod method, const std::string &path, const AccHttpReqHandler &handler);
    void RegisterLinkBrokenHandler(const AccLinkBrokenHandler &handler);
    void RegisterDecryptHandler(const AccDecryptHandler &handler);
    int32_t LoadDynamicLib(const std::string &path);
    int32_t Start(const AccHttpServerOptions &options, const AccTlsOption &tls);
    void Stop();

private:
    struct Route {
        AccHttpReqHandler handler;
    };
    static int32_t OnRequest(void *, smem_acc_http_request_t);
    static int32_t OnBroken(void *, smem_acc_link_t);
    static int32_t OnDecrypt(void *, const char *, size_t, uint8_t *, size_t);
    smem_acc_server_t handle_{nullptr};
    std::vector<std::unique_ptr<Route>> routes_;
    AccLinkBrokenHandler broken_;
    AccDecryptHandler decrypt_;
};
using AccHttpServerPtr = mmc::MmcRef<AccHttpServer>;
} // namespace ock::mmc_acc
#endif
